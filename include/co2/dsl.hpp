#pragma once

#include <type_traits>
#include <utility>

#include "co2/detail/frame.hpp"
#include "co2/detail/preprocessor.hpp"

// co2 宏 DSL。生成的代码只做 [dcl.fct.def.coroutine] 与 [expr.await] 规定的调用：
//
//   auto f(int& value) CO2_BEG(Return, (value), int local{};) { ... } CO2_END
//
// 捕获列表必须是括号包裹的参数名（by-value 参数被移动进帧，引用保持引用）；捕获
// 列表之后的声明成为帧成员（跨挂起点存活的局部）。挂起点标签取 __LINE__，同一行
// 只能有一个挂起点。

#define CO2_DETAIL_CAPTURE_TYPE(name)                                                  \
    using CO2_DETAIL_PP_CAT(_co2_capture_type_, name) = decltype(name);
#define CO2_DETAIL_CAPTURE_MEMBER(name)                                                \
    CO2_DETAIL_PP_CAT(_co2_capture_type_, name) name;
#define CO2_DETAIL_CAPTURE_FORWARD(name) std::forward<decltype(name)>(name),
#define CO2_DETAIL_CAPTURE_LVALUE(name) this->name,
// Body 通过引用访问帧里的参数副本；引用类型的参数按引用折叠仍是同一个引用。
#define CO2_DETAIL_CAPTURE_REF(name) CO2_DETAIL_PP_CAT(_co2_capture_type_, name) & name;
#define CO2_DETAIL_CAPTURE_BIND(name)                                                  \
    , name { _co2_params.name }

// 参数副本（_co2_capture_pack）与局部（_co2_body）是帧里两个独立的对象，这样销毁
// 顺序才能与标准一致：局部 → promise → 参数副本。
#define CO2_DETAIL_BEG_IMPL(return_type, allocator, captures, ...)                     \
    ->CO2_DETAIL_PP_REMOVE_PARENS(return_type) {                                       \
        using _co2_return_type = CO2_DETAIL_PP_REMOVE_PARENS(return_type);             \
        using _co2_promise_type =                                                      \
            typename ::co2::coroutine_traits<_co2_return_type>::promise_type;          \
        auto _co2_allocator = (allocator);                                             \
        CO2_DETAIL_PP_TUPLE_FOR_EACH(CO2_DETAIL_CAPTURE_TYPE, captures)                \
        struct _co2_capture_pack {                                                     \
            CO2_DETAIL_PP_TUPLE_FOR_EACH(CO2_DETAIL_CAPTURE_MEMBER, captures)          \
            void _co2_construct_promise(void* const _co2_at) {                         \
                ::co2::detail::constructPromise<_co2_promise_type>(                    \
                    _co2_at, CO2_DETAIL_PP_TUPLE_FOR_EACH(                             \
                                 CO2_DETAIL_CAPTURE_LVALUE,                            \
                                 captures)::co2::detail::NoMoreParams{});              \
            }                                                                          \
        };                                                                             \
        _co2_capture_pack _co2_captures{                                               \
            CO2_DETAIL_PP_TUPLE_FOR_EACH(CO2_DETAIL_CAPTURE_FORWARD, captures)};       \
        struct _co2_body final : ::co2::detail::BodyBase {                             \
            CO2_DETAIL_PP_TUPLE_FOR_EACH(CO2_DETAIL_CAPTURE_REF, captures)             \
            explicit _co2_body(_co2_capture_pack& _co2_params)                         \
                : ::co2::detail::BodyBase {}                                           \
            CO2_DETAIL_PP_TUPLE_FOR_EACH(CO2_DETAIL_CAPTURE_BIND, captures) {          \
                static_cast<void>(_co2_params);                                        \
            }                                                                          \
            __VA_ARGS__                                                                \
            ::co2::detail::FrameHeader*                                                \
            operator()(::co2::detail::FrameCore<_co2_promise_type, _co2_capture_pack,  \
                                                _co2_body>& _co2_context) {            \
                switch (_co2_context.suspendPoint) {                                   \
                case 0U:                                                               \
                    _co2_context.finishInitialSuspend();

#define CO2_END                                                                        \
    break;                                                                             \
    default:                                                                           \
        CO2_CONTRACT_FAIL("co2 coroutine resumed at an invalid suspend point");        \
        }                                                                              \
        _co2_context.returnVoidAtEnd();                                                \
        return _co2_context.enterFinalSuspend();                                       \
        }                                                                              \
        }                                                                              \
        ;                                                                              \
        return ::co2::detail::startCoroutine<_co2_return_type, _co2_promise_type,      \
                                             _co2_body>(std::move(_co2_captures),      \
                                                        _co2_allocator);               \
        }

