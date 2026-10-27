/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SAY IT - a SAM speech machine for the Digitakt mk1.
 *
 * The voice is SAM itself: sam/sam_port.c and sam/reciter.c are the
 * s-macke/SAM engine and its reciter, ported with their original tables and
 * proven byte-identical to that program by tools/sam_parity.py. Every word
 * you type goes through SAM's own text-to-phoneme rules and formant
 * synthesiser. sayit.h is the shared half (tokenising, the C5 note map, the
 * voice controls, resampling to 48 kHz); this file is the Digitakt's: an SRC
 * machine that renders its own voice into the track's block.
 *
 * How it differs from the Digitone machine, and why
 * -------------------------------------------------
 * The Digitone has a second CPU rendering eight FM voices, and a machine
 * borrows those voices. The Digitakt has no such engine: a machine renders
 * the track's audio itself, 32 Q31 samples a block, into fw_track_blocks
 * (docs/ADAPTING.md, "SRC machines"), and the track's filter, amp envelope,
 * level, effects and LFOs then work on it as on a sample. So there is no
 * voice allocation here and nothing to borrow: each of the eight audio
 * tracks renders its own sayit with its own SAM engine, and a track plays
 * whatever its own sound's phrase and knobs say.
 *
 * Typed phrases become triggerable tokens: WORD mode splits on whitespace,
 * CHAR mode plays one non-whitespace character per token. The phrase starts
 * at C5 (MIDI 60, the Digitakt's own note naming) and each semitone above it
 * is the next token, so the first word is on C5 and the speech climbs with
 * the key. Notes below C5, or past the end of the phrase, play nothing. A
 * new note on a track cuts that track's token with a short fade; the other
 * tracks are untouched, so SAY IT on two tracks speaks on both at once.
 *
 * The knobs are the machine's own eight SRC-page parameters. That is the
 * Digitakt's equivalent of borrowing stock parameters: they are the sound's
 * own slots, so p-locks (which arrive in fw_voice_params as the render sees
 * them), the Randomize page, MIDI CC 16-23 and the two system LFOs all reach
 * the voice without the machine adding anything.
 *
 * Knob map (machines/digitakt/overlay.c labels them; keep the two in step):
 *   A PIT   pitch offset, 37..89 around 63, +/-24 semitones
 *   B SPD   speed, 0..127, 64 is SAM's own
 *   C MODE  WORD (lower half) / CHAR (upper half)
 *   D THRT  throat offset, 0..127 around 64
 *   E MOUT  mouth offset, 0..127 around 64
 *   F-H blank
 *
 * PIT keeps the Digitone machine's 37..89 window so sayit_map_pitch maps it
 * to semitones unchanged, and a p-lock taken on the Digitone means the same
 * number here.
 *
 * Keep SAYIT_ID equal to mod.json's machine id.
 */
#include "digitakt-mk1/core3.h"
#include "../../sayit.h"
#include "phrase_store.h"

#define SAYIT_ID 77             /* must match mod.json: "machines": [{"id": 77, ...}] */

/* The editor's limit is the phrase store's capacity; mod.json sets it with
 * -DSAYIT_EDIT_LEN, and the two must not drift apart. */
#if SAYIT_EDIT_LEN != SAYIT_STORE_CHARS
#error "SAYIT_EDIT_LEN must be the phrase store's capacity (machines/digitakt/mod.json)"
#endif

/* A sound's slot s: 53 s16 words from +0x14, so slot 22 - the first the phrase
 * uses - is at +0x40, and the extras' slot 46 at +0x70 (ADAPTING.md, "Sound
 * slots on the Digitakt mk1"). */
#define SAYIT_SLOT(s) (0x14 + 2 * (s))

#define K_PIT   0
#define K_SPD   1
#define K_MODE  2
#define K_THRT  3
#define K_MOUT  4

/* Everything that belongs to an instance of the machine is per track: the
 * phrase, the SAM engine behind it and the voice reading it. SAY IT on T1 and
 * SAY IT on T2 are two instruments; one shared phrase and one shared engine
 * would make T2 speak T1's words and cut T1 off mid-word.
 *
 * The phrase is not shared and it is not lost: it is kept in the track's own
 * sound, in the words that sound does not otherwise use (phrase_store.h), and
 * the unit saves a sound with the pattern and the project. So the phrase comes
 * back with the pattern after a power cycle. This device's core has no project
 * data for a mod (core-dn1's projdata is a Digitone feature), which is why the
 * sound itself is the store, and why a phrase here is shorter than the
 * Digitone's: the sound has room for twenty characters, not two hundred. */
static char sayit_phrase[SAYIT_TRACKS][SAYIT_PHRASE_MAX] = {
    "say it", "say it", "say it", "say it",
    "say it", "say it", "say it", "say it"
};

static struct sayit_voice sayit_v[SAYIT_TRACKS];
static int32_t sayit_ready;
static int32_t sayit_gen[SAYIT_TRACKS];
static int32_t sayit_last_tok[SAYIT_TRACKS];
static int32_t sayit_last_n[SAYIT_TRACKS];
static int32_t sayit_last_note[SAYIT_TRACKS];
/* The last block's start pulse, so a bit that stayed set is not a second
 * note-on: fw_voice_start is a bit a track sets for the block it starts in,
 * and a machine must see the edge, not the level. */
static uint32_t sayit_prev_start;

static int32_t sayit_track(int32_t t)
{
    return sayit_clamp32(t, 0, SAYIT_TRACKS - 1);
}

static void sayit_copy_phrase(char *dst, const char *p)
{
    int32_t i = 0;
    if (!p) {
        dst[0] = 0;
        return;
    }
    while (p[i] && i < SAYIT_PHRASE_MAX - 1) {
        char c = p[i];
        dst[i] = (c >= 32 && c < 127) ? c : ' ';
        i++;
    }
    dst[i] = 0;
}

/* ---- the phrase in the sound -------------------------------------------- */
/* The words each track's sound was last seen holding, so a change made
 * elsewhere - a pattern or a sound loaded, a machine switched, an LFO or a MIDI
 * CC aimed at one of the words - is picked up instead of written over. */
static int16_t sayit_seen[SAYIT_TRACKS][SAYIT_STORE_WORDS];
static int32_t sayit_seen_ok[SAYIT_TRACKS];
/* Ticks left in which a sound that has just turned to SAY IT is given the
 * track's phrase again: the firmware may reset the machine's parameters after
 * machine-pages has told us which sound took the track. */
static int32_t sayit_fresh[SAYIT_TRACKS];

/* A track's sound, or 0 when the sound on that track is not SAY IT. */
static uint8_t *sayit_sound(int32_t t)
{
    uint8_t *kit = fw_kit;
    uint8_t *snd;

    if (!kit || t < 0 || t >= SAYIT_TRACKS)
        return 0;
    snd = FW_SOUND(kit, t);
    return snd[FW_SOUND_MACHINE] == SAYIT_ID ? snd : 0;
}

static void sayit_store_read(const uint8_t *snd, int16_t *w)
{
    int32_t i;
    for (i = 0; i < SAYIT_STORE_WORDS; i++)
        w[i] = *(const volatile int16_t *)(snd + SAYIT_SLOT(sayit_store_slot[i]));
}

static void sayit_store_put(uint8_t *snd, const int16_t *w)
{
    int32_t i;
    for (i = 0; i < SAYIT_STORE_WORDS; i++)
        *(volatile int16_t *)(snd + SAYIT_SLOT(sayit_store_slot[i])) = w[i];
}

/* The track's phrase into the sound it plays, and into what the tick compares
 * against. The sound is what the unit saves, so this is the write that makes a
 * phrase survive the unit being switched off. */
static void sayit_store_write_to(int32_t t, uint8_t *snd)
{
    int16_t w[SAYIT_STORE_WORDS];
    int32_t i;

    if (!snd)
        return;
    sayit_store_encode(sayit_phrase[t], w);
    sayit_store_put(snd, w);
    for (i = 0; i < SAYIT_STORE_WORDS; i++)
        sayit_seen[t][i] = w[i];
    sayit_seen_ok[t] = 1;
}

static void sayit_store_write(int32_t t)
{
    sayit_store_write_to(t, sayit_sound(t));
}

/* ev_tick (the UI task): keep the two in step. A sound whose words do not hold a
 * phrase - one just picked, or one saved before phrases lived in a sound - is
 * given the track's; a sound whose words have changed holds a phrase that was
 * loaded, copied or modulated, and its words are taken as the phrase. Called
 * from overlay.c's tick, which is the mod's ev_tick. */
void sayit_store_sync(void)
{
    int32_t t, i;

    for (t = 0; t < SAYIT_TRACKS; t++) {
        uint8_t *snd = sayit_sound(t);
        int16_t w[SAYIT_STORE_WORDS];
        char p[SAYIT_STORE_CHARS + 1];

        if (!snd) {
            sayit_seen_ok[t] = 0;
            continue;
        }
        sayit_store_read(snd, w);
        if (!sayit_store_valid(w) || sayit_fresh[t]) {
            if (sayit_fresh[t])
                sayit_fresh[t]--;
            sayit_store_write_to(t, snd);
            continue;
        }
        if (sayit_seen_ok[t] && sayit_store_same(w, sayit_seen[t]))
            continue;
        sayit_store_decode(w, p, (int32_t)sizeof(p));
        sayit_copy_phrase(sayit_phrase[t], p);
        for (i = 0; i < SAYIT_STORE_WORDS; i++)
            sayit_seen[t][i] = w[i];
        sayit_seen_ok[t] = 1;
    }
}

/* machine-pages calls this when a track's sound turns to SAY IT. The words that
 * sound holds are the previous machine's, so the track's phrase goes in. */
static void sayit_switch(int32_t t, int32_t from, uint8_t *snd)
{
    (void)from;
    if (t < 0 || t >= SAYIT_TRACKS || !snd)
        return;
    sayit_store_write_to(t, snd);
    sayit_fresh[t] = 3;
}

static void put_int(char *buf, int32_t v)
{
    char tmp[8];
    int32_t n = 0;
    if (v < 0) {
        *buf++ = '-';
        v = -v;
    }
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 7);
    while (n)
        *buf++ = tmp[--n];
    *buf = 0;
}

