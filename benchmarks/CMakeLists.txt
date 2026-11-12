# 性能基准：`cmake --preset release-bench` 或 -DCO2_BUILD_BENCHMARKS=ON 时构建。
add_executable(co2-benchmarks co2_benchmarks.cpp)
target_link_libraries(co2-benchmarks PRIVATE co2::co2)
set_target_properties(
    co2-benchmarks
    PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO)

if(MSVC)
    target_compile_options(co2-benchmarks PRIVATE /W4 /permissive- /wd4324 /wd4127)
else()
    target_compile_options(co2-benchmarks PRIVATE -Wall -Wextra -Wpedantic)
endif()
