#pragma once

#define CO2_VERSION_MAJOR 0
#define CO2_VERSION_MINOR 3
#define CO2_VERSION_PATCH 0
#define CO2_VERSION_PRERELEASE "pre"
#define CO2_VERSION_STRING "0.3.0-pre"

#ifndef CO2_AWAIT_STORAGE_SIZE
#define CO2_AWAIT_STORAGE_SIZE (sizeof(void*) * 8U)
#endif

#if __cplusplus < 201402L
#error "co2 requires C++14 or newer"
#endif