#define CO2_DETAIL_BEG_WITH_LOCALS(return_type, captures, ...)                         \
    CO2_DETAIL_BEG_IMPL(return_type, ::co2::detail::DefaultFrameAllocator{}, captures, \
                        __VA_ARGS__)
#define CO2_DETAIL_BEG_WITHOUT_LOCALS(return_type, captures)                           \
    CO2_DETAIL_BEG_IMPL(return_type, ::co2::detail::DefaultFrameAllocator{}, captures, )
#define CO2_DETAIL_BEG_SELECT(_1, _2, _3, _4, _5, _6, _7, _8, selected, ...) selected
#define CO2_BEG(...)                                                                   \
    CO2_DETAIL_BEG_SELECT(__VA_ARGS__, CO2_DETAIL_BEG_WITH_LOCALS,                     \
                          CO2_DETAIL_BEG_WITH_LOCALS, CO2_DETAIL_BEG_WITH_LOCALS,      \
                          CO2_DETAIL_BEG_WITH_LOCALS, CO2_DETAIL_BEG_WITH_LOCALS,      \
                          CO2_DETAIL_BEG_WITH_LOCALS, CO2_DETAIL_BEG_WITHOUT_LOCALS,   \
                          CO2_DETAIL_BEG_WITHOUT_LOCALS, ~)                            \
    (__VA_ARGS__)

// 用调用方提供的 allocator 分配协程帧（std::allocator_arg 约定的宏形式）。allocator
// 表达式在捕获被转发之前求值。
#define CO2_DETAIL_BEG_ALLOC_WITH_LOCALS(return_type, allocator, captures, ...)        \
    CO2_DETAIL_BEG_IMPL(return_type, allocator, captures, __VA_ARGS__)
#define CO2_DETAIL_BEG_ALLOC_WITHOUT_LOCALS(return_type, allocator, captures)          \
    CO2_DETAIL_BEG_IMPL(return_type, allocator, captures, )
#define CO2_DETAIL_BEG_ALLOC_SELECT(_1, _2, _3, _4, _5, _6, _7, _8, _9, selected, ...) \
    selected
#define CO2_BEG_WITH_ALLOCATOR(...)                                                    \
    CO2_DETAIL_BEG_ALLOC_SELECT(                                                       \
        __VA_ARGS__, CO2_DETAIL_BEG_ALLOC_WITH_LOCALS,                                 \
        CO2_DETAIL_BEG_ALLOC_WITH_LOCALS, CO2_DETAIL_BEG_ALLOC_WITH_LOCALS,            \
        CO2_DETAIL_BEG_ALLOC_WITH_LOCALS, CO2_DETAIL_BEG_ALLOC_WITH_LOCALS,            \
        CO2_DETAIL_BEG_ALLOC_WITH_LOCALS, CO2_DETAIL_BEG_ALLOC_WITHOUT_LOCALS, ~, ~)   \
    (__VA_ARGS__)

// 挂起点标签取自 __LINE__：头文件中的 inline/模板协程体在每个翻译单元展开出相同的
// token 序列（__COUNTER__ 会违反 ODR）。
#define CO2_DETAIL_SUSPEND_POINT_ID __LINE__

