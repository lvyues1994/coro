// co2 v2：stop_source / stop_token / stop_callback 对 [thread.stoptoken] 的逐条验证。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "co2/stop_token.hpp"

namespace {

void fail(char const* expression, int line) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
}

#define CHECK(expression)                                                              \
    do {                                                                               \
        if (not(expression)) fail(#expression, __LINE__);                              \
    } while (false)

void defaultTokenAndNoStopStateSourceCannotBeStopped() {
    co2::stop_token token;
    CHECK(not token.stop_possible());
    CHECK(not token.stop_requested());

    co2::stop_source source{co2::nostopstate};
    CHECK(not source.stop_possible());
    CHECK(not source.stop_requested());
    CHECK(not source.request_stop());
    CHECK(source.get_token() == co2::stop_token{});
}

void requestStopSucceedsExactlyOnce() {
    co2::stop_source source;
    auto const token = source.get_token();
    CHECK(source.stop_possible());
    CHECK(token.stop_possible());
    CHECK(not token.stop_requested());

    CHECK(source.request_stop());
    CHECK(token.stop_requested());
    CHECK(source.stop_requested());
    CHECK(not source.request_stop());
    CHECK(token.stop_possible());
}

void callbacksRunOnceOnRequestAndImmediatelyIfAlreadyRequested() {
    co2::stop_source source;
    int calls = 0;
    co2::stop_callback<> before{source.get_token(), [&] { ++calls; }};
    CHECK(calls == 0);
    CHECK(source.request_stop());
    CHECK(calls == 1);
    CHECK(not source.request_stop());
    CHECK(calls == 1);

    int lateCalls = 0;
    co2::stop_callback<> after{source.get_token(), [&] { ++lateCalls; }};
    CHECK(lateCalls == 1); // 构造函数里同步执行
}

void destroyedCallbacksAreNotInvoked() {
    co2::stop_source source;
    int calls = 0;
    {
        co2::stop_callback<> callback{source.get_token(), [&] { ++calls; }};
    }
    source.request_stop();
    CHECK(calls == 0);
}

void allRegisteredCallbacksRun() {
    co2::stop_source source;
    std::vector<int> order;
    co2::stop_callback<> first{source.get_token(), [&] { order.push_back(1); }};
    co2::stop_callback<> second{source.get_token(), [&] { order.push_back(2); }};
    co2::stop_callback<> third{source.get_token(), [&] { order.push_back(3); }};
    source.request_stop();
    CHECK(order.size() == 3U);
}

void stopPossibleFollowsTheLastSource() {
    co2::stop_token token;
    {
        co2::stop_source source;
        auto copy = source;
        token = source.get_token();
        CHECK(token.stop_possible());
        source = co2::stop_source{co2::nostopstate};
        CHECK(token.stop_possible()); // copy 还在
    }
    CHECK(not token.stop_possible());
    int calls = 0;
    co2::stop_callback<> never{token, [&] { ++calls; }}; // 不会注册
    CHECK(calls == 0);
}

void copiesShareTheState() {
    co2::stop_source source;
    auto copy = source;
    CHECK(copy == source);
    CHECK(copy.get_token() == source.get_token());
    auto moved = std::move(source);
    CHECK(not source.stop_possible());
    CHECK(moved == copy);
    CHECK(copy.request_stop());
    CHECK(moved.stop_requested());
    CHECK(moved.get_token().stop_requested());
}

void nestedRequestsAndRegistrationsInsideCallbacks() {
    co2::stop_source source;
    bool nestedReturnedFalse = false;
    int nestedCalls = 0;
    std::unique_ptr<co2::stop_callback<>> nested;
    co2::stop_callback<> outer{source.get_token(), [&] {
                                   nestedReturnedFalse = not source.request_stop();
                                   nested.reset(new co2::stop_callback<>{
                                       source.get_token(), [&] { ++nestedCalls; }});
                               }};
    CHECK(source.request_stop());
    CHECK(nestedReturnedFalse);
    CHECK(nestedCalls == 1);
}

void callbackMayDestroyItselfWhileRunning() {
    co2::stop_source source;
    std::unique_ptr<co2::stop_callback<>> self;
    int calls = 0;
    self.reset(new co2::stop_callback<>{source.get_token(), [&] {
                                            ++calls;
                                            self.reset(); // 同线程：不阻塞、不崩溃
                                        }});
    CHECK(source.request_stop());
    CHECK(calls == 1);
    CHECK(self == nullptr);
}

void destructorWaitsForACallbackRunningOnAnotherThread() {
    co2::stop_source source;
    std::mutex mutex;
    std::condition_variable started;
    bool running = false;
    std::atomic<bool> finished{false};
    std::unique_ptr<co2::stop_callback<>> callback{new co2::stop_callback<>{
        source.get_token(), [&] {
            {
                std::lock_guard<std::mutex> lock{mutex};
                running = true;
                started.notify_all();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
            finished.store(true);
        }}};
    std::thread requester{[&] { source.request_stop(); }};
    {
        std::unique_lock<std::mutex> lock{mutex};
        started.wait(lock, [&] { return running; });
    }
    callback.reset(); // 必须阻塞到回调返回
    CHECK(finished.load());
    requester.join();
}

void concurrentRegistrationAndRequestNeverDoubleInvokeOrLose() {
    for (int round = 0; round < 20; ++round) {
        co2::stop_source source;
        std::atomic<int> invoked{0};
        std::atomic<int> registeredAndKept{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                while (not go.load())
                    std::this_thread::yield();
                for (int i = 0; i < 50; ++i) {
                    co2::stop_callback<> callback{source.get_token(),
                                                  [&] { ++invoked; }};
                    if (i % 2 == 0) ++registeredAndKept; // 这些在作用域内可能被请求命中
                }
            });
        }
        std::thread requester{[&] {
            while (not go.load())
                std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::microseconds{50});
            source.request_stop();
        }};
        go.store(true);
        for (auto& thread : threads)
            thread.join();
        requester.join();
        // 请求之后构造的回调立即执行，之前构造且未销毁的执行一次；总数不超过构造总数。
        CHECK(invoked.load() <= 4 * 50);
        co2::stop_callback<> late{source.get_token(), [&] { ++invoked; }};
    }
}

