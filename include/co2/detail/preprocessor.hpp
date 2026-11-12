#pragma once

// 协程 DSL 使用的小型无依赖预处理器子集。
//
// 两个上限：捕获 tuple 支持零到八个参数名（FOR_EACH）；按实参个数做选择的宏最多接受
// 64 个实参（ARG_65，见下）。

// 下面的宏依赖标准的 __VA_ARGS__ 展开。MSVC 的传统预处理器把 __VA_ARGS__ 当成单个记号
// 传给嵌套宏，IS_PAREN / NARG / IS_EMPTY 会全部失效，报出一堆 C4002/C4003 与后续语法
// 错误。VS 2019 16.5 起的一致预处理器（/Zc:preprocessor）把 _MSVC_TRADITIONAL 定义为 0；
// 这里提前给出明确的错误，而不是让使用者面对宏展开残骸。通过 CMake 链接 co2::co2 会
// 自动带上该选项。
#if defined(_MSC_VER) && !defined(__clang__) &&                                        \
    (!defined(_MSVC_TRADITIONAL) || _MSVC_TRADITIONAL)
#error "co2's coroutine DSL requires the conforming preprocessor on MSVC: compile with /Zc:preprocessor (VS 2019 16.5 or newer)"
#endif

#define CO2_DETAIL_PP_CAT(left, right) CO2_DETAIL_PP_CAT_I(left, right)
#define CO2_DETAIL_PP_CAT_I(left, right) left##right

#define CO2_DETAIL_PP_IF(condition, when_true, when_false)                             \
    CO2_DETAIL_PP_CAT(CO2_DETAIL_PP_IF_, condition)(when_true, when_false)
#define CO2_DETAIL_PP_IF_0(when_true, when_false) when_false
#define CO2_DETAIL_PP_IF_1(when_true, when_false) when_true

#define CO2_DETAIL_PP_REMOVE_PARENS_I(...) __VA_ARGS__

#define CO2_DETAIL_PP_IS_PAREN(value)                                                  \
    CO2_DETAIL_PP_IS_PAREN_CHECK(CO2_DETAIL_PP_IS_PAREN_PROBE value)
#define CO2_DETAIL_PP_IS_PAREN_PROBE(...) ~, 1
#define CO2_DETAIL_PP_IS_PAREN_CHECK(...)                                              \
    CO2_DETAIL_PP_IS_PAREN_CHECK_I(__VA_ARGS__, 0, ~)
#define CO2_DETAIL_PP_IS_PAREN_CHECK_I(ignored, result, ...) result
// 先按"是否带括号"选出处理宏再应用，这样括号内的逗号（模板实参）不会在参数预展开
// 时被拆成多个宏参数。
#define CO2_DETAIL_PP_REMOVE_PARENS(value)                                             \
    CO2_DETAIL_PP_CAT(CO2_DETAIL_PP_REMOVE_PARENS_, CO2_DETAIL_PP_IS_PAREN(value))     \
    (value)
#define CO2_DETAIL_PP_REMOVE_PARENS_0(value) value
#define CO2_DETAIL_PP_REMOVE_PARENS_1(value) CO2_DETAIL_PP_REMOVE_PARENS_I value

// 按实参个数做选择的宏（HAS_COMMA，以及 dsl.hpp 里 CO2_BEG / CO2_BEG_WITH_ALLOCATOR 的
// 分支选择）都落在这一个取数宏上：它返回第 65 个实参，所以这些宏最多接受 64 个实参——
// 局部列表、CO2_RETURN 的表达式里的顶层逗号（模板实参、花括号初始化）都算在内。超过
// 上限时用圆括号或类型别名把逗号包起来。C++ 实现至少支持 256 个宏实参。
//
// 外层多转发一次：实参里的 CO2_DETAIL_PP_REPEAT_60 先展开，再重新按逗号切分。
#define CO2_DETAIL_PP_ARG_65(...) CO2_DETAIL_PP_ARG_65_I(__VA_ARGS__)
#define CO2_DETAIL_PP_ARG_65_I(                                                        \
    _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18,   \
    _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33, _34,    \
    _35, _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, _50,    \
    _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, _61, _62, _63, _64, result, ...) \
    result
#define CO2_DETAIL_PP_REPEAT_60(x)                                                     \
    x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x,   \
        x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x,  \
        x, x, x, x, x, x, x

// 实参里有顶层逗号（即实参不止一个）时为 1。
#define CO2_DETAIL_PP_HAS_COMMA(...)                                                   \
    CO2_DETAIL_PP_ARG_65(__VA_ARGS__, CO2_DETAIL_PP_REPEAT_60(1), 1, 1, 1, 0, ~)