static void put_signed(int32_t v, char *buf)
{
    if (v > 0)
        *buf++ = '+';
    put_int(buf, v);
}

/* Phrase access for the overlay, per track. An out-of-range track reads and
 * writes track 0 so a stray call can never corrupt another track's phrase. The
 * write also puts the phrase into the track's sound, which is what the unit
 * saves. */
void sayit_set_phrase(int32_t track, const char *p)
{
    int32_t t = sayit_track(track);

    sayit_copy_phrase(sayit_phrase[t], p);
    sayit_store_write(t);
}

const char *sayit_get_phrase(int32_t track) { return sayit_phrase[sayit_track(track)]; }
int32_t sayit_get_last_token(int32_t track) { return sayit_last_tok[sayit_track(track)]; }
int32_t sayit_get_last_count(int32_t track) { return sayit_last_n[sayit_track(track)]; }
int32_t sayit_get_last_note(int32_t track) { return sayit_last_note[sayit_track(track)]; }

/* ---- one token on a track ------------------------------------------------- */
/* Reads the track's knobs as the render sees them (p points at its eight
 * parameters, 8.8), tokenises its phrase and starts the token the note names
 * on that track's own SAM engine. An empty phrase, or a note outside
 * C5..C5+count-1, is silent: wrapping the index would make a short phrase
 * restart under the player's fingers, and the pitch restart with it. */