// [expr.await]：awaitable → (await_transform) → operator co_await → awaiter →
// await_ready → await_suspend（void / bool / coroutine_handle）→ await_resume。
// `awaiter_type` 是最终 awaiter 的类型（括号包裹，允许含逗号的模板实参）；
// `awaitable_expression` 已经完成 await_transform。
#define CO2_DETAIL_AWAIT_IMPL(awaiter_type, resume, awaitable_expression, unique)      \
    do {                                                                               \
        using _co2_awaiter_type = CO2_DETAIL_PP_REMOVE_PARENS_I awaiter_type;          \
        _co2_context.suspendPoint = static_cast<unsigned>(unique) + 1U;                \
        if (not _co2_context                                                           \
                    .template emplaceAwaiter<_co2_awaiter_type>(                       \
                        ::co2::detail::getAwaiter(awaitable_expression))               \
                    .await_ready()) {                                                  \
            _co2_context.markSuspended();                                              \
            auto const _co2_outcome = ::co2::detail::suspendWith(                      \
                _co2_context.template awaiter<_co2_awaiter_type>(),                    \
                _co2_context.handle());                                                \
            if (_co2_outcome.suspended) return _co2_outcome.next;                      \
            _co2_context.markRunning();                                                \
        }                                                                              \
    case static_cast<unsigned>(unique) + 1U:                                           \
        resume(_co2_context.template awaiter<_co2_awaiter_type>().await_resume());     \
        _co2_context.resetAwaiter();                                                   \
    } while (false)

// co_await e：经过 await_transform。
#define CO2_DETAIL_AWAIT(resume, expression, unique)                                   \
    CO2_DETAIL_AWAIT_IMPL(                                                             \
        (::co2::detail::AwaiterFor<_co2_promise_type, decltype((expression))>),        \
        resume, _co2_context.transform(expression), unique)

// yield / initial / final 隐式产生的 await：不经过
// await_transform（[expr.await]/3.2）。
#define CO2_DETAIL_AWAIT_UNTRANSFORMED(resume, expression, unique)                     \
    CO2_DETAIL_AWAIT_IMPL((::co2::detail::AwaiterOf<decltype((expression))>), resume,  \
                          (expression), unique)

#define CO2_AWAIT(expression)                                                          \
    CO2_DETAIL_AWAIT(static_cast<void>, expression, CO2_DETAIL_SUSPEND_POINT_ID)

#define CO2_AWAIT_SET(variable, expression)                                            \
    CO2_DETAIL_AWAIT(variable =, expression, CO2_DETAIL_SUSPEND_POINT_ID)

// 表达式含 lambda 时 decltype 不可用，显式给出 awaitable 的类型。类型含逗号（模板实参）
// 时用括号包裹：CO2_AWAIT_AS((Awaitable<A, B>), e)。
#define CO2_AWAIT_AS(awaitable_type, expression)                                       \
    CO2_DETAIL_AWAIT_IMPL(                                                             \
        (::co2::detail::AwaiterFor<_co2_promise_type,                                  \
                                   CO2_DETAIL_PP_REMOVE_PARENS(awaitable_type)>),      \
        static_cast<void>, _co2_context.transform(expression),                         \
        CO2_DETAIL_SUSPEND_POINT_ID)

#define CO2_AWAIT_AS_SET(variable, awaitable_type, expression)                         \
    CO2_DETAIL_AWAIT_IMPL(                                                             \
        (::co2::detail::AwaiterFor<_co2_promise_type,                                  \
                                   CO2_DETAIL_PP_REMOVE_PARENS(awaitable_type)>),      \
        variable =, _co2_context.transform(expression), CO2_DETAIL_SUSPEND_POINT_ID)

// co_yield e ≡ co_await promise.yield_value(e)。
#define CO2_YIELD(...)                                                                 \
    CO2_DETAIL_AWAIT_UNTRANSFORMED(static_cast<void>,                                  \
                                   _co2_context.promise().yield_value(__VA_ARGS__),    \
                                   CO2_DETAIL_SUSPEND_POINT_ID)

#define CO2_DETAIL_RETURN_EMPTY(...) _co2_context.promise().return_void()
#define CO2_DETAIL_RETURN_VALUE(...) _co2_context.promise().return_value(__VA_ARGS__)

// co_return [e]：return_void / return_value，然后 co_await final_suspend。
#define CO2_RETURN(...)                                                                \
    do {                                                                               \
        CO2_DETAIL_PP_IF(CO2_DETAIL_PP_IS_EMPTY(__VA_ARGS__), CO2_DETAIL_RETURN_EMPTY, \
                         CO2_DETAIL_RETURN_VALUE)                                      \
        (__VA_ARGS__);                                                                 \
        return _co2_context.enterFinalSuspend();                                       \
    } while (false)
