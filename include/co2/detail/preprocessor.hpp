#pragma once

// 协程 DSL 使用的小型无依赖预处理器子集。
// 捕获 tuple 当前支持零到八个参数名。

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

#define CO2_DETAIL_PP_HAS_COMMA(...)                                                   \
    CO2_DETAIL_PP_HAS_COMMA_I(__VA_ARGS__, 1, 1, 1, 1, 1, 1, 1, 1, 0, ~)
#define CO2_DETAIL_PP_HAS_COMMA_I(_0, _1, _2, _3, _4, _5, _6, _7, _8, result, ...)     \
    result

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
