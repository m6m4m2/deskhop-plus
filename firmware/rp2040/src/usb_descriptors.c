/* SPDX-License-Identifier: GPL-2.0-only
 *
 * USB identity and HID descriptors for the device side.
 *
 * Identity is deliberately fixed and distinctive rather than cloning some
 * other vendor's ids. This is a device that sits between a keyboard and every
 * machine you own; it should be trivially identifiable in lsusb and trivially
 * allow-listable by any endpoint-security policy that cares. Covert would be
 * the wrong design, and a cloned VID/PID would make it impossible to write a
 * policy that distinguishes this from whatever it was pretending to be.
 *
 * The VID below is the pid.codes community allocation. The PID is a
 * placeholder and must be registered at https://pid.codes before anything is
 * distributed -- shipping an unregistered PID under that VID is exactly the
 * kind of collision the registry exists to prevent.
 */
#include "tusb.h"

#define DHP_VID 0x1209 /* pid.codes */
#define DHP_PID 0x0001 /* PLACEHOLDER -- register before distributing */

#define DHP_BCD_DEVICE 0x0100

enum {
    ITF_KEYBOARD = 0,
    ITF_MOUSE,
    /* A vendor-usage HID interface for the level 3 client.
     *
     * HID rather than CDC or a bulk vendor interface because it needs no
     * driver anywhere: Linux exposes it as hidraw, macOS and Windows both
     * bind HID natively. A device that sits between a keyboard and every
     * machine you own should not also ask each of them to install something. */
    ITF_VENDOR,
    ITF_COUNT,
};

/* Report ids are not used: each interface carries one report type, so the
 * keyboard interface is byte-for-byte a boot-protocol keyboard. A BIOS that
 * only understands boot protocol therefore works with no special handling,
 * which is the entire reason for presenting as HID in the first place. */

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = DHP_VID,
    .idProduct = DHP_PID,
    .bcdDevice = DHP_BCD_DEVICE,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&desc_device;
}

/* --- report descriptors --- */

static const uint8_t desc_hid_keyboard[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

/* 64 bytes each way, matching DHP_MAX_PAYLOAD and the largest a full-speed
 * interrupt endpoint carries. */
static const uint8_t desc_hid_vendor[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(64),
};

static const uint8_t desc_hid_mouse[] = {
    /* Relative, deliberately. The pointer design never models the screen, so
     * unlike upstream DeskHop there is no absolute-coordinate descriptor here
     * and nothing that needs to know a resolution. See docs/protocols/pointer.md */
    TUD_HID_REPORT_DESC_MOUSE(),
};

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    switch (instance) {
    case ITF_KEYBOARD: return desc_hid_keyboard;
    case ITF_MOUSE:    return desc_hid_mouse;
    default:           return desc_hid_vendor;
    }
}

/* --- configuration --- */

#define CONFIG_TOTAL_LEN                                                       \
    (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

#define EPNUM_KEYBOARD    0x81
#define EPNUM_MOUSE       0x82
#define EPNUM_VENDOR_OUT  0x03
#define EPNUM_VENDOR_IN   0x83

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* 1 ms polling on both: the point of the device is that it does not add
     * perceptible latency to the keyboard. */
    TUD_HID_DESCRIPTOR(ITF_KEYBOARD, 0, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_keyboard), EPNUM_KEYBOARD,
                       CFG_TUD_HID_EP_BUFSIZE, 1),
    TUD_HID_DESCRIPTOR(ITF_MOUSE, 0, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(desc_hid_mouse), EPNUM_MOUSE,
                       CFG_TUD_HID_EP_BUFSIZE, 1),

    /* Protocol NONE, so no operating system mistakes this for a keyboard and
     * starts delivering keystrokes to it. 1 ms polling: this is the level 3
     * bottleneck, so there is no reason to ask for less. */
    TUD_HID_INOUT_DESCRIPTOR(ITF_VENDOR, 0, HID_ITF_PROTOCOL_NONE,
                             sizeof(desc_hid_vendor), EPNUM_VENDOR_OUT,
                             EPNUM_VENDOR_IN, CFG_TUD_HID_EP_BUFSIZE, 1),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

/* --- strings --- */

static const char *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, /* 0: en-US */
    "DeskHop+",                 /* 1: manufacturer */
    "DeskHop+ Chain Board",     /* 2: product */
    NULL,                       /* 3: serial, filled from the flash uid */
};

static uint16_t _desc_str[33];
static char     _serial[17];

/* The serial is the board's flash unique id in hex. It makes each board
 * individually identifiable to a host, which is what allows a per-board
 * allow-list rather than a per-model one. */
static const char *serial_string(void)
{
    static const char hex[] = "0123456789ABCDEF";
    uint8_t id[8];
    extern void board_uid_bytes(uint8_t out[8]); /* board.c */
    board_uid_bytes(id);

    for (int i = 0; i < 8; i++) {
        _serial[i * 2] = hex[(id[i] >> 4) & 0xF];
        _serial[i * 2 + 1] = hex[id[i] & 0xF];
    }
    _serial[16] = '\0';
    return _serial;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    size_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
            return NULL;
        }
        const char *str = (index == 3) ? serial_string() : string_desc_arr[index];

        chr_count = strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
