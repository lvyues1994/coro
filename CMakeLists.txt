cmake_minimum_required(VERSION 3.14)

project(co2 VERSION 0.3.0 LANGUAGES CXX)

set(CO2_VERSION_PRERELEASE "pre")
set(CO2_PACKAGE_VERSION "${PROJECT_VERSION}-${CO2_VERSION_PRERELEASE}")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

add_library(co2 INTERFACE)
add_library(co2::co2 ALIAS co2)

target_compile_features(co2 INTERFACE cxx_std_14)
target_link_libraries(co2 INTERFACE Threads::Threads)
# 协程 DSL 的预处理器元编程依赖标准的 __VA_ARGS__ 展开；MSVC 的传统预处理器把它当成单个
# 记号传给嵌套宏，会拆坏 IS_PAREN / NARG / IS_EMPTY。VS 2019 16.5 起提供的一致预处理器
# 由 /Zc:preprocessor 开启，作为 INTERFACE 选项随 co2::co2 传给消费方。clang-cl 的
# 预处理器本身就是一致的，CXX_COMPILER_ID 为 Clang，不会命中。
target_compile_options(co2 INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)
target_include_directories(
    co2
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

set(CO2_BUILD_TESTS_DEFAULT OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(CO2_BUILD_TESTS_DEFAULT ON)
endif()

option(CO2_BUILD_TESTS "Build CO2 tests" ${CO2_BUILD_TESTS_DEFAULT})
option(CO2_BUILD_BENCHMARKS "Build CO2 performance benchmarks" OFF)
unset(CO2_BUILD_TESTS_DEFAULT)

if(CO2_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CO2_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

install(
    TARGETS co2
    EXPORT co2Targets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
    EXPORT co2Targets
    FILE co2Targets.cmake
    NAMESPACE co2::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/co2)

configure_package_config_file(
    cmake/co2Config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/co2Config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/co2)

if(CO2_VERSION_PRERELEASE)
    # CMake 的版本请求只接受数字分量，无法准确表达 SemVer 预发布标识。预览包允许
    # 无版本 find_package，但拒绝所有数字版本请求，避免被当作正式 0.2.0 接受。
    configure_file(
        cmake/co2ConfigVersion.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/co2ConfigVersion.cmake
        @ONLY)
else()
    # 0.x 阶段不承诺跨 minor 兼容；稳定包只接受精确版本。
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/co2ConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY ExactVersion)
endif()

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/co2Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/co2ConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/co2)
