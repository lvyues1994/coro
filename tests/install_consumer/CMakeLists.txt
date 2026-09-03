cmake_minimum_required(VERSION 3.14)

project(co2_install_consumer LANGUAGES CXX)

find_package(co2 CONFIG REQUIRED)

add_executable(co2-install-consumer main.cpp)
target_link_libraries(co2-install-consumer PRIVATE co2::co2)
set_target_properties(
    co2-install-consumer
    PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO)
