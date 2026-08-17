/* SPDX-License-Identifier: GPL-2.0-only
 *
 * TinyUSB configuration for the dual-role chain board.
 *
 * Both stacks are enabled at once, on different root ports:
 *
 *   rhport 0  device, on the RP2040's native USB controller -- this board
 *             presenting as a keyboard and mouse to its computer.
 *   rhport 1  host, bit-banged over PIO -- the real keyboard and mouse, if
 *             they happen to be plugged into this board.
 *
 * See hid_bridge.h for why the host stack is confined to core 1.
 */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU   OPT_MCU_RP2040
#define CFG_TUSB_OS    OPT_OS_PICO

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

/* ---- device: rhport 0, native controller ---- */

#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE 64

/* Two HID interfaces: one keyboard, one mouse. Separate interfaces rather
 * than one interface with report ids, so the keyboard interface is
 * byte-for-byte a boot-protocol keyboard and a BIOS needs no special
 * handling. */
/* Keyboard, mouse, and the vendor interface the level 3 client attaches to. */
#define CFG_TUD_HID           3
/* 64 bytes because the vendor interface carries DHP_MAX_PAYLOAD-sized frames;
 * the keyboard and mouse reports are far smaller and simply do not fill it. */
#define CFG_TUD_HID_EP_BUFSIZE 64

#define CFG_TUD_CDC           0
#define CFG_TUD_MSC           0
#define CFG_TUD_MIDI          0
#define CFG_TUD_VENDOR        0

/* ---- host: rhport 1, PIO-USB ---- */

#define CFG_TUH_ENABLED       1
#define CFG_TUH_RPI_PIO_USB   1
#define CFG_TUH_MAX_SPEED     OPT_MODE_FULL_SPEED

/* A hub costs little and lets one board carry both a keyboard and a mouse on
 * a single host port, which is the common desk arrangement. */
#define CFG_TUH_HUB           1
#define CFG_TUH_HID           4
#define CFG_TUH_DEVICE_MAX    (CFG_TUH_HUB ? 4 : 1)
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#define CFG_TUH_CDC           0
#define CFG_TUH_MSC           0
#define CFG_TUH_VENDOR        0

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
