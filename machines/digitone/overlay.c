/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SAYIT overlay: the factory-style phrase naming screen, the SAYIT pages and
 * the phrase's project data.
 *
 * The naming screen follows the Digitone's own NAMING screen (manual 6.5):
 * LEFT/RIGHT move the cursor, UP/DOWN select the character at the cursor,
 * FUNC+NO deletes, FUNC+YES inserts a space, holding FUNC opens the 15 x 6
 * character pop-up (arrows move over it, releasing FUNC inserts), YES saves
 * and NO aborts. The overlay draws it with the stock fonts and the same
 * full-screen modal feel; no firmware address is invented.
 *
 * The SYN1/SYN2 first pages are redrawn as SAYIT pages: the phrase and token
 * strip fill the upper half and the page's four (or three) controls sit in
 * the lower encoder band, each under its own encoder. YES on a SAYIT page
 * opens the naming screen -- that is the only way in: the mod adds no Mod
 * Menu entry of its own, so it belongs under MACHINES and nowhere else.
 *
 * Gating follows the proven VAWT recipe: paint only when the active track's
 * sound plays SAYIT, the showing key is SYN1/SYN2, no screen sits over the
 * pages, and the shown view is on its first page. The showing key is read as
 * the long after fw_active_track (VAWT pins it at 0x41368ce4 on 1.44, i.e.
 * fw_active_track + 4, which is why no address is named here); the brain/screen
 * chain uses the same offsets core_page_shown uses. No invented firmware
 * address, no new code site: fw_* exports plus this verified layout,
 * confirmed in the emulator.
 */
#include <stdint.h>
#include "digitone-mk1/machines.h"
#include "../../sayit.h"
#include "../../sayit_edit.h"
#include "dn_knobs.h"

#define SAYIT_ID 77

/* From engine.c (same mod, `sayit_` prefixed). */
/* The phrase, the engine and the last token are per track (SAY IT is its own
 * instrument on each track), so every accessor is asked for a track. */
extern void sayit_set_phrase(int32_t track, const char *p);
extern const char *sayit_get_phrase(int32_t track);
extern int32_t sayit_get_last_token(int32_t track);
extern int32_t sayit_get_last_count(int32_t track);
extern int32_t sayit_get_last_note(int32_t track);
extern int32_t sayit_ui_knob(int32_t k);
extern int32_t sayit_knob_slot(int32_t k);
extern int32_t sayit_fmt_knob(int32_t k, int32_t value, char *buf);

#define KEY_FUNC   1
#define KEY_PTN    4
#define KEY_RECORD 10
#define KEY_PLAY   11
#define KEY_STOP   12
#define KEY_YES    13
#define KEY_NO     14
#define KEY_UP     15
#define KEY_DOWN   16
#define KEY_LEFT   17
#define KEY_RIGHT  18
#define KEY_PAGE   19

/* Brain/screen chain (same offsets core_page_shown uses). */
#define BRAIN_BOTTOM 0x54
#define BRAIN_TOP    0x5c
#define SCREEN_MGR   8
#define MGR_VIEW     0xc0
#define VIEW_CUR     144

#define FONT5 ((const void *)fw_font5)
#define FONTL ((const void *)fw_font_label)
#define FONTT ((const void *)fw_font_title)

/* fw_textf's y is the bottom row of the glyph; ytop is the top row on the
 * 64-row screen. h is the font's height (font5/label 5, title 6). */
#define TY(yt, h) (63 - (yt) - (h) + 1)

/* The page's content column: the stock header puts the first lit pixel of the
 * track name (the U of UNTITLED) at x=18. fw_textf's x is where the glyph's
 * ink starts -- font5's and font_label's first column is part of the glyph,
 * not a bearing (the label 'S' puts its left stem on x, its top bar on x+1)
 * -- so text, frames and the rule all use 18. Moving only the cells leaves
 * the text 1 px and the rule 3 px to their left. */
#define SAYIT_X      18
#define SAYIT_CELL_X 18

#define SAYIT_MSG_HOLD 90u
#define SAYIT_TOAST_HOLD 45
#define SAYIT_SAVE_VER 2     /* 2: a phrase per track (1: one for all) */
#define SAYIT_SAVE_VER_OLD 1
#define SAYIT_SAVE_PHRASE (SAYIT_PHRASE_MAX + 8)   /* 264, a multiple of 4 */
#define SAYIT_NAME_COLS 21   /* 5 px cells that fit the 118 px name field */
#define SAYIT_ENC_COUNTS 4   /* encoder counts per detent */

/* One phrase per track. Version 1 of the blob held a single phrase at the
 * same offset, so it still reads: every track takes that phrase, which is
 * what those tracks were playing. */
struct sayit_save {
    uint8_t ver;
    uint8_t pad[3];
    char phrase[SAYIT_TRACKS][SAYIT_SAVE_PHRASE];
} __attribute__((aligned(4)));

