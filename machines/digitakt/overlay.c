/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SAY IT phrase editor for the Digitakt mk1: the factory-style naming screen,
 * as a modal over whatever the unit is showing.
 *
 * The Digitakt gives a machine mod no page of its own (docs/ADAPTING.md,
 * "Machine pages": a machine describes the SRC page's eight knobs and nothing
 * else), and its core does not export the page or project-data machinery the
 * Digitone machine uses. So the editor is drawn over the frame instead: while
 * it is open, ev_draw paints the whole screen and ev_key takes every key, and
 * when it closes the stock screen comes back untouched. Nothing is drawn
 * otherwise: SAY IT on a track adds its five knobs to that track's SRC page
 * and no other UI.
 *
 * The key map is the Digitone's own NAMING screen (manual 6.5), so the two
 * SAY IT machines are learned once:
 *   LEFT/RIGHT   move the cursor
 *   UP/DOWN      select the character at the cursor
 *   encoders     the same, a turn at a time
 *   FUNC+NO      delete the character before the cursor
 *   FUNC+YES     insert a space
 *   YES          save the phrase for this track
 *   NO           abort
 *
 * How it opens: YES, as on the Digitone machine, and only when the active
 * track's sound is SAY IT and the SRC page is the one showing. The page
 * matters because YES is a confirm key everywhere else on the unit: taking a
 * bare YES on any screen would break SETTINGS, the sample browser and every
 * dialog. A mod is given no "which page is showing" signal (core exports the
 * kit, the parameters and the render, not the view), so the page is tracked
 * from the keys themselves: SRC sets it, every other page key clears it, and a
 * menu's or a dialog's own keys clear it too. The unit boots on SRC, so a
 * freshly loaded SAY IT is one YES away.
 *
 * FUNC+YES is deliberately left alone. It is a system command, and shadowing
 * it to open a text window is not worth the surprise.
 *
 * The Digitakt's SETTINGS menu would still be the tidiest door, and core
 * offers ev_settings + core_additem for it, but a row's label is a std::string
 * built by the firmware's own string constructor, whose address core does not
 * export: a mod would have to name it per OS, which is exactly the kind of
 * invented address this project does not allow.
 *
 * A saved phrase goes into the track's own sound, which the unit keeps with the
 * pattern and the project, so it comes back after a power cycle. This device's
 * core has no project data for a mod, so a sound is the only thing a machine
 * here can save, and it holds twenty characters rather than the Digitone's two
 * hundred and fifty-five (engine.c, phrase_store.h).
 */
#include "digitakt-mk1/core3.h"
#include "../../sayit.h"
#include "../../sayit_edit.h"

#define SAYIT_ID 77

#define KEY_FUNC   1
#define KEY_TRK    2
#define KEY_PTN    3
#define KEY_BANK   4
#define KEY_SONG   5
#define KEY_GLOBAL 6
#define KEY_SAMPLE 7
#define KEY_TEMPO  8
#define KEY_YES    12
#define KEY_NO     13
#define KEY_UP     14
#define KEY_DOWN   15
#define KEY_LEFT   16
#define KEY_RIGHT  17
#define KEY_PAGE   18
#define KEY_TRIG   19
#define KEY_SRC    20
#define KEY_FLTR   21
#define KEY_AMP    22
#define KEY_LFO    23

#define FONT5 ((const void *)fw_font5)
/* fw_textf's y is the bottom row of the glyph; ytop is the top row on the
 * 64-row screen and h is the font's height (font5 is 5). */
#define TY(yt, h) (63 - (yt) - (h) + 1)
/* A row counted from the top, as the y a rectangle's edge takes: those count
 * from the bottom, and include the row they name. */
#define ROW(y) (63 - (y))

/* The page's margins, in screen columns. Text starts at 4, which leaves the
 * three columns inside the frame empty; the right edge is the mirror of that,
 * the last glyph pixel at 123. fw_textf pastes glyphs and does not lay out a
 * box, so a string is placed by counting it: font5 advances 4 pixels a
 * character and 3 for a space (a 3 x 5 glyph on a 4-pixel pitch). */
