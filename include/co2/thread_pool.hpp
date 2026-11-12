#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "co2/contract.hpp"
#include "co2/detail/ring_queue.hpp"
#include "co2/scheduler.hpp"

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#endif

// co2 执行层：工作窃取线程池。
//
//   每个工作线程一个固定容量的本地 FIFO 队列（owner 在尾部 push；owner 与 thief 都从
//   头部 CAS 取），外部线程提交的协程与本地队列溢出的协程进入一个互斥锁保护的全局
//   队列。工作线程按 本地 → 全局 → 窃取 的顺序找活，找不到就停车；每 64 次循环强制先
//   看一眼全局队列，保证外部提交不会被本地工作饿死。
//
//   本地队列是 FIFO 而不是 Chase-Lev 的 LIFO：协程在工作线程上 CO2_AWAIT(scheduleOn(
//   pool)) 是"让步"，排在它前面的本地工作必须先跑；LIFO 会让它立刻被同一线程重新
//   弹出，让步形同虚设。这与 Go 的 runq、Tokio 的 local queue 一致。
//
// 工作线程有三种状态：运行（在跑某个协程）、搜索（本地与全局都空，正在自旋着偷）、
// 停车（睡在条件变量上）。两个计数描述后两种：searching 与 idle。
//
//   唤醒节流（Go 的 nmspinning、Tokio 的 num_searching 是同一个想法）：提交方 push 之后
//   只在"没有任何线程在搜索、且有线程停车"时才叫醒一个，被叫醒的线程以搜索状态醒来。
//   已经有人在搜索时，新工作由它发现，不必再叫醒别人——否则池不饱和时每次 schedule 都
//   要拿一次锁、发一次 futex 唤醒，被叫醒的线程多半扑空。为了让一批工作仍能招来足够多
//   的线程，最后一个搜索者找到活、离开搜索状态时，若还看得到别的工作，就再叫醒一个。
//
//   停车前自旋：搜索者先偷若干轮（pause 指数退避，最后让出几次时间片）才去停车。紧接
//   着到来的工作省掉一次睡眠/唤醒的往返，也让"有人在搜索"这个节流条件真正常常成立。
//
//   窃取：一次搬走受害者队列的一半。只剩一项的队列先不偷——它的主人多半正要自己取走
//   （工作线程上的 scheduleOn 就是这种形态），偷走只会让这个任务在两个核之间来回搬；
//   但下一轮再看时队首还原地不动，说明主人被当前的协程占住了，这时才偷。
//
//   不丢唤醒（Dekker）：提交方的顺序是 push → 读 searching → 读 idle；离开搜索状态去
//   停车的线程是 idle++ → searching-- → 复查所有队列。这些都是 seq_cst 操作，处在同一个
//   全序里：提交方读到 searching == 0 且 idle == 0 时，之后才登记停车的线程一定在复查里
//   看到这次 push；提交方读到 searching != 0 时，那个搜索者要么找到它，要么在停车前的
//   复查里看到它。最后一个搜索者带着活离开时同理：先 searching--，再看还有没有别的活。
//
//   "有人在搜索"是一份责任，不只是一个计数：指望着同一个搜索者的提交可能不止一份，而
//   它一次只拿走一份。所以搜索者离开搜索状态的每一条路都要把剩下的交代清楚——带着活
//   离开时接力叫醒下一个（stopSearching）；去停车却在复查里看到了活，就重新记入
//   searching、以搜索者的身份回去找（park），拿到之后照样接力。漏掉后一条，剩下的工作
//   旁边就只有睡着的线程（tests/thread_pool_tests.cpp 的三个 everyWorkerIsRecruited…
//   用例扫描提交时机来覆盖它）。
//
// 销毁：析构函数请求停止并 join；工作线程把已排队的工作全部跑完才退出（跑的过程中
// 新排入的也算）。析构期间从外部线程再 schedule 是使用者的错误。

namespace co2 {
namespace detail {

// 单生产者多消费者的固定容量 FIFO 环：owner 在 tail 端 push，任何线程（owner 或 thief）
// 在 head 端以 CAS 认领。固定容量避开了循环数组扩容后旧数组的回收问题；满了由线程池
// 溢出到全局队列。
struct LocalQueue {
    static constexpr std::int64_t Capacity = 1024; // 2 的幂
    static constexpr std::int64_t Mask = Capacity - 1;

    enum class TakeStatus { Empty, Abort, Taken };