static void *sayit_brain = 0;
static uint32_t sayit_ticks = 0;
static struct sayit_editor sayit_ed;
static int32_t sayit_ed_live = 0;      /* buffer initialised from engine */
static int32_t sayit_editor_open = 0;  /* modal naming screen */
static int32_t sayit_popup = 0;        /* FUNC-held character grid */
static int32_t sayit_func_action = 0;  /* a FUNC chord ran: no insert on release */
static int32_t sayit_func = 0;
static int32_t sayit_ptn = 0;
static int32_t sayit_page = 0;
static int32_t sayit_rec = 0;
static int32_t sayit_toast = 0;
static int32_t sayit_shown_tok = -1;
static int32_t sayit_shown_n = 0;
static int32_t sayit_shown_note = -2;
static int32_t sayit_shown_track = -1;  /* track the shown token belongs to */
static uint32_t sayit_repaint = 0;    /* ticks to keep re-arming the page */
static int32_t sayit_ed_track = -1;   /* track the editor was opened on */
static int32_t sayit_swallow = -1;    /* YES/NO whose release must not reach the stock */
static uint32_t sayit_swallow_until = 0;
static uint32_t sayit_msg_until = 0;
static uint32_t sayit_sig = 0;
static int32_t sayit_sig_valid = 0;
static int32_t sayit_enc_acc = 0;
static struct sayit_save sayit_save;

/* Held sequencer-step keys, one bit a trig (key ids 26-41). While one is
 * held the page shows that step's parameter locks. The keys are tracked here
 * rather than read from firmware RAM: the Mod Menu eats keys before stock and
 * mods see them, so a press it eats moves neither side and the bits cannot
 * desync. Page keys TRIG..LFO are ids 20-25; a page-key press clears the mask
 * (VAWT's recipe). No record mode gates this: a hold always shows the step's
 * values. Gating it on GRID/STEP does not work -- the mode a hold is in is not
 * visible to a mod, and a chorded RECORD+PLAY leaves such a gate shut. */
static uint32_t sayit_trigs;

/* ---- small helpers ------------------------------------------------------ */

