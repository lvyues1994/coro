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

// 代码里用 not / and / or 替代记号。MSVC 只有在 /permissive- 下才把它们当关键字；
// 默认模式下靠 <iso646.h> 的宏（_MSC_EXTENSIONS 开启时定义）兜底，消费方无需改编译选项。
// 每个根头文件都包含本文件，保证任何单独包含的头都能拿到这些定义。
#if defined(_MSC_VER) && !defined(__clang__)
#include <iso646.h>
#endif