    struct TakeResult {
        TakeStatus status;
        void* item;
    };

    // 某一时刻的近似观察：队首位置与元素个数。
    struct Snapshot {
        std::int64_t head;
        std::int64_t size;
    };

    LocalQueue() noexcept : head{0}, tail{0} {}

    LocalQueue(LocalQueue const&) = delete;
    LocalQueue& operator=(LocalQueue const&) = delete;

    // owner：满则返回 false。tail 的存储把元素（以及元素指向的协程帧里在 push 之前写下的
    // 一切）发布给 take() / stealInto() 里的 acquire 读取。它是 seq_cst 的：停车协议要求
    // "push 可见"与提交方随后读到的 searching / idle 计数处在同一个全序里（见 ThreadPool）。
    bool push(void* const item) noexcept {
        auto const t = tail.load(std::memory_order_relaxed);
        auto const h = head.load(std::memory_order_acquire);
        if (t - h >= Capacity) return false;
        slot(t).store(item, std::memory_order_relaxed);
        tail.store(t + 1, std::memory_order_seq_cst);
        return true;
    }

    // 任何线程：认领队首。Abort 表示与其他消费者竞争失败，可以重试。
    //
    // 槽位复用是安全的：生产者只在 t < head + Capacity 时写 slot(t)，而一个拿着过期
    // head 的消费者读到的槽位若已被覆盖，它的 CAS 必然失败，读到的值被丢弃。
    TakeResult take() noexcept {
        auto h = head.load(std::memory_order_acquire);
        auto const t = tail.load(std::memory_order_acquire);
        if (h >= t) return TakeResult{TakeStatus::Empty, nullptr};
        void* const item = slot(h).load(std::memory_order_relaxed);
        if (not head.compare_exchange_strong(h, h + 1, std::memory_order_acq_rel,
                                             std::memory_order_relaxed))
            return TakeResult{TakeStatus::Abort, nullptr};
        return TakeResult{TakeStatus::Taken, item};
    }

    // 窃取者（`destination` 的 owner）：一次取走本队列的一半（向上取整）。最老的一项返回给
    // 调用方立刻运行，其余按原顺序进 `destination`。逐项来回偷的话，受害者有积压时每一项
    // 都要付一次跨核的 CAS；一次搬走一半，窃取者接下来就在自己的队列上干活。
    //
    // 先把元素抄进 `destination` 尚未发布的槽位，再 CAS 本队列的 head 认领；CAS 失败说明
    // 有别人动过 head，抄过去的内容作废（没有发布，没人看得到）。读到被生产者覆盖的槽位
    // 也只可能发生在 head 已经前进之后，同样以 CAS 失败收场。
    TakeResult stealInto(LocalQueue& destination) noexcept {
        auto h = head.load(std::memory_order_acquire);
        auto const t = tail.load(std::memory_order_acquire);
        auto const available = t - h;
        if (available <= 0) return TakeResult{TakeStatus::Empty, nullptr};

        auto count = available - available / 2;
        if (count > Capacity / 2) count = Capacity / 2;
        auto const destinationTail = destination.tail.load(std::memory_order_relaxed);
        auto const destinationHead = destination.head.load(std::memory_order_acquire);
        auto const room = Capacity - (destinationTail - destinationHead);
        if (count - 1 > room) count = room + 1;

        void* const first = slot(h).load(std::memory_order_relaxed);
        for (auto index = std::int64_t{1}; index != count; ++index) {
            destination.slot(destinationTail + index - 1)
                .store(slot(h + index).load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
        }
        if (not head.compare_exchange_strong(h, h + count, std::memory_order_acq_rel,
                                             std::memory_order_relaxed))
            return TakeResult{TakeStatus::Abort, nullptr};
        if (count > 1)
            destination.tail.store(destinationTail + count - 1,
                                   std::memory_order_seq_cst);
        return TakeResult{TakeStatus::Taken, first};
    }

    // 任意线程：停车协议里的复查。seq_cst 读取与 push 的 seq_cst 存储配对。
    bool looksEmpty() const noexcept {
        return head.load(std::memory_order_seq_cst) >=
               tail.load(std::memory_order_seq_cst);
    }

    // 任意线程：窃取前的近似观察。head 只增不减，所以两次观察到相同的 head 且队列非空，
    // 说明这期间没有任何人从这个队列取走过东西。
    Snapshot snapshot() const noexcept {
        auto const h = head.load(std::memory_order_acquire);
        auto const t = tail.load(std::memory_order_acquire);
        return Snapshot{h, t - h};
    }

    // head 与 tail 相隔 64 字节：消费者频繁 CAS head，owner 频繁写 tail，不能共享缓存
    // 行。用填充而不是 alignas：C++14 的 new 不保证超过默认对齐的分配。
    std::atomic<std::int64_t> head;
    unsigned char padAfterHead[64U - sizeof(std::atomic<std::int64_t>)];
    std::atomic<std::int64_t> tail;
    unsigned char padAfterTail[64U - sizeof(std::atomic<std::int64_t>)];
    std::atomic<void*> buffer[Capacity];

  private:
    std::atomic<void*>& slot(std::int64_t const index) noexcept {
        return buffer[static_cast<std::size_t>(index & Mask)];
    }
};

// 忙等循环里的一拍：告诉 CPU 这里在自旋（省电，并把执行资源让给同核的另一个硬件线程）。
inline void cpuRelax() noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    __asm__ __volatile__("yield");
#else
    std::this_thread::yield();
#endif
}

} // namespace detail

struct ThreadPool final : Scheduler {
    static unsigned defaultThreadCount() noexcept {
        auto const detected = std::thread::hardware_concurrency();
        return detected == 0U ? 1U : detected;
    }