static void sayit_note_on(int32_t t, const volatile int16_t *p)
{
    uint16_t offs[SAYIT_TOKENS_MAX];
    uint8_t lens[SAYIT_TOKENS_MAX];
    char expanded[SAYIT_EXPAND_MAX];
    const char *phrase = sayit_phrase[t];
    int32_t note, count, idx, mode, exp_len, i;
    int32_t spd, pit, thr, mou;
    const char *tok;
    int32_t tlen;

    note = (fw_voice_note[t] >> 16) & 127;
    mode = sayit_map_mode((int32_t)p[K_MODE]);
    sayit_last_note[t] = note;
    count = sayit_tokenise(phrase, mode, offs, lens, SAYIT_TOKENS_MAX);
    idx = sayit_token_for_note(note, SAYIT_BASE, count);
    if (idx < 0) {
        sayit_v[t].active = 0;
        return;
    }
    tok = phrase + offs[idx];
    tlen = lens[idx];
    if (tlen == 1) {
        exp_len = sayit_expand_token(tok, tlen, expanded, sizeof(expanded));
    } else {
        exp_len = tlen < (int32_t)sizeof(expanded) - 1
            ? tlen : (int32_t)sizeof(expanded) - 1;
        for (i = 0; i < exp_len; i++)
            expanded[i] = tok[i];
        expanded[exp_len] = 0;
    }
    if (exp_len <= 0) {
        sayit_v[t].active = 0;
        return;
    }
    spd = sayit_map_speed(p[K_SPD] >> 8);
    /* C5 is both token 0 and the untransposed speech note, so token
     * sequencing and the pitch climb together. */
    pit = sayit_pitch_steps_for_note(note, SAYIT_BASE, p[K_PIT] >> 8);
    thr = sayit_map_voice(p[K_THRT] >> 8);
    mou = sayit_map_voice(p[K_MOUT] >> 8);
    sayit_gen[t]++;
    sayit_last_tok[t] = idx;
    sayit_last_n[t] = count;
    sayit_v[t].note = note;
    /* Newest note wins on this track only: the other tracks' voices and
     * engines are untouched, so T1 keeps talking while T2 starts. */
    sayit_voice_start(&sayit_v[t], t, expanded, exp_len, spd, pit, thr, mou,
                      sayit_gen[t]);
}