#define CO2_DETAIL_PP_EMPTY_TRIGGER(...) ,
#define CO2_DETAIL_PP_PASTE5(a, b, c, d, e) CO2_DETAIL_PP_PASTE5_I(a, b, c, d, e)
#define CO2_DETAIL_PP_PASTE5_I(a, b, c, d, e) a##b##c##d##e
#define CO2_DETAIL_PP_IS_EMPTY_CASE_0001 ,
#define CO2_DETAIL_PP_IS_EMPTY(...)                                                    \
    CO2_DETAIL_PP_IS_EMPTY_I(                                                          \
        CO2_DETAIL_PP_HAS_COMMA(__VA_ARGS__),                                          \
        CO2_DETAIL_PP_HAS_COMMA(CO2_DETAIL_PP_EMPTY_TRIGGER __VA_ARGS__),              \
        CO2_DETAIL_PP_HAS_COMMA(__VA_ARGS__()),                                        \
        CO2_DETAIL_PP_HAS_COMMA(CO2_DETAIL_PP_EMPTY_TRIGGER __VA_ARGS__()))
#define CO2_DETAIL_PP_IS_EMPTY_I(a, b, c, d)                                           \
    CO2_DETAIL_PP_HAS_COMMA(                                                           \
        CO2_DETAIL_PP_PASTE5(CO2_DETAIL_PP_IS_EMPTY_CASE_, a, b, c, d))

#define CO2_DETAIL_PP_NARG(...)                                                        \
    CO2_DETAIL_PP_IF(CO2_DETAIL_PP_IS_EMPTY(__VA_ARGS__), 0,                           \
                     CO2_DETAIL_PP_NARG_NONEMPTY(__VA_ARGS__))
#define CO2_DETAIL_PP_NARG_NONEMPTY(...)                                               \
    CO2_DETAIL_PP_NARG_NONEMPTY_I(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, ~)
#define CO2_DETAIL_PP_NARG_NONEMPTY_I(_1, _2, _3, _4, _5, _6, _7, _8, result, ...)     \
    result

#define CO2_DETAIL_PP_FOR_EACH(macro, ...)                                             \
    CO2_DETAIL_PP_CAT(CO2_DETAIL_PP_FOR_EACH_, CO2_DETAIL_PP_NARG(__VA_ARGS__))        \
    (macro, __VA_ARGS__)
#define CO2_DETAIL_PP_FOR_EACH_0(macro, ...)
#define CO2_DETAIL_PP_FOR_EACH_1(macro, a1) macro(a1)
#define CO2_DETAIL_PP_FOR_EACH_2(macro, a1, a2) macro(a1) macro(a2)
#define CO2_DETAIL_PP_FOR_EACH_3(macro, a1, a2, a3) macro(a1) macro(a2) macro(a3)
#define CO2_DETAIL_PP_FOR_EACH_4(macro, a1, a2, a3, a4)                                \
    macro(a1) macro(a2) macro(a3) macro(a4)
#define CO2_DETAIL_PP_FOR_EACH_5(macro, a1, a2, a3, a4, a5)                            \
    macro(a1) macro(a2) macro(a3) macro(a4) macro(a5)
#define CO2_DETAIL_PP_FOR_EACH_6(macro, a1, a2, a3, a4, a5, a6)                        \
    macro(a1) macro(a2) macro(a3) macro(a4) macro(a5) macro(a6)
#define CO2_DETAIL_PP_FOR_EACH_7(macro, a1, a2, a3, a4, a5, a6, a7)                    \
    macro(a1) macro(a2) macro(a3) macro(a4) macro(a5) macro(a6) macro(a7)
#define CO2_DETAIL_PP_FOR_EACH_8(macro, a1, a2, a3, a4, a5, a6, a7, a8)                \
    macro(a1) macro(a2) macro(a3) macro(a4) macro(a5) macro(a6) macro(a7) macro(a8)

#define CO2_DETAIL_PP_TUPLE_FOR_EACH_NONEMPTY(macro, tuple)                            \
    CO2_DETAIL_PP_TUPLE_FOR_EACH_NONEMPTY_I(macro, tuple)
#define CO2_DETAIL_PP_TUPLE_FOR_EACH_NONEMPTY_I(macro, tuple)                          \
    CO2_DETAIL_PP_FOR_EACH(macro, CO2_DETAIL_PP_REMOVE_PARENS_I tuple)
#define CO2_DETAIL_PP_TUPLE_FOR_EACH_EMPTY(macro, tuple)
#define CO2_DETAIL_PP_TUPLE_FOR_EACH(macro, tuple)                                     \
    CO2_DETAIL_PP_IF(CO2_DETAIL_PP_IS_EMPTY tuple, CO2_DETAIL_PP_TUPLE_FOR_EACH_EMPTY, \
                     CO2_DETAIL_PP_TUPLE_FOR_EACH_NONEMPTY)                            \
    (macro, tuple)
