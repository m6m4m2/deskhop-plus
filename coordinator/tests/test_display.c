/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Status screen rendering, without a panel.
 *
 * The framebuffer is just memory, so the whole screen can be checked here and
 * the SSD1306 transport is the only part that needs hardware. Run with --dump
 * to see the screens as ASCII art, which is how the font and layout were
 * checked in the first place.
 */
#include <stdio.h>
#include <string.h>

#include "display.h"

static int failures, checks;

#define CHECK(cond, what)                                                      \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            printf("  FAIL %s\n", what);                                       \
        }                                                                      \
    } while (0)

static int lit_pixels(const dsp_t *d)
{
    int n = 0;
    for (int i = 0; i < DSP_FB_BYTES; i++) {
        for (int b = 0; b < 8; b++) {
            if (d->px[i] & (1u << b)) {
                n++;
            }
        }
    }
    return n;
}

/* Row `row` (one 8-pixel page) has something drawn in it. */
static bool row_used(const dsp_t *d, int row)
{
    for (int x = 0; x < DSP_W; x++) {
        if (d->px[row * DSP_W + x]) {
            return true;
        }
    }
    return false;
}

static void base_status(dsp_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->level = DHP_LEVEL_COORDINATED;
    st->link_baud = 2000000;
    st->uptime_s = 3 * 3600 + 7 * 60;
    st->state_name = "active";
    st->n_boards = 4;
    for (int i = 0; i < 4; i++) {
        st->board[i].addr = (dhp_addr_t)(i + 1);
        st->board[i].rel_pos = (int8_t)(i - 3);
    }
    st->board[3].is_coord = true;
    st->board[3].is_self = true;
    st->board[1].is_focus = true;
    st->board[1].has_input = true;
    st->we_are_active = true;
}

int main(int argc, char **argv)
{
    const bool dump = (argc > 1 && !strcmp(argv[1], "--dump"));
    dsp_t d;
    dsp_status_t st;

    printf("== display ==\n");

    /* Every row of the normal screen must carry something: a status panel with
     * a blank band in it means a field silently failed to render. */
    base_status(&st);
    dsp_render_status(&d, &st);
    if (dump) {
        dsp_dump(&d, stdout);
    }
    CHECK(row_used(&d, 0), "title row drawn");
    CHECK(row_used(&d, 1), "rule under the title drawn");
    CHECK(row_used(&d, 2), "chain row drawn");
    CHECK(row_used(&d, 3), "routing row drawn");
    CHECK(row_used(&d, 4), "link row drawn");
    CHECK(row_used(&d, 5), "counters row drawn");
    CHECK(row_used(&d, 7), "footer row drawn");

    const int normal = lit_pixels(&d);
    CHECK(normal > 200, "screen is not almost blank");
    CHECK(normal < DSP_FB_BYTES * 8 / 2, "screen is not almost solid");

    /* The focused board is drawn inverted, so removing the focus must change
     * the picture. This is the one cue that is meant to be readable across a
     * desk without reading any text. */
    dsp_t nofocus;
    st.board[1].is_focus = false;
    dsp_render_status(&nofocus, &st);
    CHECK(memcmp(d.px, nofocus.px, DSP_FB_BYTES) != 0,
          "focus highlight changes the chain row");
    CHECK(lit_pixels(&nofocus) < normal,
          "un-inverting the focused cell lights fewer pixels");

    /* Pairing replaces the middle of the screen with the string the user has
     * to compare, because that is the only thing that matters while it is up. */
    base_status(&st);
    st.pairing = true;
    st.sas = "A3F19C";
    dsp_t pairing;
    dsp_render_status(&pairing, &st);
    if (dump) {
        dsp_dump(&pairing, stdout);
    }
    CHECK(memcmp(pairing.px + 4 * DSP_W, d.px + 4 * DSP_W, DSP_W) != 0,
          "pairing screen replaces the link row");

    /* A pairing that has not yet produced a shared secret must still say
     * something, rather than showing a blank where the code will appear. */
    st.sas = NULL;
    dsp_t waiting;
    dsp_render_status(&waiting, &st);
    CHECK(row_used(&waiting, 5), "pairing without a code still draws a prompt");

    /* Counters must not overflow the 21-character line. */
    base_status(&st);
    st.crc_errors = 4000000000u;
    st.auth_errors = 123456789u;
    dsp_t huge;
    dsp_render_status(&huge, &st);
    CHECK(row_used(&huge, 5), "huge counters still render");

    /* A chain longer than the screen must not run off the edge or corrupt the
     * rows around it. */
    base_status(&st);
    st.n_boards = 16;
    for (int i = 0; i < 16; i++) {
        st.board[i].addr = (dhp_addr_t)(i + 1);
        st.board[i].rel_pos = (int8_t)(i - 8);
        st.board[i].is_coord = false;
        st.board[i].has_input = false;
    }
    dsp_t full;
    dsp_render_status(&full, &st);
    if (dump) {
        dsp_dump(&full, stdout);
    }
    CHECK(row_used(&full, 3), "row below a full chain is intact");
    CHECK(row_used(&full, 0), "row above a full chain is intact");

    /* An empty chain is a legitimate state during startup. */
    memset(&st, 0, sizeof(st));
    st.level = DHP_LEVEL_ISOLATED;
    st.state_name = "learn";
    dsp_t empty;
    dsp_render_status(&empty, &st);
    CHECK(row_used(&empty, 0), "empty chain still draws a title");

    /* Lowercase folds to uppercase rather than rendering as '?'. */
    dsp_t lower, upper;
    dsp_clear(&lower);
    dsp_clear(&upper);
    dsp_text(&lower, 0, 0, "active", false);
    dsp_text(&upper, 0, 0, "ACTIVE", false);
    CHECK(memcmp(lower.px, upper.px, DSP_FB_BYTES) == 0,
          "lowercase folds to uppercase");

    /* Text must clip at the right edge rather than wrapping into the next
     * page, which would corrupt the row below. */
    dsp_t clip;
    dsp_clear(&clip);
    dsp_text(&clip, 0, 3,
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", false);
    CHECK(!row_used(&clip, 4), "over-long text does not spill into the next row");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
