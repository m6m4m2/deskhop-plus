/* SPDX-License-Identifier: GPL-2.0-only */
#include "display.h"

#include <stdlib.h>
#include <string.h>

#include "font5x7.h"

void dsp_clear(dsp_t *d)
{
    memset(d->px, 0, sizeof(d->px));
    d->dirty = true;
}

static void set_col(dsp_t *d, int x, int page, uint8_t bits, bool invert)
{
    if (x < 0 || x >= DSP_W || page < 0 || page >= DSP_PAGES) {
        return;
    }
    d->px[page * DSP_W + x] = invert ? (uint8_t)~bits : bits;
}

void dsp_text(dsp_t *d, int col, int row, const char *s, bool invert)
{
    int x = col * FONT_ADVANCE;

    for (; *s && x < DSP_W; s++) {
        char c = *s;
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A'); /* the font has no lowercase */
        }
        if (c < FONT_FIRST || c > FONT_LAST) {
            c = '?';
        }

        const uint8_t *g = font5x7[c - FONT_FIRST];
        for (int i = 0; i < FONT_W; i++) {
            set_col(d, x + i, row, g[i], invert);
        }
        set_col(d, x + FONT_W, row, 0x00, invert); /* inter-character gap */
        x += FONT_ADVANCE;
    }
    d->dirty = true;
}

void dsp_hrule(dsp_t *d, int y)
{
    if (y < 0 || y >= DSP_H) {
        return;
    }
    const int page = y / 8;
    const uint8_t bit = (uint8_t)(1u << (y % 8));
    for (int x = 0; x < DSP_W; x++) {
        d->px[page * DSP_W + x] |= bit;
    }
    d->dirty = true;
}

/* Chain boards are discovered in whatever order they first speak, but the
 * display has to show them in physical order or it is worse than useless for
 * working out which machine is which. */
static int by_position(const void *a, const void *b)
{
    const dsp_board_t *x = a, *y = b;
    return (int)x->rel_pos - (int)y->rel_pos;
}

void dsp_render_status(dsp_t *d, const dsp_status_t *st)
{
    char line[DSP_COLS + 1];

    dsp_clear(d);

    /* Row 0: identity and current level, level right-aligned. */
    snprintf(line, sizeof(line), "DESKHOP+");
    dsp_text(d, 0, 0, line, false);
    snprintf(line, sizeof(line), "L%d", (int)st->level);
    dsp_text(d, DSP_COLS - 2, 0, line, false);

    dsp_hrule(d, 9);

    /* Row 2: the chain, in physical order.
     *
     * Each board is one cell. The focused board is inverted, so the one thing
     * you most often want to know -- where the keystrokes are going -- is
     * legible from across a desk without reading anything. */
    dsp_board_t sorted[16];
    const int n = st->n_boards > 16 ? 16 : st->n_boards;
    memcpy(sorted, st->board, sizeof(dsp_board_t) * (size_t)n);
    qsort(sorted, (size_t)n, sizeof(sorted[0]), by_position);

    int col = 0;
    int machine = 0; /* counts machines only: the coordinator is not one */
    for (int i = 0; i < n && col < DSP_COLS - 2; i++) {
        char cell[5];

        if (sorted[i].is_coord) {
            snprintf(cell, sizeof(cell), "C");
        } else {
            /* Number the machines from the head of the chain, which is what
             * the user counts along their desk. The coordinator sits in the
             * chain but is not a machine, so it must not consume a number --
             * otherwise the labels stop matching the desk the moment the
             * coordinator is plugged in anywhere but the end. */
            snprintf(cell, sizeof(cell), "%u", (unsigned)(++machine % 100));
        }

        dsp_text(d, col, 2, cell, sorted[i].is_focus);
        col += (int)strlen(cell);

        /* A board with the real keyboard attached gets a marker, because
         * which board the keyboard is in is the one piece of physical state
         * that is otherwise invisible. */
        if (sorted[i].has_input) {
            dsp_text(d, col, 2, "*", false);
            col += 1;
        }
        if (i + 1 < n) {
            dsp_text(d, col, 2, "-", false);
            col += 1;
        }
    }

    /* Row 3: who is routing. */
    if (st->we_are_active) {
        dsp_text(d, 0, 3, "ROUTING: THIS", false);
    } else {
        const char *s = st->state_name ? st->state_name : "?";
        snprintf(line, sizeof(line), "ROUTING: BOARD/%s", s);
        dsp_text(d, 0, 3, line, false);
    }

    /* Rows 4-5: pairing takes over the middle of the screen when active,
     * because the short authentication string is the one thing the user must
     * read off the panel and compare. */
    if (st->pairing) {
        dsp_text(d, 0, 4, "PAIRING - COMPARE:", false);
        if (st->sas) {
            snprintf(line, sizeof(line), "   %s", st->sas);
            dsp_text(d, 0, 5, line, true);
        } else {
            dsp_text(d, 0, 5, "   WAITING...", false);
        }
    } else {
        snprintf(line, sizeof(line), "LINK %lu.%luM",
                 (unsigned long)(st->link_baud / 1000000u),
                 (unsigned long)((st->link_baud / 100000u) % 10u));
        dsp_text(d, 0, 4, line, false);

        /* Clamped rather than truncated. An exact error count past a few
         * thousand tells you nothing a "9999+" does not, and the line has to
         * fit in 21 characters. */
        const unsigned long crc = st->crc_errors > 9999 ? 9999
                                                        : (unsigned long)st->crc_errors;
        const unsigned long aut = st->auth_errors > 9999 ? 9999
                                                         : (unsigned long)st->auth_errors;
        snprintf(line, sizeof(line), "CRC %lu%s AUTH %lu%s",
                 crc, st->crc_errors > 9999 ? "+" : "",
                 aut, st->auth_errors > 9999 ? "+" : "");
        dsp_text(d, 0, 5, line, false);
    }

    dsp_hrule(d, 55);

    /* Row 7: uptime, or a transient notice if there is one to show. */
    if (st->message) {
        dsp_text(d, 0, 7, st->message, false);
    } else {
        const uint32_t s = st->uptime_s;
        snprintf(line, sizeof(line), "UP %luH%02luM",
                 (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
        dsp_text(d, 0, 7, line, false);
    }
}

void dsp_dump(const dsp_t *d, FILE *out)
{
    fputc('+', out);
    for (int x = 0; x < DSP_W; x++) {
        fputc('-', out);
    }
    fputs("+\n", out);

    for (int y = 0; y < DSP_H; y++) {
        fputc('|', out);
        const int page = y / 8;
        const uint8_t bit = (uint8_t)(1u << (y % 8));
        for (int x = 0; x < DSP_W; x++) {
            fputc((d->px[page * DSP_W + x] & bit) ? '#' : ' ', out);
        }
        fputs("|\n", out);
    }

    fputc('+', out);
    for (int x = 0; x < DSP_W; x++) {
        fputc('-', out);
    }
    fputs("+\n", out);
}
