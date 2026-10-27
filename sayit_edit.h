/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SAYIT phrase editor core: pure, fixed-size text editing for the Digitone
 * overlay. No firmware calls, no allocation, no I/O: host-testable via
 * tools/sayit_host_test.py.
 *
 * The model follows the Digitone's own NAMING screen
 * (manual 6.5, "The naming screen"):
 *   LEFT/RIGHT   move the cursor between characters
 *   UP/DOWN      select the character at the cursor (the LEVEL/DATA knob
 *                does the same in the factory screen; the overlay maps the
 *                arrow keys, which every unit has)
 *   FUNC+NO      delete the character before the cursor
 *   FUNC+YES     insert a space
 *   FUNC held    the character pop-up: arrows move over the grid, releasing
 *                FUNC inserts the highlighted character
 *   YES          accept      NO  abort
 *
 * The pop-up is a 15 x 6 grid of 90 characters, the same shape as the
 * factory naming pop-up on the 128 x 64 screen. SAYIT_CHARSET is both the
 * UP/DOWN order and the grid's order, so stepping and picking agree.
 */
#ifndef SAYIT_EDIT_H
#define SAYIT_EDIT_H

#include <stdint.h>
#include "sayit_chars.h"

#define SAYIT_EDIT_MAX  256   /* incl. NUL: 255 usable chars */
#define SAYIT_GRID_COLS 15
#define SAYIT_GRID_ROWS 6

/* A phrase's maximum length. The Digitone machine keeps its phrases in project
 * data and has all 255 of them; the Digitakt machine keeps them in its sound,
 * which has room for 20 (machines/digitakt/phrase_store.h), and its build says
 * so with -DSAYIT_EDIT_LEN. */
#ifndef SAYIT_EDIT_LEN
#define SAYIT_EDIT_LEN (SAYIT_EDIT_MAX - 1)
#endif
#if SAYIT_EDIT_LEN < 1 || SAYIT_EDIT_LEN >= SAYIT_EDIT_MAX
#error "SAYIT_EDIT_LEN must leave room for the NUL"
#endif

/* SAYIT_CHARSET, its length and its position lookup are in sayit_chars.h. */

struct sayit_editor {
    char buf[SAYIT_EDIT_MAX];
    int32_t len;       /* chars, excl. NUL */
    int32_t cursor;    /* 0..len */
    int32_t grid_row;  /* pop-up selection, 0..SAYIT_GRID_ROWS-1 */
    int32_t grid_col;  /* 0..SAYIT_GRID_COLS-1 */
};

static void sayit_ed_init(struct sayit_editor *e)
{
    int32_t i;
    if (!e)
        return;
    for (i = 0; i < SAYIT_EDIT_MAX; i++)
        e->buf[i] = 0;
    e->len = 0;
    e->cursor = 0;
    e->grid_row = 0;
    e->grid_col = 0;
}

static void sayit_ed_set(struct sayit_editor *e, const char *p)
{
    int32_t i = 0;
    if (!e)
        return;
    sayit_ed_init(e);
    if (!p)
        return;
    while (p[i] && i < SAYIT_EDIT_LEN) {
        char c = p[i];
        e->buf[i] = (c >= 32 && c < 127) ? c : ' ';
        i++;
    }
    e->buf[i] = 0;
    e->len = i;
    e->cursor = i;
}

static int32_t sayit_ed_insert(struct sayit_editor *e, char c)
{
    int32_t i;
    if (!e || e->len >= SAYIT_EDIT_LEN)
        return -1;
    if (c < 32 || c > 126)
        return -1;
    if (e->cursor < 0)
        e->cursor = 0;
    if (e->cursor > e->len)
        e->cursor = e->len;
    for (i = e->len; i > e->cursor; i--)
        e->buf[i] = e->buf[i - 1];
    e->buf[e->cursor] = c;
    e->cursor++;
    e->len++;
    e->buf[e->len] = 0;
    return 0;
}

/* Erase the char before the cursor. */
static int32_t sayit_ed_backspace(struct sayit_editor *e)
{
    int32_t i;
    if (!e || e->cursor <= 0)
        return -1;
    if (e->cursor > e->len)
        e->cursor = e->len;
    for (i = e->cursor - 1; i < e->len; i++)
        e->buf[i] = e->buf[i + 1];
    e->cursor--;
    e->len--;
    return 0;
}

