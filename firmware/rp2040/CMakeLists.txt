# SPDX-License-Identifier: GPL-2.0-only
#
# Chain board firmware.
#
#   git submodule update --init --recursive
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   cmake -B build -S . && cmake --build build
#
# Produces build/deskhop_plus.uf2 -- one image for every board in the chain.
# Boards differ only by their flash unique id, which they read at runtime.
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico CACHE STRING "Target board")

include(pico_sdk_import.cmake)

project(deskhop_plus C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

set(DHP_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

file(GLOB DHP_CORE_SRC ${DHP_ROOT}/core/src/*.c)

# Monocypher ships as plain sources with a hand-written makefile, so the
# target is declared here rather than by add_subdirectory.
add_library(monocypher STATIC ${DHP_ROOT}/third_party/monocypher/src/monocypher.c)
target_include_directories(monocypher PUBLIC
    ${DHP_ROOT}/third_party/monocypher/src)

# PIO-USB provides the host port; the native controller stays in device mode.
# See docs/hardware.md for why both roles are needed on every board.
add_subdirectory(${DHP_ROOT}/third_party/Pico-PIO-USB pico_pio_usb)

add_executable(deskhop_plus
    src/main.c
    src/board.c
    src/hid_bridge.c
    src/clientlink.c
    src/crypto_backend.c
    src/usb_descriptors.c
    ${DHP_CORE_SRC}
    # The PIO-USB host controller driver. The SDK's tinyusb_host target only
    # brings in the native controller, so without this every hcd_* entry point
    # the host stack calls is undefined at link time.
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

# Assembles ws2812.pio into a header of program descriptors and helpers.
pico_generate_pio_header(deskhop_plus ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)

target_include_directories(deskhop_plus PRIVATE
    ${DHP_ROOT}/core/include
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${PICO_SDK_PATH}/lib/tinyusb/src
)

target_link_libraries(deskhop_plus PRIVATE
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_pio
    hardware_dma
    hardware_flash
    hardware_uart
    hardware_clocks
    tinyusb_device
    tinyusb_host
    pico_pio_usb
    monocypher
)

# tinyusb_board is deliberately NOT linked. It defines board_init(), which
# would collide with this project's own board layer, and nothing here uses the
# BSP -- board.c is the hardware layer.

# Both UARTs belong to the chain, so neither can be a console. Disabling the
# stdio drivers is the way to say that -- PICO_DEFAULT_UART=-1 is not, since
# the SDK pastes that index straight into a `uart##N` token.
pico_enable_stdio_uart(deskhop_plus 0)
# ...and stdio-over-USB would want a CDC interface on the controller that is
# busy being a keyboard.
pico_enable_stdio_usb(deskhop_plus 0)

target_compile_definitions(deskhop_plus PRIVATE
    PICO_DEFAULT_PIO_USB_DP_PIN=0
)

# PIO-USB needs a 120 MHz system clock, which board_init() sets at runtime
# rather than here: a compile-time SYS_CLK_KHZ obliges you to supply the PLL
# VCO and post-dividers by hand, and setting it at runtime keeps the UART baud
# divisor calculation correct because uart_init() then runs at the final clock.
pico_add_extra_outputs(deskhop_plus)