/* PIT and SPD reach a word already speaking: SAM reads the glottal period and
 * the speed counter as it goes (sam/sam_api.h). So a turned knob, a p-lock
 * arriving mid-phrase or an LFO on either bends the word in progress, which
 * is what a player expects of a sound parameter. THRT and MOUT are not sent:
 * SAM bakes them into its formant tables and a breath chunk copies the
 * frequencies when it starts, so they arrive with the next token. Nothing is
 * sent while the live values match what the engine has, so a voice with no
 * LFO and no turning never reaches the port. */
static void sayit_live_controls(int32_t t, const volatile int16_t *p,
                                struct sayit_voice *s)
{
    struct sam_settings st;
    int32_t spd, pit;

    if (!s->active)
        return;
    spd = sayit_map_speed(p[K_SPD] >> 8);
    pit = sayit_pitch_steps_for_note(s->note, SAYIT_BASE, p[K_PIT] >> 8);
    sayit_resolve(spd, pit, sayit_map_voice(p[K_THRT] >> 8),
                  sayit_map_voice(p[K_MOUT] >> 8), &st);
    if (st.speed == s->set.speed && st.period_q15 == s->set.period_q15)
        return;
    sam_voice_live_tr(t, &st);
    s->set.speed = st.speed;
    s->set.period_q15 = st.period_q15;
}

/* ---- the render (interrupt level, 1500 blocks a second) ------------------ */
/* ev_render_voices: after playback has written every track's block and before
 * the overdrive. Only the tracks whose machine is SAY IT are touched; every
 * other track's block is left exactly as the stock wrote it. */
