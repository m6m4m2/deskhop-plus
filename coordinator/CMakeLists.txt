# SPDX-License-Identifier: GPL-2.0-only
#
# Coordinator daemon. Builds on any Linux, not only a Pi -- which is the point
# of the platform layer being separate: the protocol behaviour can be exercised
# on a development machine with --headless and a USB serial adapter.
cmake_minimum_required(VERSION 3.13)
project(deskhop_coordinator C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(DHP_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

file(GLOB DHP_CORE_SRC ${DHP_ROOT}/core/src/*.c)

# The same Monocypher the firmware uses. Two implementations of a key exchange
# that must agree byte-for-byte is a good way to find out on a bench that they
# do not.
add_library(monocypher STATIC ${DHP_ROOT}/third_party/monocypher/src/monocypher.c)
target_include_directories(monocypher PUBLIC
    ${DHP_ROOT}/third_party/monocypher/src)

add_executable(deskhop-coord
    src/main.c
    src/serial.c
    src/ssd1306.c
    src/display.c
    src/config.c
    src/crypto_linux.c
    ${DHP_CORE_SRC}
)

target_include_directories(deskhop-coord PRIVATE
    ${DHP_ROOT}/core/include
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_compile_options(deskhop-coord PRIVATE -Wall -Wextra -Wshadow)
target_link_libraries(deskhop-coord PRIVATE monocypher)

enable_testing()

# Renders every status screen into a framebuffer and checks it. No panel, no
# I2C bus; run the binary with --dump to see them as ASCII art.
add_executable(test_display tests/test_display.c src/display.c)
target_include_directories(test_display PRIVATE
    ${DHP_ROOT}/core/include ${CMAKE_CURRENT_LIST_DIR}/src)
add_test(NAME display COMMAND test_display)

# The only place the daemon is actually run: a pty stands in for the UART and
# a real chain board (built from the real core/) sits on the other end.
add_executable(fakeboard tests/fakeboard.c ${DHP_CORE_SRC})
target_include_directories(fakeboard PRIVATE ${DHP_ROOT}/core/include)
target_link_libraries(fakeboard PRIVATE util)
add_test(NAME end_to_end COMMAND fakeboard $<TARGET_FILE:deskhop-coord>)

install(TARGETS deskhop-coord RUNTIME DESTINATION bin)
install(FILES ${CMAKE_CURRENT_LIST_DIR}/deskhop-coord.service
        DESTINATION lib/systemd/system)