    explicit ThreadPool(unsigned const threadCount = defaultThreadCount()) {
        CO2_CONTRACT_CHECK(threadCount != 0U);
        workers.reserve(threadCount);
        for (auto index = 0U; index != threadCount; ++index)
            workers.push_back(
                std::unique_ptr<Worker>{new Worker{*this, index, threadCount}});
        try {
            for (auto& worker : workers) {
                auto* const target = worker.get();
                target->thread = std::thread{[this, target] { workerMain(*target); }};
            }
        } catch (...) {
            requestStop();
            joinWorkers();
            throw;
        }
    }

    ~ThreadPool() override {
        requestStop();
        joinWorkers();
    }

    void schedule(coroutine_handle<> const coroutine) noexcept override {
        CO2_CONTRACT_CHECK(coroutine);
        auto* const item = coroutine.address();
        auto* const worker = currentWorkerSlot();
        if (worker == nullptr || worker->pool != this || not worker->queue.push(item))
            pushGlobal(item);
        notifyWork();
    }

    unsigned threadCount() const noexcept {
        return static_cast<unsigned>(workers.size());
    }

    // 当前线程是否是本线程池的工作线程。
    bool isWorkerThread() const noexcept {
        auto* const worker = currentWorkerSlot();
        return worker != nullptr && worker->pool == this;
    }

  private:
    // 停车前的搜索轮数：前 spinRounds 轮之间以 pause 指数退避（1、2、4…… 拍），之后每轮
    // 让出一次时间片。总共几微秒到几十微秒，远短于一次睡眠/唤醒的往返。
    enum : unsigned { spinRounds = 10U, yieldRounds = 2U };

    struct Worker {
        Worker(ThreadPool& pool_, unsigned const index_, unsigned const workerCount)
            : pool{&pool_}, index{index_}, rng{0x9E3779B9U * (index_ + 1U)},
              seenHead(workerCount, std::int64_t{-1}) {}

        detail::LocalQueue queue;
        ThreadPool* pool;
        unsigned index;
        std::uint32_t rng;
        unsigned tick{};
        // 本线程是否占着 searching 计数里的一份。只有本线程读写。
        bool searching{};
        // 下一次搜索从第几轮开始，见 search()。
        unsigned searchStart{};
        // 上一次看到某个受害者"只剩一项"时它的队首位置，见 steal()。
        std::vector<std::int64_t> seenHead;
        std::thread thread;
    };

    // 提交方每次 schedule 都读这两个计数，只有工作线程在 运行 / 搜索 / 停车 之间切换时
    // 才写。前后各留一条缓存行，免得满载时被相邻成员的写入拖累。
    struct StateCounters {
        unsigned char padBefore[64];
        std::atomic<unsigned> searching{0U};
        std::atomic<unsigned> idle{0U};
        unsigned char padAfter[64U - 2U * sizeof(std::atomic<unsigned>)];
    };

    static Worker*& currentWorkerSlot() noexcept {
        thread_local Worker* current = nullptr;
        return current;
    }

