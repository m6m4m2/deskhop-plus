# SPDX-License-Identifier: GPL-2.0-only
#
# Chain board firmware.
#
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   cmake -B build -S . && cmake --build build
#
# Produces build/deskhop_plus.uf2 -- one image for every board in the chain.
# Boards differ only by their flash unique id, which they read at runtime.
cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(deskhop_plus C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

set(DHP_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

file(GLOB DHP_CORE_SRC ${DHP_ROOT}/core/src/*.c)

add_executable(deskhop_plus
    src/main.c
    src/board.c
    src/hid_bridge.c
    src/crypto_backend.c
    src/usb_descriptors.c
    ${DHP_CORE_SRC}
)

target_include_directories(deskhop_plus PRIVATE
    ${DHP_ROOT}/core/include
    ${CMAKE_CURRENT_LIST_DIR}/src
)

# PIO-USB provides the host port; the native controller stays in device mode.
# See docs/hardware.md for why both roles are needed on every board.
add_subdirectory(${DHP_ROOT}/third_party/Pico-PIO-USB pico_pio_usb)
add_subdirectory(${DHP_ROOT}/third_party/monocypher monocypher)

target_link_libraries(deskhop_plus PRIVATE
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_pio
    hardware_dma
    hardware_flash
    hardware_uart
    tinyusb_device
    tinyusb_host
    tinyusb_board
    pico_pio_usb
    monocypher
)

# PIO-USB bit-bangs USB in software and needs the full clock.
pico_set_binary_type(deskhop_plus default)
target_compile_definitions(deskhop_plus PRIVATE
    PICO_DEFAULT_UART=-1          # both UARTs belong to the chain
    CFG_TUSB_RHPORT0_MODE=OPT_MODE_DEVICE
    CFG_TUSB_RHPORT1_MODE=OPT_MODE_HOST
)

pico_add_extra_outputs(deskhop_plus)
