// 编译失败测试：挂起点不能写在协程体自己的 switch 语句里。
//
// CO2_AWAIT 展开出的 case 标签属于最近一层 switch。状态机的条件类型是作用域枚举
// co2::detail::SuspendPoint，标签落进任何别的 switch 都无法隐式转换成那个 switch 的条件
// 类型，于是编译失败——否则它要等到真的挂起并恢复时才以"恢复到无效挂起点"暴露。
//
// 定义 CO2_EXPECT_COMPILE_FAILURE 时本文件必须编不过；不定义时是对照，必须能编过。

#include "co2/task.hpp"

auto dispatch(int code) CO2_BEG(co2::Task<int>, (code), int result{};) {
#ifdef CO2_EXPECT_COMPILE_FAILURE
    switch (code) {
    case 1:
        CO2_AWAIT(co2::suspend_always{});
        result = 10;
        break;
    default:
        result = 20;
        break;
    }
#else
    if (code == 1) {
        CO2_AWAIT(co2::suspend_always{});
        result = 10;
    } else {
        result = 20;
    }
#endif
    CO2_RETURN(result);
}
CO2_END