    void workerMain(Worker& worker) noexcept {
        currentWorkerSlot() = &worker;
        for (;;) {
            if (auto* const item = findWork(worker)) {
                worker.searchStart = 0U;
                if (worker.searching) stopSearching(worker);
                coroutine_handle<>::from_address(item).resume();
                continue;
            }
            if (not park(worker)) break;
        }
        currentWorkerSlot() = nullptr;
    }

    void* findWork(Worker& worker) noexcept {
        // 每 64 次先看全局队列：外部提交不能被本地工作无限推后。
        if ((++worker.tick & 63U) == 0U) {
            if (auto* const item = popGlobal()) return item;
        }
        if (auto* const item = takeLocal(worker)) return item;
        if (auto* const item = popGlobal()) return item;
        return search(worker);
    }

    // owner 从自己的队列取：与 thief 竞争失败就重试，直到取到或确认为空。
    static void* takeLocal(Worker& worker) noexcept {
        for (;;) {
            auto const result = worker.queue.take();
            if (result.status != detail::LocalQueue::TakeStatus::Abort)
                return result.item;
        }
    }

    // 本地与全局都空：进入搜索状态，偷若干轮。返回空时仍处在搜索状态，由 park() 结束它
    // ——那里的顺序（先登记停车、再退出搜索、最后复查队列）是不丢唤醒的关键。
    void* search(Worker& worker) noexcept {
        if (not worker.searching) {
            worker.searching = true;
            counters.searching.fetch_add(1U, std::memory_order_seq_cst);
        }
        for (auto round = worker.searchStart; round != spinRounds + yieldRounds; ++round) {
            if (auto* const item = steal(worker)) return item;
            if (auto* const item = popGlobal()) return item;
            if (stopping.load(std::memory_order_relaxed)) break;
            relax(round);
        }
        // 空手而归。如果紧接着的停车复查看到了工作却偷不走（别的线程队列里那唯一的一项，
        // 主人自己在消化），本线程会马上回到这里；那时不再从最短的退避重新开始，否则它会
        // 以最高频率反复读主人的队列头尾，把正在干活的线程的缓存行来回拖。拿到活或被
        // 唤醒时复位。
        worker.searchStart = spinRounds - 1U;
        return nullptr;
    }

    static void relax(unsigned const round) noexcept {
        if (round >= spinRounds) {
            std::this_thread::yield();
            return;
        }
        for (auto beat = 0U; beat != (1U << round); ++beat)
            detail::cpuRelax();
    }

    // 带着活离开搜索状态。自己是最后一个搜索者时：这期间到来的工作都指望着"有人在搜索"
    // 而没有叫醒任何人，所以先退出计数、再看还有没有别的活，有就接力叫醒一个。
    void stopSearching(Worker& worker) noexcept {
        worker.searching = false;
        if (counters.searching.fetch_sub(1U, std::memory_order_seq_cst) == 1U &&
            hasVisibleWork())
            notifyWork();
    }

    void* steal(Worker& thief) noexcept {
        auto const count = workers.size();
        if (count < 2U) return nullptr;
        auto const start = static_cast<std::size_t>(nextRandom(thief)) % count;
        for (auto offset = std::size_t{}; offset != count; ++offset) {
            auto& victim = *workers[(start + offset) % count];
            if (&victim == &thief) continue;
            auto const seen = victim.queue.snapshot();
            if (seen.size <= 0) continue;
            if (seen.size == 1 && thief.seenHead[victim.index] != seen.head) {
                // 只剩一项，而且队首自上次看过之后动过（或者这是第一次看到）：主人多半
                // 正要自己取走它。记下队首；下一轮它还原地不动，才说明主人被占住了。
                thief.seenHead[victim.index] = seen.head;
                continue;
            }
            auto const result = victim.queue.stealInto(thief.queue);
            if (result.status == detail::LocalQueue::TakeStatus::Taken)
                return result.item;
        }
        return nullptr;
    }

