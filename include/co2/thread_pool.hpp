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
//   停车/唤醒用 Dekker 式协议而不是共享计数器：提交方 push 之后放 seq_cst 栅栏再读
//   idleWorkers；停车方先加 idleWorkers、放栅栏、再复查所有队列。二者总有一方看到另
//   一方，热路径上没有任何共享的 RMW。
//
// 销毁：析构函数请求停止并 join；工作线程把已排队的工作全部跑完才退出（跑的过程中
// 新排入的也算）。析构期间从外部线程再 schedule 是使用者的错误。

namespace co2 {
namespace detail {

// 单生产者多消费者的固定容量 FIFO 环：owner 在 tail 端 push，任何线程（owner 或 thief）
// 在 head 端以 CAS 认领。固定容量避开了循环数组扩容后旧数组的回收问题；满了由线程池
// 溢出到全局队列。
struct LocalQueue {
    static constexpr std::size_t Capacity = 1024U; // 2 的幂
    static constexpr std::size_t Mask = Capacity - 1U;

    enum class TakeStatus { Empty, Abort, Taken };

    struct TakeResult {
        TakeStatus status;
        void* item;
    };

    LocalQueue() noexcept : head{0}, tail{0} {}

    LocalQueue(LocalQueue const&) = delete;
    LocalQueue& operator=(LocalQueue const&) = delete;

    // owner：满则返回 false。tail 的 release 存储把元素（以及元素指向的协程帧里在 push
    // 之前写下的一切）发布给 take() 里的 acquire 读取。
    bool push(void* const item) noexcept {
        auto const t = tail.load(std::memory_order_relaxed);
        auto const h = head.load(std::memory_order_acquire);
        if (t - h >= static_cast<std::int64_t>(Capacity)) return false;
        slot(t).store(item, std::memory_order_relaxed);
        tail.store(t + 1, std::memory_order_release);
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

    // 任意线程的近似观察，用于停车前的复查。
    bool looksEmpty() const noexcept {
        return head.load(std::memory_order_relaxed) >=
               tail.load(std::memory_order_relaxed);
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
        return buffer[static_cast<std::size_t>(index) & Mask];
    }
};

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
            workers.push_back(std::unique_ptr<Worker>{new Worker{*this, index}});
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
        if (worker != nullptr && worker->pool == this) {
            if (not worker->queue.push(item)) pushGlobal(item);
        } else {
            pushGlobal(item);
        }
        // Dekker：先让 push 对停车方的复查可见，再看是否有人正在停车。
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (idleWorkers.load(std::memory_order_seq_cst) != 0U) wakeOne();
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
    struct Worker {
        Worker(ThreadPool& pool_, unsigned const index_) noexcept
            : pool{&pool_}, index{index_}, rng{0x9E3779B9U * (index_ + 1U)} {}

        detail::LocalQueue queue;
        ThreadPool* pool;
        unsigned index;
        std::uint32_t rng;
        unsigned tick{};
        std::thread thread;
    };

    static Worker*& currentWorkerSlot() noexcept {
        thread_local Worker* current = nullptr;
        return current;
    }

    void workerMain(Worker& worker) noexcept {
        currentWorkerSlot() = &worker;
        for (;;) {
            if (auto* const item = findWork(worker)) {
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
        return steal(worker);
    }

    // owner 从自己的队列取：与 thief 竞争失败就重试，直到取到或确认为空。
    static void* takeLocal(Worker& worker) noexcept {
        for (;;) {
            auto const result = worker.queue.take();
            if (result.status != detail::LocalQueue::TakeStatus::Abort)
                return result.item;
        }
    }

    void* steal(Worker& thief) noexcept {
        auto const count = workers.size();
        if (count < 2U) return nullptr;
        for (auto round = 0; round != 2; ++round) {
            auto retry = false;
            auto const start = static_cast<std::size_t>(nextRandom(thief)) % count;
            for (auto offset = std::size_t{}; offset != count; ++offset) {
                auto& victim = *workers[(start + offset) % count];
                if (&victim == &thief) continue;
                auto const result = victim.queue.take();
                if (result.status == detail::LocalQueue::TakeStatus::Taken)
                    return result.item;
                if (result.status == detail::LocalQueue::TakeStatus::Abort)
                    retry = true;
            }
            if (not retry) break;
        }
        return nullptr;
    }

    // 找不到工作时停车。返回 false 表示线程池正在停止且没有剩余工作，线程应退出。
    bool park(Worker&) noexcept {
        std::unique_lock<std::mutex> lock{parkMutex};
        idleWorkers.fetch_add(1U, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (hasVisibleWork()) {
            idleWorkers.fetch_sub(1U, std::memory_order_seq_cst);
            return true;
        }
        if (stopping) {
            idleWorkers.fetch_sub(1U, std::memory_order_seq_cst);
            return false;
        }
        parkCv.wait(lock, [this] { return pendingWakeups != 0U || stopping; });
        if (pendingWakeups != 0U) --pendingWakeups;
        idleWorkers.fetch_sub(1U, std::memory_order_seq_cst);
        return true;
    }

    bool hasVisibleWork() noexcept {
        for (auto& worker : workers)
            if (not worker->queue.looksEmpty()) return true;
        std::lock_guard<std::mutex> lock{globalMutex};
        return not globalQueue.empty();
    }

    void wakeOne() noexcept {
        std::lock_guard<std::mutex> lock{parkMutex};
        if (pendingWakeups < workers.size()) ++pendingWakeups;
        parkCv.notify_one();
    }

    void pushGlobal(void* const item) noexcept {
        std::lock_guard<std::mutex> lock{globalMutex};
        globalQueue.push(item);
    }

    void* popGlobal() noexcept {
        std::lock_guard<std::mutex> lock{globalMutex};
        if (globalQueue.empty()) return nullptr;
        return globalQueue.pop();
    }

    void requestStop() noexcept {
        {
            std::lock_guard<std::mutex> lock{parkMutex};
            stopping = true;
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
    std::mutex globalMutex;
    detail::RingQueue<void*> globalQueue;
    std::mutex parkMutex;
    std::condition_variable parkCv;
    std::size_t pendingWakeups{};
    bool stopping{};
    std::atomic<unsigned> idleWorkers{0U};
};

} // namespace co2