void sayit_render(int32_t *blocks)
{
    uint32_t started = fw_voice_start;
    int32_t t, i;

    if (!sayit_ready) {
        for (t = 0; t < SAYIT_TRACKS; t++)
            sayit_voice_init(&sayit_v[t]);
        sayit_ready = 1;
    }
    for (t = 0; t < SAYIT_TRACKS; t++) {
        const volatile int16_t *p = &fw_voice_params[53 * t];
        struct sayit_voice *s = &sayit_v[t];

        if (fw_render_machine[t] != SAYIT_ID)
            continue;
        if ((started & (1u << t)) && !(sayit_prev_start & (1u << t)))
            sayit_note_on(t, p);
        sayit_live_controls(t, p, s);
        for (i = 0; i < 32; i++) {
            int32_t q15 = s->active ? sayit_frame(s) : 0;
            /* SAM's 8-bit stream arrives as Q15 and peaks near full scale;
             * trim it to about -4 dBFS, as the Digitone machine does, so a
             * track's own level and envelope sit where a sample's do. */
            q15 = (q15 * 5) >> 3;
            if (q15 > 32767) q15 = 32767;
            if (q15 < -32768) q15 = -32768;
            blocks[32 * t + i] = q15 << 16;      /* Q15 -> Q31 */
        }
    }
    sayit_prev_start = started;
}

/* ---- the page's value texts (machine-pages calls these) ------------------ */
/* The SRC page's knobs show what the player set, in the machine's own terms:
 * PIT in semitones, SPD as an offset around SAM's own speed, THRT and MOUT as
 * SAM's 0..255 formant values, MODE as WORD or CHAR. */
static int32_t pit_text(char *buf, int32_t value, int32_t ctx, int32_t machine)
{
    (void)ctx;
    (void)machine;
    put_signed(sayit_map_pitch(value >> 8), buf);
    return 1;
}

static int32_t spd_text(char *buf, int32_t value, int32_t ctx, int32_t machine)
{
    (void)ctx;
    (void)machine;
    put_signed((value >> 8) - 64, buf);
    return 1;
}

static int32_t mode_text(char *buf, int32_t value, int32_t ctx, int32_t machine)
{
    const char *s = sayit_map_mode(value) == SAYIT_WORD ? "WORD" : "CHAR";
    int32_t i = 0;
    (void)ctx;
    (void)machine;
    while (*s)
        buf[i++] = *s++;
    buf[i] = 0;
    return 1;
}

static int32_t thrt_text(char *buf, int32_t value, int32_t ctx, int32_t machine)
{
    (void)ctx;
    (void)machine;
    put_int(buf, sayit_clamp32(128 + ((value >> 8) - 64) * 2, 0, 255));
    return 1;
}

static int32_t mout_text(char *buf, int32_t value, int32_t ctx, int32_t machine)
{
    (void)ctx;
    (void)machine;
    put_int(buf, sayit_clamp32(128 + ((value >> 8) - 64) * 2, 0, 255));
    return 1;
}

/* machine-pages draws the SRC page from this: SLICE's page, our five controls
 * on A-E, and F-H hidden. `look` names the stock parameter whose UI record
 * and knob graphic each one borrows, so a turn moves it and the LFO page's
 * DEST list knows it; 134 is the id machine-pages' own SRC test pages use for
 * a control with no stock counterpart. PIT's range is the Digitone HARM
 * window (37..89, 63 centre) so the semitone map is the same on both
 * machines; CM_NOT_SAMPLE on D keeps the sample list closed.
 *
 * The defaults put every knob at its neutral position, as the Digitone
 * machine's do: PIT 0 semitones, SPD at SAM's own speed, THRT and MOUT at
 * SAM's 128, and MODE on CHAR, which is the first step of the upper half.
 *
 * The page names cm_ui_v32 rather than cm_ui_v3 for the seven extra values
 * below it: a machine that declares them keeps its own slots at 16 bits each
 * in machine-pages' saved record, and those forty-eight bits plus the extras'
 * hundred and five are the room the phrase needs (phrase_store.h). The extras
 * are hidden, so the page shows A-E and three blanks, exactly as before, and
 * its title stays the machine's short name. */
static const struct cm_src_page sayit_pages[1] = {
    { "SAYI", { 0, 1, 2, 3, 4, 5, 6, 7 }, 0, { 0, 0, 0 } },
};

