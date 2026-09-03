#pragma once

// co2 协程核心的聚合头：句柄、awaitable 协议、帧与 ramp、宏 DSL。
#include "co2/coroutine_handle.hpp"
#include "co2/detail/awaitable.hpp"
#include "co2/detail/frame.hpp"
#include "co2/dsl.hpp"