#define TX_LEFT  4
#define TX_RIGHT 125        /* x = TX_RIGHT - width puts the ink at 123 */
#define TX_COL2  64         /* the help block's right column: one x for all */

static int32_t sayit_text_w(const char *s)
{
    int32_t w = 0;
    if (!s)
        return 0;
    while (*s)
        w += (*s++ == ' ') ? 3 : 4;
    return w;
}

/* From engine.c. */
extern void sayit_set_phrase(int32_t track, const char *p);
extern const char *sayit_get_phrase(int32_t track);
extern void sayit_store_sync(void);
extern void sayit_plan_tick(void);
extern int32_t sayit_get_last_token(int32_t track);
extern int32_t sayit_get_last_count(int32_t track);

static struct sayit_editor sayit_ed;
static int32_t sayit_open;      /* the editor is up */
static int32_t sayit_edit_track;/* the track it edits */
static uint32_t sayit_toast;    /* frames a confirmation stays up */
/* Whether the SRC page is the one showing, as far as the keys we have seen
 * say (see the top of this file). The unit boots on SRC. */
static int32_t sayit_on_src = 1;

/* Is the active track's sound SAY IT? That is the only condition under which
 * this mod takes a key or draws anything. */
static int32_t sayit_active_is_sayit(void)
{
    int32_t t = (int32_t)fw_active_track;
    uint8_t *kit = fw_kit;
    if (!kit || t < 0 || t >= SAYIT_TRACKS)
        return 0;
    return FW_SOUND(kit, t)[FW_SOUND_MACHINE] == SAYIT_ID;
}

static void sayit_close(int32_t save)
{
    if (save)
        sayit_set_phrase(sayit_edit_track, sayit_ed.buf);
    sayit_open = 0;
    sayit_toast = save ? 45 : 0;
}

static void sayit_open_editor(void)
{
    int32_t t = (int32_t)fw_active_track;
    if (t < 0 || t >= SAYIT_TRACKS)
        return;
    sayit_edit_track = t;
    sayit_ed_set(&sayit_ed, sayit_get_phrase(t));
    sayit_open = 1;
}

/* ---- keys --------------------------------------------------------------- */
/* ev_key: ev+12 is the key id, ev+16 the flags (1 pressed, 8 repeat, 2 FUNC
 * held, 0x10 release). While the editor is up every key is taken, so nothing
 * behind it reacts; otherwise FUNC+YES opens it on a SAY IT track and every
 * other key goes where it always went. */
int32_t sayit_key_handler(void *brain, void *ev)
{
    uint8_t *e = (uint8_t *)ev;
    int32_t key, flags, pressed, func;
    (void)brain;

    if (!e)
        return 0;
    key = *(int32_t *)(e + 12);
    flags = *(int32_t *)(e + 16);
    pressed = (flags & 1) != 0;
    func = (flags & 2) != 0;

    if (sayit_open) {
        if (!pressed)
            return 1;               /* releases are taken too: keep it modal */
        switch (key) {
        case KEY_LEFT:  sayit_ed_move(&sayit_ed, -1); break;
        case KEY_RIGHT: sayit_ed_move(&sayit_ed, 1);  break;
        case KEY_UP:    sayit_ed_step(&sayit_ed, 1);  break;
        case KEY_DOWN:  sayit_ed_step(&sayit_ed, -1); break;
        case KEY_YES:
            if (func)
                sayit_ed_space(&sayit_ed);
            else
                sayit_close(1);
            break;
        case KEY_NO:
            if (func)
                sayit_ed_backspace(&sayit_ed);
            else
                sayit_close(0);
            break;
        default:
            break;
        }
        return 1;
    }

    if (!pressed)
        return 0;

    /* Which page is showing, from the keys that switch pages and the keys that
     * raise a menu or a dialog over one. A page key decides it; anything that
     * could put a menu on top (GLOBAL, the pattern and bank menus, the browser,
     * the tempo dialog, the trig and scale pages) makes it untrue until SRC is
     * pressed again. */
    switch (key) {
    case KEY_SRC:
        sayit_on_src = 1;
        return 0;
    case KEY_FLTR:
    case KEY_AMP:
    case KEY_LFO:
    case KEY_TRIG:
    case KEY_PAGE:
    case KEY_GLOBAL:
    case KEY_PTN:
    case KEY_BANK:
    case KEY_SONG:
    case KEY_SAMPLE:
    case KEY_TEMPO:
    case KEY_TRK:
        sayit_on_src = 0;
        return 0;
    default:
        break;
    }

    /* YES on the SRC page of a SAY IT track opens the phrase. Not with FUNC:
     * that is the unit's own chord and stays the unit's. */
    if (key == KEY_YES && !func && sayit_on_src && sayit_active_is_sayit()) {
        sayit_open_editor();
        return 1;
    }
    return 0;
}