static const struct cm_ui32 sayit_page = {
    .base = {
        .abi = cm_ui_v32,
        .page_from = 3,                         /* SLICE's page */
        .group = "SAY IT",                      /* the LFO page's group */
        .on_switch = sayit_switch,
        .knob = {
            [K_PIT]  = { .name = "PIT",  .lname = "Pitch",
                         .look = 134, .flags = CM_RANGE | CM_DEFAULT,
                         .min = 37 << 8, .max = 89 << 8, .def = 63 << 8,
                         .fmt = pit_text },
            [K_SPD]  = { .name = "SPD",  .lname = "Speed",
                         .look = 134, .flags = CM_RANGE | CM_DEFAULT,
                         .min = 0, .max = 127 << 8, .def = 64 << 8,
                         .fmt = spd_text },
            [K_MODE] = { .name = "MODE", .lname = "Mode",
                         .look = 134, .flags = CM_RANGE | CM_DEFAULT,
                         .min = 0, .max = 127 << 8, .def = 64 << 8,
                         .fmt = mode_text },
            [K_THRT] = { .name = "THRT", .lname = "Throat",
                         .look = 134, .flags = CM_RANGE | CM_DEFAULT | CM_NOT_SAMPLE,
                         .min = 0, .max = 127 << 8, .def = 64 << 8,
                         .fmt = thrt_text },
            [K_MOUT] = { .name = "MOUT", .lname = "Mouth",
                         .look = 134, .flags = CM_RANGE | CM_DEFAULT,
                         .min = 0, .max = 127 << 8, .def = 64 << 8,
                         .fmt = mout_text },
            [5] = { .flags = CM_HIDDEN },
            [6] = { .flags = CM_HIDDEN },
            [7] = { .flags = CM_HIDDEN },
        },
    },
    /* The seven extra values: no stock counterpart, never shown, never turned.
     * machine-pages requires a range and a default of them, 0..0x7F00; 0 is
     * also what a sound with no phrase in it holds. */
    .extra = {
        { .look = 134, .flags = CM_HIDDEN | CM_RANGE | CM_DEFAULT, .min = 0, .max = 0x7f00, .def = 0 },
        { .look = 134, .flags = CM_HIDDEN | CM_RANGE | CM_DEFAULT, .min = 0, .max = 0x7f00, .def = 0 },
        { .look = 134, .flags = CM_HIDDEN | CM_RANGE | CM_DEFAULT, .min = 0, .max = 0x7f00, .def = 0 },
        { .look = 134, .flags = CM_HIDDEN | CM_RANGE | CM_DEFAULT, .min = 0, .max = 0x7f00, .def = 0 },
        { .look = 134, .flags = CM_HIDDEN | CM_RANGE | CM_DEFAULT, .min = 0, .max = 0x7f00, .def = 0 },
        { .look = 134, .flags = CM_HIDDEN | CM_RANGE | CM_DEFAULT, .min = 0, .max = 0x7f00, .def = 0 },
        { .look = 134, .flags = CM_HIDDEN | CM_RANGE | CM_DEFAULT, .min = 0, .max = 0x7f00, .def = 0 },
    },
    .page_count = 1,
    .pages = sayit_pages,
};

/* A mouth: an open oval, 11 x 7, one column a word with the top row in bit
 * 25, as the stock icons are. */
static const uint32_t sayit_px[11] = {
    0x38000000, 0x44000000, 0x82000000, 0x82000000, 0x82000000, 0x82000000,
    0x82000000, 0x82000000, 0x82000000, 0x44000000, 0x38000000,
};
static const uint32_t sayit_mask[11] = {
    0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000,
    0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000,
};
static const struct {
    const void *vt;
    int32_t width, height, one;
    const uint32_t *px, *mask;
    int32_t zero;
} sayit_icon = { fw_bitmap_vt, 11, 7, 1, sayit_px, sayit_mask, 0 };

/* params 3 and render its own id: SLICE's eight parameter slots (their
 * defaults on a switch, MIDI CC 16-23, NRPN, locks, the LFOs and Randomize),
 * and an empty voice window that sayit_render fills. The sound's own spare
 * words carry the phrase. */
const struct cm_machine sayit_machine = {
    SAYIT_ID, "SAY IT", "SAYI", &sayit_icon, 3, SAYIT_ID, CM_UI_TAG, &sayit_page.base,
};