/* Erase the char at the cursor (stays put). */
static int32_t sayit_ed_delete_at(struct sayit_editor *e)
{
    int32_t i;
    if (!e || e->cursor < 0 || e->cursor >= e->len)
        return -1;
    for (i = e->cursor; i < e->len; i++)
        e->buf[i] = e->buf[i + 1];
    e->len--;
    return 0;
}

static void sayit_ed_move(struct sayit_editor *e, int32_t delta)
{
    if (!e)
        return;
    if (delta > 64)
        delta = 64;
    if (delta < -64)
        delta = -64;
    e->cursor += delta;
    if (e->cursor < 0)
        e->cursor = 0;
    if (e->cursor > e->len)
        e->cursor = e->len;
}

/* Step the char at the cursor through the charset (delta may be negative).
 * At end-of-text a forward step appends one character and leaves the cursor
 * on it, so further steps cycle that character instead of piling up new
 * ones: turning LEVEL/DATA at the end types a single character, exactly as
 * editing any other position does. A backward step at end-of-text does
 * nothing. Chars outside the set are normalised to 'A' first. Bounded:
 * |delta| is clamped to 64 steps. Returns 0 on change, -1 when the buffer
 * is full and the step would append. */
static int32_t sayit_ed_step(struct sayit_editor *e, int32_t delta)
{
    int32_t pos;
    if (!e)
        return -1;
    if (delta == 0)
        return 0;
    if (delta > 64)
        delta = 64;
    if (delta < -64)
        delta = -64;
    if (e->cursor < 0)
        e->cursor = 0;
    if (e->cursor > e->len)
        e->cursor = e->len;
    if (e->cursor == e->len) {
        if (delta < 0)
            return 0;
        if (e->len >= SAYIT_EDIT_LEN)
            return -1;
        e->buf[e->len] = SAYIT_CHARSET[0];
        e->len++;
        e->buf[e->len] = 0;
        e->cursor = e->len - 1;   /* stay on the new char: steps cycle it */
        pos = 0;
        delta--;
        if (delta == 0)
            return 0;
    } else {
        pos = sayit_ed_charset_pos(e->buf[e->cursor]);
        if (pos < 0)
            pos = 0;
    }
    pos = (pos + delta) % SAYIT_CHARSET_N;
    if (pos < 0)
        pos += SAYIT_CHARSET_N;
    e->buf[e->cursor] = SAYIT_CHARSET[pos];
    return 0;
}

/* Insert a space at the cursor (FUNC+YES). */
static int32_t sayit_ed_space(struct sayit_editor *e)
{
    return sayit_ed_insert(e, ' ');
}

/* ---- pop-up grid -------------------------------------------------------- */

static char sayit_ed_grid_char(int32_t row, int32_t col)
{
    int32_t i;
    if (row < 0 || row >= SAYIT_GRID_ROWS || col < 0 || col >= SAYIT_GRID_COLS)
        return 0;
    i = row * SAYIT_GRID_COLS + col;
    if (i >= SAYIT_CHARSET_N)
        return 0;
    return SAYIT_CHARSET[i];
}

/* Put the pop-up cursor on c when it is in the grid, else on 'A'. */
static void sayit_ed_grid_open(struct sayit_editor *e)
{
    int32_t pos = 0;
    if (!e)
        return;
    if (e->cursor >= 0 && e->cursor < e->len)
        pos = sayit_ed_charset_pos(e->buf[e->cursor]);
    if (pos < 0)
        pos = 0;
    e->grid_row = pos / SAYIT_GRID_COLS;
    e->grid_col = pos % SAYIT_GRID_COLS;
}

static void sayit_ed_grid_move(struct sayit_editor *e, int32_t dx, int32_t dy)
{
    if (!e)
        return;
    e->grid_col += dx;
    e->grid_row += dy;
    while (e->grid_col < 0)
        e->grid_col += SAYIT_GRID_COLS;
    while (e->grid_col >= SAYIT_GRID_COLS)
        e->grid_col -= SAYIT_GRID_COLS;
    while (e->grid_row < 0)
        e->grid_row += SAYIT_GRID_ROWS;
    while (e->grid_row >= SAYIT_GRID_ROWS)
        e->grid_row -= SAYIT_GRID_ROWS;
}

/* Insert the highlighted grid character at the cursor. -> 0, or -1 full. */
static int32_t sayit_ed_grid_accept(struct sayit_editor *e)
{
    char c;
    if (!e)
        return -1;
    c = sayit_ed_grid_char(e->grid_row, e->grid_col);
    if (!c)
        return -1;
    return sayit_ed_insert(e, c);
}

#endif /* SAYIT_EDIT_H */
