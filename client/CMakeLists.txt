# SPDX-License-Identifier: GPL-2.0-only
#
# Level 3 client. Builds on any Linux; the board link has a socket backend so
# it can be developed and tested with no hardware at all.
cmake_minimum_required(VERSION 3.13)
project(deskhop_client C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(DHP_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
file(GLOB DHP_CORE_SRC ${DHP_ROOT}/core/src/*.c)

add_executable(deskhop-client
    src/main.c
    src/boardlink.c
    src/clipboard.c
    ${DHP_CORE_SRC}
)
target_include_directories(deskhop-client PRIVATE
    ${DHP_ROOT}/core/include
    ${CMAKE_CURRENT_LIST_DIR}/src
)
target_compile_options(deskhop-client PRIVATE -Wall -Wextra -Wshadow)

enable_testing()

# Two real clients, a proxy board between them, and a transfer end to end.
add_executable(client_e2e tests/e2e.c ${DHP_CORE_SRC})
target_include_directories(client_e2e PRIVATE
    ${DHP_ROOT}/core/include ${CMAKE_CURRENT_LIST_DIR}/src)
add_test(NAME client_end_to_end
         COMMAND client_e2e $<TARGET_FILE:deskhop-client>)
# Spawns real processes and waits on sockets, so it gets a hard ceiling.
set_tests_properties(client_end_to_end PROPERTIES TIMEOUT 120)

install(TARGETS deskhop-client RUNTIME DESTINATION bin)
install(PROGRAMS ${CMAKE_CURRENT_LIST_DIR}/deskhop-send DESTINATION bin)
