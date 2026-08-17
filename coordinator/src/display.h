/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The level 2 status display: a 128x64 framebuffer and the status screen
 * drawn into it.
 *
 * Rendering is deliberately separated from the SSD1306 transport. The
 * framebuffer is just memory, so the whole screen can be rendered and checked
 * on a development machine -- tests/test_display.c dumps it as ASCII art --
 * without an I2C bus or a panel present. It also means --headless is a real
 * mode rather than a stub: the daemon renders exactly the same screen and
 * simply does not push it anywhere.
 */
#ifndef DHP_DISPLAY_H
#define DHP_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "dhp/level.h"
#include "dhp/types.h"

#define DSP_W 128
#define DSP_H 64
#define DSP_PAGES (DSP_H / 8)
#define DSP_FB_BYTES (DSP_W * DSP_PAGES)

#define DSP_COLS (DSP_W / 6) /* 21 characters across */
#define DSP_ROWS DSP_PAGES   /* 8 rows of text */

typedef struct {
    uint8_t px[DSP_FB_BYTES]; /* SSD1306 page order: 8 pages of 128 columns */
    bool    dirty;
} dsp_t;

/* One board as the display understands it. */
typedef struct {
    dhp_addr_t addr;
    int8_t     rel_pos;
    bool       is_self;
    bool       is_active;
    bool       is_focus;
    bool       has_input;   /* the real keyboard is on this one */
    bool       is_coord;
} dsp_board_t;

typedef struct {
    dhp_level_t level;
    dsp_board_t board[16];
    int         n_boards;
    bool        we_are_active;
    const char *state_name;   /* our UHRP state */
    uint32_t    link_baud;
    uint32_t    crc_errors;
    uint32_t    auth_errors;
    uint32_t    uptime_s;
    bool        pairing;
    const char *sas;          /* six hex digits while pairing, else NULL */
    const char *message;      /* transient notice, else NULL */
} dsp_status_t;

void dsp_clear(dsp_t *d);

/* Text at character column `col`, text row `row` (each row is one 8-pixel
 * page). Lowercase is folded to uppercase; the font has no lowercase. */
void dsp_text(dsp_t *d, int col, int row, const char *s, bool invert);

/* Horizontal rule across the whole width, at the given pixel row. */
void dsp_hrule(dsp_t *d, int y);

/* Draw the whole status screen. This is the only caller-facing composition
 * function; everything above it exists to serve this. */
void dsp_render_status(dsp_t *d, const dsp_status_t *st);

/* Dump as ASCII art, for tests and for --headless --verbose. */
void dsp_dump(const dsp_t *d, FILE *out);

#endif /* DHP_DISPLAY_H */