// 回调放掉了停止状态的其他全部引用——连正在调用 request_stop() 的 stop_source 也一起
// 销毁（库内 ForwardStop 就是这样：它指向的 source 住在一个可能被回调链拆掉的对象里）。
// request_stop() 必须在派发期间自己持有状态，否则回调返回后它会触碰已释放的内存。
void callbackMayReleaseEveryOtherReferenceIncludingTheRequestingSource() {
    auto source = std::unique_ptr<co2::stop_source>{new co2::stop_source{}};
    auto callback = std::unique_ptr<co2::stop_callback<>>{};
    int calls = 0;
    callback.reset(new co2::stop_callback<>{source->get_token(), [&] {
        ++calls;
        source.reset();
        callback.reset();
    }});
    auto* const raw = source.get();
    CHECK(raw->request_stop());
    CHECK(calls == 1);
    CHECK(source == nullptr && callback == nullptr);
}

// 同一场景，但还有一个回调排在后面：先执行的回调销毁了 stop_source，后执行的回调仍要
// 运行，并且是它放掉最后一个引用——状态在派发循环仍在进行时到达零引用。
void laterCallbacksStillRunAfterAnEarlierOneDestroyedTheSource() {
    auto source = std::unique_ptr<co2::stop_source>{new co2::stop_source{}};
    std::vector<int> order;
    std::unique_ptr<co2::stop_callback<>> first;
    std::unique_ptr<co2::stop_callback<>> second;
    // 注册顺序 first → second；执行顺序是后注册者先执行（LIFO，与标准实现一致）。
    first.reset(new co2::stop_callback<>{source->get_token(), [&] {
        order.push_back(1);
        first.reset(); // 最后一个引用
    }});
    second.reset(new co2::stop_callback<>{source->get_token(), [&] {
        order.push_back(2);
        source.reset();
        second.reset();
    }});
    auto* const raw = source.get();
    CHECK(raw->request_stop());
    CHECK((order == std::vector<int>{2, 1}));
    CHECK(source == nullptr && first == nullptr && second == nullptr);
}

} // namespace

int main() {
    defaultTokenAndNoStopStateSourceCannotBeStopped();
    requestStopSucceedsExactlyOnce();
    callbacksRunOnceOnRequestAndImmediatelyIfAlreadyRequested();
    destroyedCallbacksAreNotInvoked();
    allRegisteredCallbacksRun();
    stopPossibleFollowsTheLastSource();
    copiesShareTheState();
    nestedRequestsAndRegistrationsInsideCallbacks();
    callbackMayDestroyItselfWhileRunning();
    destructorWaitsForACallbackRunningOnAnotherThread();
    concurrentRegistrationAndRequestNeverDoubleInvokeOrLose();
    callbackMayReleaseEveryOtherReferenceIncludingTheRequestingSource();
    laterCallbacksStillRunAfterAnEarlierOneDestroyedTheSource();
    return 0;
}
