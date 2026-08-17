# SPDX-License-Identifier: GPL-2.0-only
#
# Host-side build of the protocol core and its tests.
# Firmware lives in firmware/rp2040/ and has its own Pico SDK build.
cmake_minimum_required(VERSION 3.13)
project(deskhop_plus C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

file(GLOB DHP_CORE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/core/src/*.c)

# The core is deliberately a plain static library with no dependencies: it is
# the same translation units that get compiled into the firmware.
add_library(dhp_core STATIC ${DHP_CORE_SRC})
target_include_directories(dhp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
target_compile_options(dhp_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

if(PROJECT_IS_TOP_LEVEL)
    enable_testing()

    file(GLOB DHP_TEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c)
    # The board's client proxy is firmware but touches no hardware; the rule it
    # enforces is security-critical, so it is tested here.
    add_executable(dhp_tests ${DHP_TEST_SRC}
        ${CMAKE_CURRENT_SOURCE_DIR}/firmware/rp2040/src/clientlink.c)
    target_link_libraries(dhp_tests PRIVATE dhp_core)
    target_include_directories(dhp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    add_test(NAME dhp_tests COMMAND dhp_tests)
endif()