static void sayit_str_copy(char *dst, int32_t dstsz, const char *src, int32_t max)
{
    int32_t i = 0;
    if (!dst || dstsz <= 0)
        return;
    if (!src)
        src = "";
    while (src[i] && i < dstsz - 1 && i < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* The showing key (1 SYN1, 2 SYN2): the long after fw_active_track. */
static int32_t sayit_showing_key(void)
{
    return *(volatile int32_t *)((volatile uint8_t *)&fw_active_track + 4);
}

/* A track index the project data can hold (fw_active_track is 0-3). */
static int32_t sayit_track_of(int32_t t)
{
    return (t < 0 || t >= SAYIT_TRACKS) ? 0 : t;
}

static int32_t sayit_is_sayit_track(void)
{
    uint8_t *kit = fw_kit;
    int32_t t = fw_active_track;
    if (!kit || t < 0 || t > 3)
        return 0;
    return (FW_SOUND_SLOT(kit, t, 0) >> 8) == SAYIT_ID;
}

/* No menu, browser or other screen over the pages. */
static int32_t sayit_no_menu(void)
{
    void *bottom, *top, *mgr, *view;
    if (!sayit_brain)
        return 0;
    bottom = *(void *volatile *)((uint8_t *)sayit_brain + BRAIN_BOTTOM);
    top = *(void *volatile *)((uint8_t *)sayit_brain + BRAIN_TOP);
    if (!bottom || bottom != top)
        return 0;
    mgr = *(void *volatile *)((uint8_t *)bottom + SCREEN_MGR);
    if (!mgr)
        return 0;
    view = *(void *volatile *)((uint8_t *)mgr + MGR_VIEW);
    return view != 0;
}

/* No screen over the pages and the shown view on its first page. */
static int32_t sayit_page_first(void)
{
    void *bottom, *top, *mgr, *view;
    int32_t pos;
    if (!sayit_no_menu())
        return 0;
    bottom = *(void *volatile *)((uint8_t *)sayit_brain + BRAIN_BOTTOM);
    top = *(void *volatile *)((uint8_t *)sayit_brain + BRAIN_TOP);
    mgr = *(void *volatile *)((uint8_t *)bottom + SCREEN_MGR);
    view = *(void *volatile *)((uint8_t *)mgr + MGR_VIEW);
    pos = *(volatile int32_t *)((uint8_t *)view + VIEW_CUR);
    return pos == 0;
}

static uint32_t sayit_hash(const char *s, int32_t len)
{
    uint32_t h = 0;
    int32_t i;
    for (i = 0; i < len; i++)
        h = h * 33u + (uint8_t)s[i];
    return h;
}

/* The track the UI is speaking for: the editor's while it is up (it edits
 * the track it was opened on), otherwise the active track. */
static int32_t sayit_cur_track(void)
{
    if (sayit_editor_open && sayit_ed_track >= 0)
        return sayit_ed_track;
    return fw_active_track;
}

/* True while the buffer differs from what the engine plays. */
static int32_t sayit_dirty(void)
{
    const char *p = sayit_get_phrase(sayit_cur_track());
    int32_t i;
    if (!sayit_ed_live)
        return 0;
    for (i = 0; i <= sayit_ed.len; i++) {
        if (sayit_ed.buf[i] != p[i])
            return 1;
        if (p[i] == 0)
            break;
    }
    return p[i] != 0;
}

/* ---- project data ------------------------------------------------------- */

static void sayit_save_phrase(void)
{
    int32_t t, i;
    sayit_save.ver = SAYIT_SAVE_VER;
    for (t = 0; t < SAYIT_TRACKS; t++) {
        for (i = 0; i < SAYIT_SAVE_PHRASE; i++)
            sayit_save.phrase[t][i] = 0;
        sayit_str_copy(sayit_save.phrase[t], SAYIT_SAVE_PHRASE,
                       sayit_get_phrase(t), SAYIT_EDIT_MAX - 1);
    }
}

static void sayit_loaded(int32_t found)
{
    int32_t t;
    if (found && sayit_save.ver == SAYIT_SAVE_VER) {
        for (t = 0; t < SAYIT_TRACKS; t++) {
            sayit_save.phrase[t][SAYIT_SAVE_PHRASE - 1] = 0;
            sayit_set_phrase(t, sayit_save.phrase[t]);
        }
    } else if (found && sayit_save.ver == SAYIT_SAVE_VER_OLD) {
        /* Version 1 had one phrase for the whole project. Give it to every
         * track: that is what each of them was already speaking. */
        sayit_save.phrase[0][SAYIT_SAVE_PHRASE - 1] = 0;
        for (t = 0; t < SAYIT_TRACKS; t++)
            sayit_set_phrase(t, sayit_save.phrase[0]);
        sayit_save_phrase();
    } else {
        for (t = 0; t < SAYIT_TRACKS; t++)
            sayit_set_phrase(t, "say it");
        sayit_save_phrase();
    }
    sayit_editor_open = 0;
    sayit_popup = 0;
    sayit_ed_live = 0;
    sayit_enc_acc = 0;
    sayit_sig_valid = 0;
}

const struct core_projdata sayit_projdata = {
    0x53415949u /* "SAYI" */, sizeof(struct sayit_save), &sayit_save,
    sayit_loaded
};

/* ---- the naming screen -------------------------------------------------- */

static void sayit_open_editor(void)
{
    sayit_ed_track = sayit_cur_track();
    sayit_ed_set(&sayit_ed, sayit_get_phrase(sayit_ed_track));
    sayit_ed_live = 1;
    sayit_editor_open = 1;
    sayit_popup = 0;
    sayit_func_action = 0;
    sayit_enc_acc = 0;
    sayit_sig_valid = 0;
    /* Keys pressed on the way here were taken by the editor, so their
     * releases never reach the page handler. Start from a clean slate or a
     * stale edge state would refuse the next bare YES. */
    sayit_func = 0;
    sayit_ptn = 0;
    sayit_page = 0;
    sayit_rec = 0;
    sayit_trigs = 0;
}

static void sayit_close_editor(int32_t save)
{
    int32_t t = sayit_ed_track >= 0 ? sayit_ed_track : fw_active_track;
    if (save) {
        sayit_set_phrase(t, sayit_ed.buf);
        sayit_save_phrase();
    } else {
        sayit_ed_set(&sayit_ed, sayit_get_phrase(t));
    }
    sayit_editor_open = 0;
    sayit_popup = 0;
    sayit_func_action = 0;
    sayit_enc_acc = 0;
    sayit_ed_live = 1;
    sayit_sig_valid = 0;
    /* The stock may reload a track's sound across a modal screen; a voice
     * keeps the sound it loaded last until core_sound_set writes it again.
     * Put this machine back on the track the editor was opened on, so the
     * page gate and the voices agree whether or not that reload happened. */
    if (sayit_ed_track >= 0 && sayit_ed_track <= 3 && fw_kit)
        core_sound_set(sayit_ed_track, 0, SAYIT_ID << 8);
    sayit_ed_track = -1;
    /* The editor took every key but PLAY/STOP, releases included: clear the
     * chord states so a later bare YES opens the editor again. */
    sayit_func = 0;
    sayit_ptn = 0;
    sayit_page = 0;
    sayit_rec = 0;
    sayit_trigs = 0;
    /* The stock may repaint once more after YES/NO (a save, a view change).
     * Keep asking for frames for a while so the page, not the stock page, is
     * what stays on screen. */
    sayit_repaint = 90;
}

/* Build the visible slice of the name around the cursor, with < > markers.
 * The slice is at most SAYIT_NAME_COLS glyphs, so a long phrase cannot spill
 * over the name field. */
static void sayit_name_line(char *out, int32_t outsz)
{
    int32_t start, i, k = 0, maxchars = SAYIT_NAME_COLS - 1;
    if (!out || outsz < 4)
        return;
    start = sayit_ed.cursor - SAYIT_NAME_COLS / 2;
    if (start < 0)
        start = 0;
    if (start > 0) {
        out[k++] = '<';
        maxchars--;
    }
    for (i = start; i < sayit_ed.len && k < maxchars; i++)
        out[k++] = sayit_ed.buf[i];
    if (i < sayit_ed.len)
        out[k++] = '>';
    out[k] = 0;
}

/* Draw three rows of the 15 x 6 character grid around the selection. The
 * factory pop-up is a three-row window too; showing all six rows at once is
 * too dense for the 5-pixel font. */
static void sayit_draw_grid(void *bmp)
{
    int32_t vr, r, c, first;
    int32_t gx = 4, gy = 26, cw = 8, ch = 10;

    first = sayit_ed.grid_row - 1;
    if (first < 0)
        first = 0;
    if (first > SAYIT_GRID_ROWS - 3)
        first = SAYIT_GRID_ROWS - 3;
    for (vr = 0; vr < 3; vr++) {
        r = first + vr;
        for (c = 0; c < SAYIT_GRID_COLS; c++) {
            char chv = sayit_ed_grid_char(r, c);
            int32_t x = gx + c * cw;
            int32_t ytop = gy + vr * ch;
            if (chv == ' ') {
                /* the space slot: a short baseline, so it is visible */
                fw_fillrect(bmp, x + 2, 63 - (ytop + 5), x + 5,
                            63 - (ytop + 5), 1);
            } else if (chv) {
                fw_textf(bmp, FONT5, x + 2, TY(ytop + 1, 5), -1, "%c", chv);
            }
            if (r == sayit_ed.grid_row && c == sayit_ed.grid_col) {
                fw_framerect(bmp, x, 63 - (ytop + 7), x + cw - 1,
                             63 - ytop + 1, 1);
            }
        }
    }
    /* show where the window is in the six rows */
    fw_textf(bmp, FONT5, 104, TY(59, 5), -1, "%d/%d",
             (int)sayit_ed.grid_row + 1, SAYIT_GRID_ROWS);
}

static void sayit_draw_name_screen(void *bmp)
{
    char line[28];
    int32_t n, x;
    const char *p = sayit_get_phrase(sayit_cur_track());

    fw_fillrect(bmp, 0, 0, 127, 63, 0);
    /* title bar */
    fw_framerect(bmp, 0, 54, 127, 63, 1);
    fw_textf(bmp, FONTT, 31, TY(2, 6), -1, "PHRASE NAME");

    /* name field */
    fw_framerect(bmp, 2, 41, 125, 53, 1);
    sayit_name_line(line, sizeof(line));
    /* One glyph per 5 px cell, so the factory-style cursor frame sits on
     * the character it edits. The title font is proportional: a single
     * fw_textf("%s") would leave the frame at a guessed offset. */
    {
        int32_t start = sayit_ed.cursor - SAYIT_NAME_COLS / 2;
        int32_t col, i;
        if (start < 0)
            start = 0;
        for (i = 0; line[i]; i++)
            fw_textf(bmp, FONTT, 5 + i * 5, TY(13, 6), -1, "%c", line[i]);
        col = sayit_ed.cursor - start + (start > 0 ? 1 : 0);
        if (col > SAYIT_NAME_COLS) col = SAYIT_NAME_COLS;
        x = 5 + col * 5;
        if (x > 111) x = 111;
        fw_framerect(bmp, x - 1, 43, x + 4, 52, 1);  /* cursor frame */
    }

    /* token count for the status column */
    {
        uint16_t offs[SAYIT_TOKENS_MAX];
        uint8_t lens[SAYIT_TOKENS_MAX];
        n = sayit_tokenise(p, sayit_map_mode(sayit_ui_knob(K_MODE)), offs,
                           lens, SAYIT_TOKENS_MAX);
    }

    if (sayit_popup) {
        sayit_draw_grid(bmp);
        fw_textf(bmp, FONT5, 3, TY(59, 5), -1, "RELEASE FUNC TO INSERT");
    } else {
        fw_textf(bmp, FONT5, 3, TY(25, 5), -1, "FUNC:GRID");
        fw_textf(bmp, FONT5, 68, TY(25, 5), -1, "U/D:CHAR");
        fw_textf(bmp, FONT5, 3, TY(32, 5), -1, "FUNC+NO:DEL");
        fw_textf(bmp, FONT5, 68, TY(32, 5), -1, "L/R:MOVE");
        fw_textf(bmp, FONT5, 3, TY(39, 5), -1, "FUNC+YES:SPACE");
        fw_textf(bmp, FONT5, 68, TY(39, 5), -1, "YES:SAVE");
        fw_textf(bmp, FONT5, 3, TY(46, 5), -1, "NO:ABORT");
        fw_textf(bmp, FONT5, 68, TY(46, 5), -1, "%d TOK", (int)n);
        fw_textf(bmp, FONT5, 3, TY(56, 5), -1, "LEVEL:CHAR");
        fw_textf(bmp, FONT5, 68, TY(56, 5), -1, "%s",
                 sayit_dirty() ? "EDITED" : "PHRASE");
    }
}

/* ---- the held step's parameter locks ------------------------------------ */
/* Holding a trig key shows that step's locks on the page, inverted where a
 * knob is locked, as the stock p-lock pages do -- in any record mode, since
 * which mode a hold is in is not visible to a mod and a guess wedges the
 * display shut. The step is trig-1 on the shown pattern's first page (1:4,
 * VAWT's limit). The RAM layout is VAWT's, hardware-verified there: the
 * pattern's lock table at +0x1e80, a track's 10319-byte stride, a step's
 * 160-byte row, one int16 a sound slot with -1 for none. The only firmware
 * address this needs is the active pattern pointer, which mod.json passes as
 * PATPTR_AT (only 1.44 is built). */
#ifndef PATPTR_AT
#error "PATPTR_AT: the active-pattern pointer (0x4138f214 on 1.44)"
#endif

#define LOCK_TAB_OFF    0x1e80u
#define LOCK_TRACK_STR  10319u
#define LOCK_STEP_STR   160u

/* The held step: the lowest held trig key (ids 26-41) as step 0-15 of the
 * shown page; -1 when none is held. */
static int32_t sayit_held_step(void)
{
    uint32_t m = sayit_trigs, i;
    if (!m)
        return -1;
    for (i = 0; i < 16; i++)
        if (m & (1u << i))
            return (int32_t)i;
    return -1;
}

/* One knob's lock on a step, -1 for none. */
static int32_t sayit_lock_read(int32_t k, int32_t step)
{
    uint8_t *row;
    int32_t slot, t = fw_active_track;
    int16_t v;
    if (step < 0 || step > 63 || t < 0 || t > 3)
        return -1;
    slot = sayit_knob_slot(k);
    if (slot < 0)
        return -1;
    row = *(uint8_t *volatile *)PATPTR_AT;
    if (!row)
        return -1;
    row += LOCK_TAB_OFF + (uint32_t)t * LOCK_TRACK_STR +
        (uint32_t)step * LOCK_STEP_STR;
    v = *(volatile int16_t *)(row + (uint32_t)slot * 2u);
    return (int32_t)v;
}

/* What a cell shows: the held step's lock where that knob is locked on it,
 * else the kit's value. *locked says which. */
static int32_t sayit_shown(int32_t k, int32_t step, int32_t *locked)
{
    int32_t v;
    *locked = 0;
    if (step >= 0) {
        v = sayit_lock_read(k, step);
        if (v != -1) {
            *locked = 1;
            return v;
        }
    }
    return sayit_ui_knob(k);
}

/* ---- the SAYIT pages ---------------------------------------------------- */

static void sayit_phrase_window(char *out, int32_t outsz,
                                const char *phrase, const uint16_t *offs,
                                const uint8_t *lens, int32_t count,
                                int32_t focus)
{
    int32_t start = 0, i, k = 0, room;
    if (!out || outsz < 4)
        return;
    room = outsz - 1;
    if (count <= 0 || !phrase) {
        sayit_str_copy(out, outsz, "(EMPTY)", 8);
        return;
    }
    if (focus < 0 || focus >= count)
        focus = 0;
    start = offs[focus] > 10 ? offs[focus] - 10 : 0;
    if (start > 0)
        out[k++] = '<';
    for (i = start; phrase[i] && k < room - 1; i++)
        out[k++] = phrase[i];
    if (phrase[i] && k < room)
        out[k++] = '>';
    out[k] = 0;
}

static void sayit_token_strip(char *out, int32_t outsz,
                              const char *phrase, const uint16_t *offs,
                              const uint8_t *lens, int32_t count,
                              int32_t focus)
{
    int32_t first, last, t, k = 0, j, room;
    char num[4];
    if (!out || outsz < 4)
        return;
    room = outsz - 1;
    if (count <= 0) {
        sayit_str_copy(out, outsz, "NO TOKENS", 10);
        return;
    }
    if (focus < 0 || focus >= count)
        focus = 0;
    first = focus > 1 ? focus - 1 : 0;
    last = first + 4;
    if (last > count)
        last = count;
    if (first > 0 && k < room)
        out[k++] = '<';
    for (t = first; t < last && k < room - 2; t++) {
        int32_t v = t + 1, d = 0;
        if (t > first && k < room - 1)
            out[k++] = ' ';
        do {
            num[d++] = (char)('0' + v % 10);
            v /= 10;
        } while (v && d < 3);
        while (d && k < room - 1)
            out[k++] = num[--d];
        if (k < room - 1)
            out[k++] = ':';
        if (t == focus && k < room - 1)
            out[k++] = '[';
        for (j = 0; j < lens[t] && j < 7 && k < room - 2; j++)
            out[k++] = phrase[offs[t] + j];
        if (t == focus && k < room - 1)
            out[k++] = ']';
        if (k < room - 1)
            out[k++] = ' ';
    }
    if (last < count && k < room)
        out[k++] = '>';
    out[k] = 0;
}

/* The controls each SAYIT page shows, in encoder order. Every one of them
 * is on the lower encoder row E-H, whose band is the screen's lower half:
 * the Digitone's encoders are two rows of four (A-D above E-H), so a cell
 * drawn in the wrong band would sit under another encoder and a turn there
 * would edit a parameter the player did not aim at. */
static const int32_t sayit_page_knobs[2][4] = {
    { K_PIT, K_SPD, K_MODE, -1 },          /* SYN1 E-G */
    { K_THRT, K_MOUT, -1, -1 },            /* SYN2 E-F */
};

/* One knob cell in the lower band: encoder letter and label, then value.
 * The label and value are the whole cell: the stock-style bar that grew and
 * shrank under the value told the player nothing they could not read, so it
 * is left out. A step-locked knob shows the step's value on an
 * inverted cell, as the manual's p-lock display does. */
static void sayit_draw_cell(void *bmp, int32_t x, int32_t k, int32_t step)
{
    int32_t w = 26;
    int32_t locked = 0;
    int32_t v = sayit_shown(k, step, &locked);
    char val[12];
    const char *lbl = "";
    switch (k) {
    case K_PIT:  lbl = "PIT";  break;
    case K_SPD:  lbl = "SPD";  break;
    case K_MODE: lbl = "MODE"; break;
    case K_THRT: lbl = "THRT"; break;
    case K_MOUT: lbl = "MOUT"; break;
    default: return;
    }
    fw_framerect(bmp, x, 63 - 54, x + w - 1, 63 - 41, 1);
    fw_textf(bmp, FONT5, x + 2, TY(42, 5), -1, "%c %s",
             (char)('A' + (k % 8)), lbl);
    sayit_fmt_knob(k, v, val);
    fw_textf(bmp, FONT5, x + 2, TY(49, 5), -1, "%s", val);
    /* The lock: the cell's inside, label and value, inverted (colour -1),
     * inside the frame so the grid stays readable. */
    if (locked)
        fw_fillrect(bmp, x + 1, 10, x + w - 2, 21, -1);
}

static void sayit_draw_page(void *bmp, int32_t key)
{
    int32_t track = sayit_track_of(fw_active_track);
    const char *phrase = sayit_get_phrase(track);
    uint16_t offs[SAYIT_TOKENS_MAX];
    uint8_t lens[SAYIT_TOKENS_MAX];
    int32_t count, focus, mode, i, step, locked = 0;
    char pwin[34], strip[44], text[40], num[8];

    /* A held step's locks stand in for the kit while the page is up, so the
     * phrase, the token count and the cells all describe that one step. */
    step = sayit_held_step();
    mode = sayit_map_mode(sayit_shown(K_MODE, step, &locked));
    count = sayit_tokenise(phrase, mode, offs, lens, SAYIT_TOKENS_MAX);
    focus = sayit_get_last_token(track);
    if (focus < 0 || focus >= count)
        focus = 0;

    /* content rect below the stock title bar and right of the rail */
    fw_fillrect(bmp, 15, 0, 127, 53, 0);

    /* header: the last note played, by the name the unit uses (C5, C#5), so
     * the C5-relative key-to-token mapping is visible instead of guessed.
     * The raw number (N72) would read as C6 on the unit, which is why the
     * name is printed instead. */
    fw_textf(bmp, FONTL, SAYIT_X, TY(11, 5), -1, "SAY IT");
    {
        int32_t note = sayit_get_last_note(track);
        char name[5];
        if (note >= 0)
            fw_textf(bmp, FONT5, 50, TY(11, 5), -1, "%s",
                     sayit_note_name(note, name));
        else
            fw_textf(bmp, FONT5, 50, TY(11, 5), -1, "--");
    }
    fw_textf(bmp, FONT5, 78, TY(11, 5), -1, "%s",
             mode == SAYIT_WORD ? "WORD" : "CHAR");
    fw_textf(bmp, FONT5, 110, TY(11, 5), -1, "%dT", (int)count);
    fw_fillrect(bmp, SAYIT_X, 63 - 16, 127, 63 - 16, 1);

    /* phrase and token strip fill the upper half, above the encoder band */
    sayit_phrase_window(pwin, sizeof(pwin), phrase, offs, lens, count, focus);
    sayit_token_strip(strip, sizeof(strip), phrase, offs, lens, count, focus);
    fw_textf(bmp, FONT5, SAYIT_X, TY(18, 5), -1, "%s", pwin);
    fw_textf(bmp, FONT5, SAYIT_X, TY(25, 5), -1, "%s", strip);

    /* A note outside C5..C5+count-1 plays nothing; say why, so a silent key
     * never looks like a broken machine. */
    {
        int32_t last = sayit_get_last_note(track);
        if (last >= 0 && count > 0 &&
            sayit_token_for_note(last, SAYIT_BASE, count) < 0) {
            char name[5];
            fw_textf(bmp, FONT5, SAYIT_X, TY(32, 5), -1, "%s OUTSIDE %dT",
                     sayit_note_name(last, name), (int)count);
        } else if (sayit_toast > 0 && focus >= 0 && focus < count) {
            /* the token that just sounded */
            int32_t m = lens[focus] > 7 ? 7 : lens[focus];
            int32_t j;
            for (j = 0; j < m; j++)
                text[j] = phrase[offs[focus] + j];
            text[m] = 0;
            fw_textf(bmp, FONT5, SAYIT_X, TY(32, 5), -1, "T%d/%d %s",
                     (int)focus + 1, (int)count, text);
        } else if (key == 1) {
            int32_t v = sayit_shown(K_SPD, step, &locked);
            sayit_fmt_knob(K_PIT, sayit_shown(K_PIT, step, &locked), num);
            sayit_fmt_knob(K_SPD, v, text);
            fw_textf(bmp, FONT5, SAYIT_X, TY(32, 5), -1, "PIT %s  SPD %s",
                     num, text);
        } else {
            sayit_fmt_knob(K_THRT, sayit_shown(K_THRT, step, &locked), num);
            sayit_fmt_knob(K_MOUT, sayit_shown(K_MOUT, step, &locked), text);
            fw_textf(bmp, FONT5, SAYIT_X, TY(32, 5), -1, "THRT %s  MOUT %s",
                     num, text);
        }
    }
    fw_textf(bmp, FONT5, 93, TY(32, 5), -1, "YES:EDIT");

    /* this page's controls, each in its encoder's column of the lower band */
    for (i = 0; i < 4; i++) {
        int32_t k = sayit_page_knobs[key == 1 ? 0 : 1][i];
        if (k < 0)
            continue;
        sayit_draw_cell(bmp, SAYIT_CELL_X + i * 28, k, step);
    }
}

/* ---- tick, draw and keys ------------------------------------------------ */

void sayit_tick(void *ctrl)
{
    int32_t tok, n, key, show;
    uint32_t sig;

    if (!ctrl)
        return;
    sayit_ticks++;
    /* Modal sweep: a menu can eat a release with no brain-screen change. */
    if (sayit_brain) {
        void *bottom = *(void *volatile *)((uint8_t *)sayit_brain + BRAIN_BOTTOM);
        void *top = *(void *volatile *)((uint8_t *)sayit_brain + BRAIN_TOP);
        if (!bottom || bottom != top) {
            sayit_func = 0;
            sayit_ptn = 0;
            sayit_page = 0;
            sayit_rec = 0;
            sayit_popup = 0;
            sayit_trigs = 0;
        }
    }
    if (sayit_toast > 0)
        sayit_toast--;
    if (sayit_repaint > 0)
        sayit_repaint--;
    {
        int32_t tr = sayit_track_of(fw_active_track);
        if (tr != sayit_shown_track) {
            /* another track's last token is not this page's news */
            sayit_shown_track = tr;
            sayit_shown_tok = -2;
            sayit_shown_n = 0;
        }
        tok = sayit_get_last_token(tr);
        n = sayit_get_last_count(tr);
        sayit_shown_note = sayit_get_last_note(tr);
    }
    if (tok != sayit_shown_tok || n != sayit_shown_n) {
        sayit_shown_tok = tok;
        sayit_shown_n = n;
        if (tok >= 0)
            sayit_toast = SAYIT_TOAST_HOLD;
    }
    key = sayit_showing_key();
    show = sayit_is_sayit_track() && (key == 1 || key == 2) &&
        (int32_t)(sayit_ticks - sayit_msg_until) >= 0;
    {
        /* Hash what the page would paint: a held step's locks stand in for
         * the kit, so holding or releasing a trig that locks a knob re-arms
         * the frame. The pattern's lock table is only read while the page is
         * the one on screen (VAWT's gating), never from behind a menu. */
        uint32_t kv = 0;
        int32_t nm = sayit_no_menu();
        int32_t k, step = (show && nm) ? sayit_held_step() : -1;
        for (k = 0; k < DNM_KNOBS; k++) {
            int32_t locked = 0;
            int32_t v = sayit_shown(k, step, &locked);
            kv = kv * 33u + ((uint32_t)v & 0xffffu) +
                ((uint32_t)locked << 16);
        }
        sig = ((uint32_t)(sayit_editor_open ? 1 : 0) << 31)
            ^ ((uint32_t)(sayit_popup ? 1 : 0) << 30)
            ^ ((uint32_t)(show ? key : 0) << 28)
            ^ ((uint32_t)((fw_active_track + 1) & 15) << 24)
            ^ ((uint32_t)(sayit_toast > 0 ? 1 : 0) << 27)
            ^ ((uint32_t)(sayit_dirty() ? 1 : 0) << 26)
            ^ ((uint32_t)(nm ? 1 : 0) << 25)   /* a popup went up/down */
            ^ ((uint32_t)(sayit_shown_note + 2) << 8)
            ^ ((uint32_t)(sayit_ed.cursor + 1) << 18)
            ^ ((uint32_t)(sayit_ed.grid_row * SAYIT_GRID_COLS + sayit_ed.grid_col + 1) << 10)
            ^ (sayit_hash(sayit_ed.buf, sayit_ed.len) & 0x3ff)
            ^ (uint32_t)(sayit_shown_tok + 17)
            ^ kv;
    }
    if (!sayit_sig_valid || sig != sayit_sig) {
        sayit_sig = sig;
        sayit_sig_valid = 1;
        *((unsigned char *)ctrl + 0x20) = 1; /* re-arm once per change */
    }
    /* A modal screen and the frame after it are the stock's to overwrite:
     * ask for a frame every tick until the page has settled again. */
    if (sayit_editor_open || sayit_repaint > 0)
        *((unsigned char *)ctrl + 0x20) = 1;
}

void sayit_draw(void *bmp, void *ctrl)
{
    int32_t key;
    (void)ctrl;
    if (!bmp)
        return;
    if (sayit_editor_open) {
        if (!sayit_no_menu())
            return;
        sayit_draw_name_screen(bmp);
        return;
    }
    if (!sayit_is_sayit_track())
        return;
    key = sayit_showing_key();
    if (sayit_repaint > 0) {
        /* Just back from the modal screen: the showing key and the page
         * position can still read stale for a frame, so paint from the
         * machine alone and default to SYN1. A screen over the pages still
         * wins -- never paint under a menu. */
        if (!sayit_no_menu())
            return;
        if (key != 2)
            key = 1;
    } else {
        if (key != 1 && key != 2)
            return;
        if (!sayit_page_first())
            return;
    }
    if ((int32_t)(sayit_ticks - sayit_msg_until) < 0)
        return; /* copy/paste/clear toast keeps the frame */
    if (!sayit_ed_live) {
        sayit_ed_set(&sayit_ed, sayit_get_phrase(sayit_track_of(fw_active_track)));
        sayit_ed_live = 1;
    }
    sayit_draw_page(bmp, key);
}

/* While the naming screen is open it owns the keys, except PLAY/STOP so the
 * pattern can be auditioned. */
static int32_t sayit_editor_key(int32_t id, int32_t flags)
{
    int32_t down = (flags & 1) != 0;
    int32_t rep = (flags & 8) != 0;
    int32_t rel = (flags & 0x10) != 0;
    int32_t act = down || rep;

    if (id == KEY_FUNC) {
        /* FUNC's own release is 0x10, or 0 when another key went down while
         * it was held; treat either as the release. */
        int32_t pressed = (flags & 1) || (flags & 8);
        if (!pressed) {
            if (sayit_popup) {
                if (!sayit_func_action)
                    sayit_ed_grid_accept(&sayit_ed);
                sayit_popup = 0;
            }
            sayit_func_action = 0;
            return 1;
        }
        if (!sayit_popup) {
            sayit_ed_grid_open(&sayit_ed);
            sayit_popup = 1;
            sayit_func_action = 0;
            sayit_enc_acc = 0;
        }
        return 1;
    }
    if (id == KEY_PLAY || id == KEY_STOP)
        return 0;

    if (sayit_popup) {
        if (rel)
            return 1;
        if (id == KEY_LEFT)  { sayit_ed_grid_move(&sayit_ed, -1, 0); return 1; }
        if (id == KEY_RIGHT) { sayit_ed_grid_move(&sayit_ed, 1, 0); return 1; }
        if (id == KEY_UP)    { sayit_ed_grid_move(&sayit_ed, 0, -1); return 1; }
        if (id == KEY_DOWN)  { sayit_ed_grid_move(&sayit_ed, 0, 1); return 1; }
        if (id == KEY_YES) {
            sayit_ed_space(&sayit_ed);
            sayit_func_action = 1;
            return 1;
        }
        if (id == KEY_NO) {
            sayit_ed_backspace(&sayit_ed);
            sayit_func_action = 1;
            return 1;
        }
        return 1;
    }
    if (rel)
        return 1;
    if (id == KEY_LEFT)  { sayit_ed_move(&sayit_ed, -1); return 1; }
    if (id == KEY_RIGHT) { sayit_ed_move(&sayit_ed, 1); return 1; }
    if (id == KEY_UP)    { sayit_ed_step(&sayit_ed, 1); return 1; }
    if (id == KEY_DOWN)  { sayit_ed_step(&sayit_ed, -1); return 1; }
    if (id == KEY_YES || id == KEY_NO) {
        sayit_swallow = id;
        sayit_swallow_until = sayit_ticks + 30;
        sayit_close_editor(id == KEY_YES);
        return 1;
    }
    return 1;
}

int32_t sayit_key_handler(void *brain, void *ev)
{
    int32_t id, flags;
    if (brain)
        sayit_brain = brain;
    if (!ev)
        return 0;
    id = *(volatile int32_t *)((char *)ev + 12);
    flags = *(volatile int32_t *)((char *)ev + 16);

    if (sayit_editor_open)
        return sayit_editor_key(id, flags);

    /* The key that closed the editor must not reach the stock as a bare
     * press or a release it never saw pressed. */
    if (sayit_swallow >= 0) {
        if ((int32_t)(sayit_ticks - sayit_swallow_until) >= 0) {
            sayit_swallow = -1;
        } else if (id == sayit_swallow) {
            if (flags & 0x10)
                sayit_swallow = -1;
            return 1;
        }
    }

    if (id == KEY_FUNC) {
        sayit_func = (flags & (1 | 8)) ? 1 : 0;
        return 0;
    }
    if (id == KEY_PTN) {
        sayit_ptn = (flags & 0x10) ? 0 : 1;
        return 0;
    }
    if (id == KEY_PAGE) {
        sayit_page = (flags & 0x10) ? 0 : 1;
        return 0;
    }
    /* RECORD is tracked only so a FUNC/PTN/PAGE + RECORD chord can be told
     * from a bare one. No record mode gates the lock display: a held trig
     * always shows its step's values, because gating it on GRID/STEP leaves
     * a RECORD+PLAY hold with nothing to show. */
    if (id == KEY_RECORD)
        sayit_rec = (flags & 0x10) ? 0 : 1;
    if (id >= 26 && id <= 41) {
        if (flags & 0x10)
            sayit_trigs &= ~(1u << (uint32_t)(id - 26));
        else
            sayit_trigs |= 1u << (uint32_t)(id - 26);
    } else if (id >= 20 && id <= 25 && (flags & 1)) {
        sayit_trigs = 0;   /* another page: that hold was the last page's */
    }
    if (id >= 42 && id <= 45 && (flags & 1)) {
        /* Track press/hold: menus eat releases unseen — drop edge states. */
        sayit_func = 0;
        sayit_ptn = 0;
        sayit_page = 0;
        sayit_rec = 0;
        sayit_trigs = 0;
    }
    if ((id == KEY_RECORD || id == KEY_PLAY || id == KEY_STOP) && !(flags & 0x10)) {
        /* Yield the frame only to the chords that open a popup in the page's
         * own view — FUNC/PTN/PAGE + RECORD/PLAY/STOP, i.e. copy, paste and
         * clear, which cover the pages with a message box. Everything else
         * (a bare RECORD, RECORD+PLAY, RECORD+STOP) reports itself in the top
         * bar, which sits above the pages, so the overlay stays up. */
        if (sayit_func || sayit_ptn || sayit_page)
            sayit_msg_until = sayit_ticks + SAYIT_MSG_HOLD;
        return 0;
    }

    /* On a SAYIT SYN1/SYN2 first page, YES opens the naming screen. It is a
     * bare press: a chord (copy/paste/clear) keeps its stock meaning. */
    if (id == KEY_YES && (flags & 1) && !(flags & 0x10) &&
        !sayit_func && !sayit_ptn && !sayit_page && !sayit_rec &&
        (int32_t)(sayit_ticks - sayit_msg_until) >= 0 &&
        sayit_is_sayit_track() &&
        (sayit_showing_key() == 1 || sayit_showing_key() == 2) &&
        sayit_page_first()) {
        sayit_open_editor();
        return 1;
    }
    return 0;
}

int32_t sayit_enc_handler(void *brain, void *ev)
{
    int32_t id, delta, step;
    if (brain)
        sayit_brain = brain;
    if (!sayit_editor_open)
        return 0;   /* the pages' knobs stay live outside the naming screen */
    if (!ev)
        return 1;
    id = *(volatile int32_t *)((char *)ev + 12);
    delta = *(volatile int32_t *)((char *)ev + 16);
    if (id == 9) {  /* LEVEL/DATA: the factory naming screen's character knob */
        sayit_enc_acc += delta;
        step = sayit_enc_acc / SAYIT_ENC_COUNTS;
        sayit_enc_acc -= step * SAYIT_ENC_COUNTS;
        if (step > 16) step = 16;
        if (step < -16) step = -16;
        if (step) {
            if (sayit_popup) {
                sayit_ed_grid_move(&sayit_ed, 0, step > 0 ? 1 : -1);
            } else {
                int32_t i, n = step > 0 ? step : -step;
                for (i = 0; i < n; i++)
                    sayit_ed_step(&sayit_ed, step > 0 ? 1 : -1);
            }
            sayit_sig_valid = 0;
        }
    }
    return 1;   /* the naming screen owns the encoders while it is up */
}
