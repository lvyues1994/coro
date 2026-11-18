// 编译失败测试：CO2_HAS_EXCEPTIONS 不能由使用者指定。
//
// 它由编译器的异常开关得出（config.hpp）。手动定成与开关不一致的值，库里要么留下编不过的
// try / catch，要么去掉了回滚用的 catch 而泄漏资源；所以一经定义就报错，而不是采信。
//
// 定义 CO2_EXPECT_COMPILE_FAILURE 时本文件必须编不过；不定义时是对照，必须能编过。

#ifdef CO2_EXPECT_COMPILE_FAILURE
#define CO2_HAS_EXCEPTIONS 1
#endif

#include "co2/config.hpp"

static_assert(CO2_HAS_EXCEPTIONS == 0 || CO2_HAS_EXCEPTIONS == 1,
              "CO2_HAS_EXCEPTIONS is either 0 or 1");
