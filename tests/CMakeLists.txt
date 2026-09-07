# 普通测试：可执行文件名与源文件名一一对应，成功即退出码 0。
set(co2_test_sources
    conformance_tests.cpp
    storage_tests.cpp
    return_types_tests.cpp
    scheduler_tests.cpp
    thread_pool_tests.cpp
    stop_token_tests.cpp
    spawn_tests.cpp
    when_all_tests.cpp
    callback_tests.cpp
    allocation_tests.cpp
    edge_case_tests.cpp)

# 契约违规测试：程序在预期的失败点经由契约处理器以退出码 86 结束。
set(co2_contract_violation_test_sources
    task_destroy_contract_violation_test.cpp
    manual_executor_contract_violation_test.cpp
    join_handle_contract_violation_test.cpp
    generator_begin_twice_contract_violation_test.cpp
    resume_running_contract_violation_test.cpp
    thread_pool_zero_threads_contract_violation_test.cpp
    async_generator_destroy_contract_violation_test.cpp
    continuation_twice_contract_violation_test.cpp)

include(CheckCXXCompilerFlag)

# GCC 在 -fsanitize=thread 下对 std::atomic_thread_fence 报 -Wtsan（TSan 不建模栅栏），
# 与 -Werror 一起会让 TSan 构建编不过。thread_pool.hpp 里的两处 seq_cst 栅栏只承担
# Dekker 协议的活性（不丢唤醒）；数据可见性走队列自身的 release/acquire 与互斥量，TSan
# 忽略这两处栅栏不会产生误报，因此在支持该选项的编译器上关掉它。Clang 没有 -Wtsan。
check_cxx_compiler_flag(-Wtsan CO2_COMPILER_HAS_WTSAN)

function(co2_configure_test_target target)
    target_link_libraries(${target} PRIVATE co2::co2)
    set_target_properties(
        ${target}
        PROPERTIES
            CXX_STANDARD 14
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO)

    if(MSVC)
        # /permissive- 走标准一致模式（两阶段查找、替代记号等）。不加 /Zc:__cplusplus，
        # 让 config.hpp 的 _MSVC_LANG 版本判断在 CI 里真正被测到。
        # C4324：alignas 导致结构体填充——AwaitSlot / MoveOnlyFunction 的内联缓冲刻意如此。
        # C4127：条件表达式为常量——C++14 没有 if constexpr，模板里依赖 T 的 if 必然如此。
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /wd4324 /wd4127)
    else()
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Werror
                -Wconversion
                -Wsign-conversion
                -Wnon-virtual-dtor
                -Wold-style-cast
                -Wzero-as-null-pointer-constant
                -Werror=return-type
                -Werror=uninitialized
                -Werror=suggest-override
                -Werror=vla)
        if(CO2_COMPILER_HAS_WTSAN)
            target_compile_options(${target} PRIVATE -Wno-tsan)
        endif()
    endif()
endfunction()

function(co2_test_target_name source out_var)
    get_filename_component(stem "${source}" NAME_WE)
    string(REPLACE "_" "-" stem "${stem}")
    set(${out_var} "co2-${stem}" PARENT_SCOPE)
endfunction()

foreach(co2_test_source IN LISTS co2_test_sources)
    co2_test_target_name("${co2_test_source}" co2_test_target)
    add_executable(${co2_test_target} ${co2_test_source})
    co2_configure_test_target(${co2_test_target})
    add_test(NAME ${co2_test_target} COMMAND ${co2_test_target})
endforeach()

foreach(co2_test_source IN LISTS co2_contract_violation_test_sources)
    co2_test_target_name("${co2_test_source}" co2_test_target)
    add_executable(${co2_test_target} ${co2_test_source})
    co2_configure_test_target(${co2_test_target})
    add_test(NAME ${co2_test_target}
             COMMAND ${CMAKE_COMMAND}
                     -DTEST_EXECUTABLE=$<TARGET_FILE:${co2_test_target}>
                     -DEXPECTED_EXIT_CODE=86
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/expect_exit_code.cmake)
endforeach()

# 每个公共头文件都必须能单独包含。
file(GLOB_RECURSE co2_public_headers CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/include/co2/*.hpp")
set(co2_header_test_sources)
foreach(co2_public_header IN LISTS co2_public_headers)
    file(RELATIVE_PATH co2_header_include "${PROJECT_SOURCE_DIR}/include"
         "${co2_public_header}")
    string(MAKE_C_IDENTIFIER "${co2_header_include}" co2_header_identifier)
    set(co2_header_test_source
        "${CMAKE_CURRENT_BINARY_DIR}/header-self-containment/${co2_header_identifier}.cpp")
    file(GENERATE OUTPUT "${co2_header_test_source}"
         CONTENT "#include <${co2_header_include}>\n")
    list(APPEND co2_header_test_sources "${co2_header_test_source}")
endforeach()

add_library(co2-header-self-containment-tests OBJECT ${co2_header_test_sources})
co2_configure_test_target(co2-header-self-containment-tests)

# 与原生 co_await 的差分测试只在编译器支持标准协程时构建。
if("cxx_std_20" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    foreach(co2_cpp20_source IN ITEMS differential_tests.cpp)
        co2_test_target_name("${co2_cpp20_source}" co2_test_target)
        add_executable(${co2_test_target} ${co2_cpp20_source})
        co2_configure_test_target(${co2_test_target})
        set_target_properties(${co2_test_target} PROPERTIES CXX_STANDARD 20)
        add_test(NAME ${co2_test_target} COMMAND ${co2_test_target})
    endforeach()
endif()
