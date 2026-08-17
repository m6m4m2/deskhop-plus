/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Hardware glue for the RP2040 chain board.
 *
 * Everything platform-specific lives behind this header. core/ knows nothing
 * about pins, clocks or USB, which is what lets the same translation units be
 * compiled for the host test build.
 */
#ifndef DHP_BOARD_H
#define DHP_BOARD_H

#include "dhp/types.h"

/* --- pin assignment; see docs/hardware.md --- */
#define PIN_PIO_USB_DP   0   /* D- must be DP+1: the PIO program requires it */
#define PIN_UART1_TX     4   /* down port, toward the tail */
#define PIN_UART1_RX     5
#define PIN_UART0_TX    12   /* up port, toward the head */
#define PIN_UART0_RX    13
#define PIN_BTN_PAIR    14
#define PIN_BTN_SWITCH  15
#define PIN_WS2812      16
#define PIN_I2C_SDA     26
#define PIN_I2C_SCL     27

/* Chain-wide. Set by the coordinator's capability, not by the boards: the Pi
 * Zero's PL011 is the limiting part and every board must agree.
 * See docs/hardware.md for the latency arithmetic. */
#define DHP_LINK_BAUD 2000000

void board_init(void);

/* Milliseconds since boot. */
dhp_time_t board_now_ms(void);

/* The 64-bit flash unique id. This is the board's permanent identity: the
 * election's tiebreak and the pairing transcript both depend on it being
 * genuinely unique per board, which the flash id is and a random value
 * generated at boot is not. */
dhp_uid_t board_uid(void);

/* Short wire address, derived from the uid. Collisions across a 16-board chain
 * are vanishingly unlikely but not impossible, so the coordinator reports one
 * if it ever sees the same address from two different uids. */
dhp_addr_t board_addr(void);

/* --- link ports --- */

/* Queue bytes for transmission. Non-blocking: writes go to a ring buffer and
 * are drained by DMA, because a blocking UART write in the HID path would add
 * jitter to every keystroke. */
void board_uart_write(dhp_port_t port, const uint8_t *data, size_t len);

/* Pop one received byte. Returns false when the port is empty. */
bool board_uart_read(dhp_port_t port, uint8_t *out);

/* --- controls --- */

/* Edge-triggered: true once per press. */
bool board_switch_pressed(void);

/* True while the pairing button has been held for the required time. Holding
 * rather than clicking is deliberate -- pairing should not be reachable by
 * brushing against the board. */
bool board_pair_held(void);

/* --- indication --- */

/* Focus and role are independent -- a board can hold the routing role while
 * the user is typing on a different machine, or both at once -- so they get
 * distinct colours rather than one overwriting the other. Being able to see
 * which board is routing while watching the user's focus move is most of the
 * value of having an LED at all. */
typedef enum {
    LED_OFF,            /* on the chain, neither focused nor routing */
    LED_FOCUSED,        /* white:  this machine has the user */
    LED_ACTIVE,         /* blue:   this board routes; the user is elsewhere */
    LED_ACTIVE_FOCUSED, /* cyan:   routes and has the user */
    LED_STANDBY,        /* dim blue: pre-elected backup */
    LED_PAIRING,        /* amber, breathing */
    LED_UNPAIRED,       /* amber, dim steady: no chain key */
    LED_ERROR,          /* red, fast blink */
} led_state_t;

/* Cheap: stores the state and returns. Safe from any hook or callback. */
void board_led(led_state_t s);

/* Renders. Call from the core 0 main loop; it rate-limits itself and drives
 * the breathing and blinking animations. */
void board_led_task(void);

/* Claims a PIO state machine for the LED.
 *
 * Must be called from core 1 AFTER PIO-USB has claimed what it needs. In host
 * mode PIO-USB uses state machines in *both* PIO blocks, so the LED can only
 * have whatever is left over -- and if nothing is, it does without rather than
 * taking a resource USB needs. Indication is a convenience; being a keyboard
 * is not. */
void board_led_hw_init(void);

/* Called once ON core 1 before it enters its loop. Registers core 1 as a
 * lockout victim so that core 0 can park it while writing flash -- without
 * this, saving the chain key crashes the board. */
void board_flash_lockout_ready(void);

/* --- persistent chain key ---
 *
 * Stored in the last flash sector. Returns false when the board has never been
 * paired, which is not an error: an unpaired board comes up in level 0 and
 * serves its own machine perfectly well while waiting to be paired. */
bool board_key_load(uint8_t key[16]);
void board_key_save(const uint8_t key[16]);
void board_key_erase(void);

#endif /* DHP_BOARD_H */
