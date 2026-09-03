if(NOT DEFINED CO2_INSTALL_PREFIX)
    message(FATAL_ERROR "CO2_INSTALL_PREFIX is required")
endif()

set(CMAKE_PREFIX_PATH "${CO2_INSTALL_PREFIX}")
find_package(co2 0.2.0 EXACT CONFIG QUIET)
if(co2_FOUND)
    message(FATAL_ERROR "0.2.0-pre was incorrectly accepted as exact 0.2.0")
endif()

message(STATUS "0.2.0-pre correctly rejected exact numeric version 0.2.0")
