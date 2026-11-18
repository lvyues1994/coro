#pragma once

#define CO2_VERSION_MAJOR 0
#define CO2_VERSION_MINOR 3
#define CO2_VERSION_PATCH 0
#define CO2_VERSION_PRERELEASE "pre"
#define CO2_VERSION_STRING "0.3.0-pre"

#ifndef CO2_AWAIT_STORAGE_SIZE
#define CO2_AWAIT_STORAGE_SIZE (sizeof(void*) * 8U)
#endif

// MSVC 默认把 __cplusplus 钉在 199711L（除非 /Zc:__cplusplus），真实语言版本在 _MSVC_LANG。
#if defined(_MSVC_LANG)
#define CO2_CPLUSPLUS _MSVC_LANG
#else
#define CO2_CPLUSPLUS __cplusplus
#endif

#if CO2_CPLUSPLUS < 201402L
#error "co2 requires C++14 or newer"
#endif

// CO2_HAS_EXCEPTIONS：这个翻译单元有没有异常，由编译器的开关决定（GCC / Clang 的
// -fno-exceptions，MSVC 不带 /EH）。为 0 时库里不出现 try / catch / throw：
//   - 协程体变换里的 catch 不存在，promise 不需要 unhandled_exception()；
//   - co2::Error 只装错误码，没有装异常的那一半，Continuation::setException 也没有；
//   - Error::raise()——错误到达根、或者到达只能靠抛出来报告它的地方——是契约违规。
// 值、错误码、已停止这三种完成方式在两种构建下是同一份代码
// （docs/design/completion-channels.md）。
//
// 它不能手动指定：关着异常却让库保留 catch 没有意义，开着异常却让库去掉 catch 则是
// 资源泄漏。同一个程序的所有翻译单元必须用同一个异常开关编译，否则头文件里的内联函数
// 就有了两份不同的定义（ODR 违规）；MSVC 上由 detect_mismatch 在链接期检查。
#if defined(CO2_HAS_EXCEPTIONS)
#error "CO2_HAS_EXCEPTIONS follows the compiler's exception switch; do not define it"
#endif
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define CO2_HAS_EXCEPTIONS 1
#else
#define CO2_HAS_EXCEPTIONS 0
#endif

#if defined(_MSC_VER)
#if CO2_HAS_EXCEPTIONS
#pragma detect_mismatch("co2_exceptions", "enabled")
#else
#pragma detect_mismatch("co2_exceptions", "disabled")
#endif
#endif

// 代码里用 not / and / or 替代记号。MSVC 只有在 /permissive- 下才把它们当关键字；
// 默认模式下靠 <iso646.h> 的宏（_MSC_EXTENSIONS 开启时定义）兜底，消费方无需改编译选项。
// 每个根头文件都包含本文件，保证任何单独包含的头都能拿到这些定义。
#if defined(_MSC_VER) && !defined(__clang__)
#include <iso646.h>
#endif

// MSVC 的 C47xx 是代码生成阶段的警告：以函数开括号处的警告状态为准，函数体内再改无效。
// DSL 展开的 switch 状态机在 co_return 之后必然留下不可达的 break / 收尾语句，GCC 与
// Clang 不报，MSVC /W4 报 C4702。dsl.hpp 用这两个宏把生成的函数包起来。`number` 可以是
// 空格分隔的多个编号。
#if defined(_MSC_VER) && !defined(__clang__)
#define CO2_DETAIL_MSVC_WARNING_PUSH_DISABLE(number)                                   \
    __pragma(warning(push)) __pragma(warning(disable : number))
#define CO2_DETAIL_MSVC_WARNING_POP __pragma(warning(pop))
#else
#define CO2_DETAIL_MSVC_WARNING_PUSH_DISABLE(number)
#define CO2_DETAIL_MSVC_WARNING_POP
#endif