/* ---- encoders ----------------------------------------------------------- */
/* ev_enc: ev+12 is the encoder 1-8 (A-H), ev+16 the delta. A, E and F sit
 * under PIT, THRT and MOUT, whose values the page shows, so any turn edits the
 * phrase here while the editor is up: the editor is the only thing on screen. */
int32_t sayit_enc_handler(void *brain, void *ev)
{
    uint8_t *e = (uint8_t *)ev;
    int32_t delta;
    (void)brain;

    if (!sayit_open || !e)
        return 0;
    delta = *(int32_t *)(e + 16);
    if (delta)
        sayit_ed_step(&sayit_ed, delta);
    return 1;
}

/* ---- the screen --------------------------------------------------------- */
/* ev_tick asks for a redraw while something of ours is on screen. The unit
 * redraws on its own only when the stock UI changes, so a modal has to ask.
 * The tick is also where a phrase is picked up from the sounds the unit loaded:
 * engine.c keeps the two in step, which is what makes a saved phrase come back
 * after a power cycle. */
void sayit_tick(void *ctrl)
{
    sayit_store_sync();
    /* The recitation the audio callback would otherwise have to do at a
     * trigger: one token of one track a tick (sayit_plan_tick). */
    sayit_plan_tick();
    if (!ctrl)
        return;
    if (sayit_open)
        *((uint8_t *)ctrl + 0x20) = 1;
    else if (sayit_toast) {
        sayit_toast--;
        *((uint8_t *)ctrl + 0x20) = 1;
    }
}

/* The track's MODE knob: the phrase's token count depends on it, because CHAR
 * plays one token per character. The editor reads the sound's own parameter
 * word, the same value the render sees, so the count matches what will play. */
static int32_t sayit_edit_mode(void)
{
    uint8_t *kit = fw_kit;
    if (!kit || sayit_edit_track < 0 || sayit_edit_track >= SAYIT_TRACKS)
        return SAYIT_WORD;
    return sayit_map_mode(*(const int16_t *)(FW_SOUND(kit, sayit_edit_track) +
                                             FW_SOUND_PARAM(2)));
}

/* How many tokens the phrase in the editor will play. The page shows it so a
 * player can see the span of keys the phrase needs before saving it. */
static int32_t sayit_count_tokens(const char *p, int32_t mode)
{
    uint16_t offs[SAYIT_TOKENS_MAX];
    uint8_t lens[SAYIT_TOKENS_MAX];
    return sayit_tokenise(p, mode, offs, lens, SAYIT_TOKENS_MAX);
}

/* The phrase around the cursor, at most 28 characters (font5 is a 4-pixel
 * pitch on a 128-pixel screen), so the cursor is always on screen. Returns the
 * cursor's column in pixels from the left of the window, so the bar lands under
 * the character it edits rather than where a fixed 4 a character would put it. */
static int32_t sayit_window(char *out, int32_t out_sz)
{
    const int32_t cols = 28;
    int32_t start = 0, i, n = 0, cur = 0;

    if (sayit_ed.cursor >= cols)
        start = sayit_ed.cursor - cols + 1;
    for (i = start; i < sayit_ed.len && n < out_sz - 1 && n < cols; i++)
        out[n++] = sayit_ed.buf[i];
    out[n] = 0;
    for (i = start; i < sayit_ed.cursor && i < start + n; i++)
        cur += sayit_ed.buf[i] == ' ' ? 3 : 4;
    return cur;
}