    // 找不到工作时停车。返回 false 表示线程池正在停止且没有剩余工作，线程应退出。
    bool park(Worker& worker) noexcept {
        std::unique_lock<std::mutex> lock{parkMutex};
        // 顺序是协议的一部分：先登记停车，再退出搜索，最后复查队列。
        counters.idle.fetch_add(1U, std::memory_order_seq_cst);
        if (worker.searching) {
            worker.searching = false;
            counters.searching.fetch_sub(1U, std::memory_order_seq_cst);
        }
        if (hasVisibleWork()) {
            // 不睡，而且要以搜索者的身份回去。本线程可能是最后一个搜索者：这期间到来的
            // 提交看到"有人在搜索"就没有叫醒别人，而它们可能不止一份。只有重新记入
            // searching，本线程拿到其中一份之后才会经 stopSearching() 接力叫醒下一个；
            // 否则剩下的工作旁边就只有睡着的线程。
            worker.searching = true;
            counters.searching.fetch_add(1U, std::memory_order_seq_cst);
            counters.idle.fetch_sub(1U, std::memory_order_seq_cst);
            return true;
        }
        if (stopping.load(std::memory_order_relaxed)) {
            counters.idle.fetch_sub(1U, std::memory_order_seq_cst);
            return false;
        }
        parkCv.wait(lock, [this] {
            return pendingWakeups != 0U || stopping.load(std::memory_order_relaxed);
        });
        if (pendingWakeups != 0U) {
            // 叫醒本线程的一方已经替它在 searching 里记了一份。
            --pendingWakeups;
            worker.searching = true;
        }
        worker.searchStart = 0U; // 睡过一觉：有新工作，从头快速扫描
        counters.idle.fetch_sub(1U, std::memory_order_seq_cst);
        return true;
    }

    // 停车协议与"最后一个搜索者"用的复查：seq_cst 读取，与 push 的 seq_cst 存储配对。
    bool hasVisibleWork() const noexcept {
        if (globalCount.load(std::memory_order_seq_cst) != 0U) return true;
        for (auto const& worker : workers)
            if (not worker->queue.looksEmpty()) return true;
        return false;
    }

    // 有新工作：没有任何线程在搜索、且有线程停车时，叫醒一个。调用方必须已经让这份工作
    // 可见（seq_cst）。
    void notifyWork() noexcept {
        if (counters.searching.load(std::memory_order_seq_cst) != 0U) return;
        if (counters.idle.load(std::memory_order_seq_cst) == 0U) return;
        wakeOne();
    }

    void wakeOne() noexcept {
        std::lock_guard<std::mutex> lock{parkMutex};
        // 锁内复查：唤醒令牌只在锁内发放，并发的提交方不会为同一批工作叫醒一串线程。
        if (counters.searching.load(std::memory_order_relaxed) != 0U) return;
        // 锁内的 idle 就是睡在条件变量上的线程数；每人至多一张令牌。
        if (pendingWakeups >= counters.idle.load(std::memory_order_relaxed)) return;
        ++pendingWakeups;
        counters.searching.fetch_add(1U, std::memory_order_seq_cst);
        parkCv.notify_one();
    }

    void pushGlobal(void* const item) noexcept {
        std::lock_guard<std::mutex> lock{globalMutex};
        globalQueue.push(item);
        globalCount.store(globalQueue.size(), std::memory_order_seq_cst);
    }

    void* popGlobal() noexcept {
        // 长度提示：全局队列为空是常态，不为它拿锁。这里读漏了也无妨——停车前的复查用
        // seq_cst 再读一次。
        if (globalCount.load(std::memory_order_relaxed) == 0U) return nullptr;
        std::lock_guard<std::mutex> lock{globalMutex};
        if (globalQueue.empty()) return nullptr;
        auto* const item = globalQueue.pop();
        globalCount.store(globalQueue.size(), std::memory_order_seq_cst);
        return item;
    }

    void requestStop() noexcept {
        {
            std::lock_guard<std::mutex> lock{parkMutex};
            stopping.store(true, std::memory_order_relaxed);
        }
        parkCv.notify_all();
    }

    void joinWorkers() noexcept {
        for (auto& worker : workers)
            if (worker->thread.joinable()) worker->thread.join();
    }

    static std::uint32_t nextRandom(Worker& worker) noexcept {
        auto x = worker.rng;
        x ^= x << 13U;
        x ^= x >> 17U;
        x ^= x << 5U;
        worker.rng = x;
        return x;
    }

    std::vector<std::unique_ptr<Worker>> workers;
    StateCounters counters;
    // 全局队列的长度：push / pop 在锁内更新，找活的线程不拿锁先看它。
    std::atomic<std::size_t> globalCount{0U};
    std::mutex globalMutex;
    detail::RingQueue<void*> globalQueue;
    std::mutex parkMutex;
    std::condition_variable parkCv;
    std::size_t pendingWakeups{}; // 已发出、尚未被领走的唤醒令牌；parkMutex 保护
    std::atomic<bool> stopping{false};
};

} // namespace co2
