// 编译失败测试：CO2_BEG_MEMBER 只能用在非静态成员函数里。
//
// 它把 this 存进帧并交给 promise 的构造函数；普通函数和静态成员函数没有 this，展开出的
// decltype(this) 无法通过编译——而不是悄悄生成一个不带对象参数的协程。
//
// 定义 CO2_EXPECT_COMPILE_FAILURE 时本文件必须编不过；不定义时是对照，必须能编过。

#include "co2/task.hpp"

struct Counter {
#ifdef CO2_EXPECT_COMPILE_FAILURE
    static auto next(int step) CO2_BEG_MEMBER(co2::Task<int>, (step)) {
        CO2_RETURN(step);
    }
    CO2_END
#else
    auto next(int step) CO2_BEG_MEMBER(co2::Task<int>, (step)) {
        CO2_RETURN(self->value + step);
    }
    CO2_END
#endif

    int value{};
};