/* ev_draw(bmp, ctrl): y = 0 is the bottom row. */
void sayit_draw(void *bmp, void *ctrl)
{
    char line[SAYIT_EDIT_MAX];
    char trk[4];
    int32_t count, i, x, cur;

    (void)ctrl;
    if (!bmp)
        return;

    if (!sayit_open) {
        /* A saved phrase leaves a short confirmation, and then nothing. The
         * frame underneath is the stock one, still showing its knobs, so the
         * message gets a box of its own - the editor's black ground and
         * outline, three clear columns inside it - rather than text pasted
         * straight onto the page, which ran through the knob outlines. */
        if (sayit_toast) {
            char msg[] = "PHRASE SAVED T0";
            int32_t w;

            msg[sizeof(msg) - 2] = (char)('1' + sayit_edit_track);
            w = sayit_text_w(msg);
            x = (128 - (w + 8)) / 2;        /* frame, 3 clear, text, 3 clear, frame */
            fw_fillrect(bmp, x, ROW(46), x + w + 7, ROW(38), 0);
            fw_framerect(bmp, x, ROW(46), x + w + 7, ROW(38), 1);
            fw_textf(bmp, FONT5, x + 4, TY(40, 5), (int32_t)sizeof(msg), "%s", msg);
        }
        return;
    }

    fw_fillrect(bmp, 0, 0, 127, 63, 0);
    fw_framerect(bmp, 0, 0, 127, 63, 1);

    /* Title on the left, the track on the right: the track number ends where a
     * left-hand line starts, three clear columns in from the frame. */
    fw_textf(bmp, FONT5, TX_LEFT, TY(3, 5), 60, "SAY IT PHRASE");
    trk[0] = 'T';
    trk[1] = (char)('1' + sayit_edit_track);
    trk[2] = 0;
    fw_textf(bmp, FONT5, TX_RIGHT - sayit_text_w(trk), TY(3, 5), 20, "%s", trk);

    /* The phrase, with the cursor as a bar under the character it is on. */
    cur = sayit_window(line, (int32_t)sizeof(line));
    fw_textf(bmp, FONT5, TX_LEFT, TY(14, 5), 116, "%s", line);
    x = TX_LEFT + cur;
    fw_fillrect(bmp, x, TY(14, 5) - 3, x + 2, TY(14, 5) - 2, 1);

    /* The span of keys the phrase needs, by the note it starts on: C5 is token
     * 1, and each semitone above it is the next token. Saying it here means a
     * player does not have to be told the mapping to find the first word. */
    count = sayit_count_tokens(sayit_ed.buf, sayit_edit_mode());
    fw_textf(bmp, FONT5, TX_LEFT, TY(24, 5), 60, "%d TOKENS FROM C5", (int)count);
    i = sayit_get_last_token(sayit_edit_track);
    if (i >= 0)
        fw_textf(bmp, FONT5, 74, TY(24, 5), 52, "LAST %d", (int)(i + 1));

    /* The keys, in two columns. Each column is one string at one x: padding
     * with spaces cannot line them up, because a space is narrower than a
     * character and the columns drift apart. */
    fw_textf(bmp, FONT5, TX_LEFT, TY(38, 5), 60, "L/R CURSOR");
    fw_textf(bmp, FONT5, TX_COL2, TY(38, 5), 60, "U/D CHAR");
    fw_textf(bmp, FONT5, TX_LEFT, TY(46, 5), 60, "FUNC+NO DEL");
    fw_textf(bmp, FONT5, TX_COL2, TY(46, 5), 60, "FUNC+YES SPC");
    fw_textf(bmp, FONT5, TX_LEFT, TY(54, 5), 60, "YES SAVE");
    fw_textf(bmp, FONT5, TX_COL2, TY(54, 5), 60, "NO ABORT");
}
