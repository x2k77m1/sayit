/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SAY IT core: phrase bookkeeping and the SAM voice, shared by every machine
 * SAY IT is built for (machines/digitone, machines/digitakt).
 *
 * Nothing here knows a device: the knob layout, the page and the render
 * belong to the machine's own folder, and this file is the speech.
 *
 * The speech itself is SAM, ported: sam/sam_port.c and sam/reciter.c are
 * s-macke/SAM's engine and reciter with their original tables, and they
 * produce SAM's own 8-bit stream at 22050 Hz. This file is the part that is
 * SAYIT's: tokenising the typed phrase, mapping notes to tokens, the voice
 * controls, and resampling that stream to the machine's 48 kHz render.
 *
 * Pipeline:
 *   token -> (CHAR mode: letter name) -> SAM reciter -> phonemes
 *         -> SAM's renderer, 8 bits at 22050 Hz
 *         -> linear interpolation to 48 kHz, Q15
 *
 * Bounds: phrase <= SAYIT_PHRASE_MAX bytes, tokens <= SAYIT_TOKENS_MAX,
 * token text <= SAYIT_TOKEN_MAX chars, render <= SAM's own frame tables.
 * No allocation, no I/O, bounded work per sample.
 */
#ifndef SAYIT_H
#define SAYIT_H

#include <stdint.h>

#include "sam/sam_api.h"

#define SAYIT_PHRASE_MAX  256
/* One phrase and one SAM engine per track. The machine's mod.json sets it:
 * the Digitone has four synth tracks, the Digitakt eight audio tracks. */
#ifndef SAYIT_TRACKS
#define SAYIT_TRACKS      4
#endif
#define SAYIT_TOKENS_MAX  32
#define SAYIT_TOKEN_MAX   32
#define SAYIT_EXPAND_MAX  24

#define SAYIT_WORD 0
#define SAYIT_CHAR 1

/* The phrase starts at C5: MIDI 60 in the note naming the Digitone itself
 * uses, where MIDI 72 reads as C6 (the unit's name is the letter for note %
 * 12 plus the number note / 12). C5 is token 0 and each semitone above it is
 * the next token: a base of 72 would be C6, one octave too high, and the first
 * word would land on the wrong key. There is no ROOT knob; the base is the
 * machine's. */
#define SAYIT_BASE 60

/* Voice-parameter ranges.  SAM's speed is its frames-per-glottal-pulse
 * counter, 1..127; mouth and throat are 0..255 formant controls. */
#define SAYIT_SPEED_MIN  1
#define SAYIT_SPEED_MAX  127
#define SAYIT_VOICE_MIN  0
#define SAYIT_VOICE_MAX  127

/* SAM renders at 22050 Hz; the machine renders at 48000 Hz. */
#define SAYIT_SRC_RATE   22050
#define SAYIT_STEP       30106   /* 22050/48000 in Q16 (exact would be 30105.6) */

static int32_t sayit_clamp32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* The unit's note name: the letter for note % 12 and the number note / 12,
 * so 60 is C5 and 72 is C6, which is what the Digitone's own screens print.
 * buf must hold 5 bytes ("C#11" + NUL). Returns buf ("" for a null buf). */
static const char *sayit_note_name(int32_t note, char *buf)
{
    static const char letters[12] = { 'C', 'C', 'D', 'D', 'E', 'F',
                                      'F', 'G', 'G', 'A', 'A', 'B' };
    static const char sharps[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
    int32_t n = sayit_clamp32(note, 0, 127);
    int32_t oct = n / 12, pc = n % 12, i = 0;
    if (!buf)
        return "";
    buf[i++] = letters[pc];
    if (sharps[pc])
        buf[i++] = '#';
    if (oct >= 10) {
        buf[i++] = (char)('0' + oct / 10);
        buf[i++] = (char)('0' + oct % 10);
    } else {
        buf[i++] = (char)('0' + oct);
    }
    buf[i] = 0;
    return buf;
}

/* ---- tokenising --------------------------------------------------------- */

/* Split phrase into tokens. WORD: runs of non-whitespace (punctuation stays
 * attached). CHAR: each non-whitespace char is one token. Returns count
 * (0 for empty/whitespace-only). Overlong words truncate to SAYIT_TOKEN_MAX;
 * tokens past SAYIT_TOKENS_MAX are dropped. */
static int32_t sayit_tokenise(const char *phrase, int32_t mode,
                              uint16_t *offs, uint8_t *lens, int32_t max_tokens)
{
    int32_t n = 0, i = 0;
    if (!phrase || !offs || !lens || max_tokens <= 0)
        return 0;
    if (mode != SAYIT_WORD && mode != SAYIT_CHAR)
        mode = SAYIT_WORD;
    while (phrase[i] && n < max_tokens) {
        while (phrase[i] == ' ' || phrase[i] == '\t' ||
               phrase[i] == '\n' || phrase[i] == '\r')
            i++;
        if (!phrase[i])
            break;
        if (mode == SAYIT_CHAR) {
            offs[n] = (uint16_t)i;
            lens[n] = 1;
            n++;
            i++;
        } else {
            int32_t start = i, len = 0;
            while (phrase[i] && phrase[i] != ' ' && phrase[i] != '\t' &&
                   phrase[i] != '\n' && phrase[i] != '\r') {
                i++;
                len++;
            }
            if (len > SAYIT_TOKEN_MAX)
                len = SAYIT_TOKEN_MAX;
            offs[n] = (uint16_t)start;
            lens[n] = (uint8_t)len;
            n++;
        }
    }
    return n;
}

/* token index = note - base. The base is C5 (SAYIT_BASE, the Keyboard's C
 * key at the default octave) and each semitone above it is the next token,
 * so a CHAR-mode "say it" is S on C, A on C#, Y on D. A note below C5, or
 * past the last token, plays nothing: wrapping the index would make a short
 * phrase restart under the player's fingers, and the pitch restart with it.
 * An empty phrase (count <= 0) -> -1, and the render stays silent. */
static int32_t sayit_token_for_note(int32_t note, int32_t root, int32_t count)
{
    int32_t idx;
    if (count <= 0)
        return -1;
    idx = sayit_clamp32(note, 0, 127) - sayit_clamp32(root, 0, 127);
    if (idx < 0 || idx >= count)
        return -1;
    return idx;
}

/* English letter/digit names for single-character tokens ("B" -> "bee").
 * Writes a lower-case expansion into out (out_sz incl. NUL). Returns length
 * without NUL. Non-alphanumeric input copies the char unchanged. */
static int32_t sayit_expand_token(const char *tok, int32_t len,
                                  char *out, int32_t out_sz)
{
    static const char *const names[36] = {
        "ay", "bee", "see", "dee", "ee", "eff", "gee", "aych",
        "eye", "jay", "kay", "ell", "emm", "enn", "oh", "pee",
        "kyoo", "ar", "ess", "tee", "yoo", "vee", "doubleyou", "ex",
        "why", "zee", "zero", "one", "two", "three", "four", "five",
        "six", "seven", "eight", "nine"
    };
    int32_t k = 0, idx = -1;
    const char *s;
    if (!tok || !out || out_sz <= 0)
        return 0;
    if (len == 1) {
        char c = tok[0];
        if (c >= 'a' && c <= 'z')
            idx = c - 'a';
        else if (c >= 'A' && c <= 'Z')
            idx = c - 'A';
        else if (c >= '0' && c <= '9')
            idx = 26 + (c - '0');
    }
    s = idx >= 0 ? names[idx] : 0;
    if (!s) {
        int32_t n = len < out_sz - 1 ? len : out_sz - 1;
        int32_t j;
        if (n < 0) n = 0;
        for (j = 0; j < n; j++)
            out[j] = tok[j];
        out[n] = 0;
        return n;
    }
    while (*s && k < out_sz - 1)
        out[k++] = *s++;
    out[k] = 0;
    return k;
}

/* MODE sits on the stock FDBK knob (0..127): the lower half speaks words,
 * the upper half speaks characters. Two values, two halves of one turn. */
static int32_t sayit_map_mode(int32_t knob)
{
    return (knob >> 8) < 64 ? SAYIT_WORD : SAYIT_CHAR;
}

/* Knob clamps for the controls. */
static int32_t sayit_map_speed(int32_t knob) { return sayit_clamp32(knob, 0, SAYIT_SPEED_MAX); }
static int32_t sayit_map_voice(int32_t knob) { return sayit_clamp32(knob, SAYIT_VOICE_MIN, SAYIT_VOICE_MAX); }

/* PIT rides the stock HARM knob, whose kit value is the parameter's own
 * position 37..89 with 63 at the centre -- NOT semitones, which is why the
 * raw value had to be mapped rather than displayed. The full turn is two
 * octaves each way: +24 semitones clockwise, -24 counter-clockwise, which
 * works out at about one semitone per position (one semitone per detent). */
#define SAYIT_HARM_CENTRE 63
#define SAYIT_HARM_SPAN   26
#define SAYIT_PIT_MAX     24

static int32_t sayit_map_pitch(int32_t knob)
{
    int32_t v = sayit_clamp32(knob, SAYIT_HARM_CENTRE - SAYIT_HARM_SPAN,
                              SAYIT_HARM_CENTRE + SAYIT_HARM_SPAN) -
                SAYIT_HARM_CENTRE;
    int32_t st = v >= 0 ? (v * SAYIT_PIT_MAX + SAYIT_HARM_SPAN / 2) / SAYIT_HARM_SPAN
                        : -((-v * SAYIT_PIT_MAX + SAYIT_HARM_SPAN / 2) / SAYIT_HARM_SPAN);
    return sayit_clamp32(st, -SAYIT_PIT_MAX, SAYIT_PIT_MAX);
}

/* The phrase starts at C5 and climbs a semitone per token, so a CHAR-mode
 * "say it" is S on C, A on C#, Y on D; PIT (SYN1 E) adds a bipolar offset
 * in semitones on top. The total is bounded so a far-out note cannot push
 * SAM's period to its rails. */
static int32_t sayit_pitch_steps_for_note(int32_t note, int32_t base,
                                          int32_t pitch_knob)
{
    int32_t steps = sayit_map_pitch(pitch_knob) +
        sayit_clamp32(note, 0, 127) - sayit_clamp32(base, 0, 127);
    return sayit_clamp32(steps, -48, 48);
}

/* ---- the voice --------------------------------------------------------- */

/* One voice only: SAM's own default, the numbers s-macke/SAM starts with
 * (speed 72, pitch 64, throat 128, mouth 128). SPD, PIT, THRT and MOUT are
 * offsets around those values. SAM's speed counter runs the other way --
 * a LARGER number is SLOWER (SAM spends that many glottal steps on each
 * 10 ms frame) -- so SPD is presented as a rate: turning it up speeds the
 * voice up. pitch, mouth and throat are SAM's 0..255 controls. */
#define SAYIT_SAM_SPEED  72
#define SAYIT_SAM_PITCH  64
#define SAYIT_SAM_THROAT 128
#define SAYIT_SAM_MOUTH  128

/* Transposition as a Q15 multiplier on the glottal period: 32768 is
 * unison, 16384 one octave up (half the period), 65536 one octave down.
 * The port applies it to every frame's finished period, so the interval is
 * exact no matter what the formant subtraction did to that frame (see
 * sam/sam_api.h). Integer math keeps this safe in the audio callback.
 *
 * Never returns 0: a zero period wraps SAM's unsigned pulse counter. */
static uint32_t sayit_period_q15(int32_t semitones)
{
    static const uint32_t shorter[12] = {        /* n semitones up */
        32768, 30929, 29193, 27554, 26008, 24548,
        23170, 21868, 20643, 19484, 18384, 17352
    };
    static const uint32_t longer[12] = {         /* n semitones down */
        32768, 34716, 36781, 38968, 41285, 43755,
        46341, 49045, 51989, 55035, 58386, 61858
    };
    int32_t oct, rem;
    uint32_t v;

    semitones = sayit_clamp32(semitones, -36, 36);
    if (semitones >= 0) {
        oct = semitones / 12;
        rem = semitones % 12;
        v = shorter[rem];
        while (oct-- > 0)
            v >>= 1;
    } else {
        semitones = -semitones;
        oct = semitones / 12;
        rem = semitones % 12;
        v = longer[rem];
        while (oct-- > 0 && v <= 131072u)
            v <<= 1;
    }
    if (v < 1024u)
        v = 1024u;              /* 1/32 period: well inside SAM's range */
    return v;
}

/* Resolve the four controls into SAM's settings. pit_steps is ALREADY
 * semitones (sayit_map_pitch's output, plus the note offset): the HARM
 * position is mapped to semitones once, in sayit_pitch_steps_for_note, so
 * this function must not map it again -- a second map clamps every value below
 * 37 to -12 and pins the pitch an octave down. SAM's own
 * pitch stays at its stock value and the semitones ride period_q15, because
 * a `pitch` value is not a fixed interval once F1 is subtracted. SPD and
 * THRT/MOUT are centred at 64; THRT/MOUT double their offsets because SAM's
 * formant controls span 0..255 while the knob spans 0..127. */
static void sayit_resolve(int32_t spd_knob, int32_t pit_steps,
                          int32_t thr_knob, int32_t mou_knob,
                          struct sam_settings *out)
{
    int32_t rate;
    if (!out)
        return;
    out->pitch = SAYIT_SAM_PITCH;
    out->period_q15 = (unsigned long)sayit_period_q15(pit_steps);
    /* SPD knob: 64 = SAM's own speed, higher = faster. SAM's counter runs
     * the other way, so the rate is mirrored before it reaches the engine. */
    rate = (128 - SAYIT_SAM_SPEED) + (sayit_clamp32(spd_knob, 0, 127) - 64);
    out->speed = (unsigned char)(128 - sayit_clamp32(rate, 1, SAYIT_SPEED_MAX));
    out->throat = (unsigned char)sayit_clamp32(
        SAYIT_SAM_THROAT + (sayit_clamp32(thr_knob, 0, 127) - 64) * 2,
        0, 255);
    out->mouth = (unsigned char)sayit_clamp32(
        SAYIT_SAM_MOUTH + (sayit_clamp32(mou_knob, 0, 127) - 64) * 2,
        0, 255);
    out->sing = 0;
}

/* ---- voice state -------------------------------------------------------- */

/* Per-voice state: SAM's engine is monophonic within a track (newest note
 * wins), so only the resampling and the end fade live per voice. Each voice
 * pulls from its track's engine, not from a shared one: two tracks can speak
 * at the same time, and the phrase is per track too (engine.c). */
struct sayit_voice {
    int32_t active;     /* 1 while a token is sounding */
    int32_t track;      /* the track (and so the engine) it speaks on, 0..3 */
    int32_t gen;        /* generation: newest note wins */
    int32_t pos;        /* Q16 position between prev and cur */
    int32_t prev;       /* last source sample, 0..255 */
    int32_t cur;        /* next source sample, 0..255 */
    int32_t fade;       /* samples of tail fade remaining */
    int32_t level;      /* the pre-fade output, to fade from */
    /* The note the token started on and the settings the engine is speaking
     * with. The machine reads its knobs (and the LFOs on them) every block,
     * so a token already speaking follows a turned PIT or SPD: `set` is what
     * was last handed to the engine, and a difference is what it gets next
     * (engine.c's sayit_live_controls; SAM bakes the rest in per chunk). */
    int32_t note;
    struct sam_settings set;
    /* A note whose token had no stream yet: the render stays silent and asks
     * the UI task for it (sayit_plan_want_token) instead of reciting the token
     * in the audio block. want_idx is -1 when nothing is waiting. */
    int32_t want_idx;
    int32_t want_note;
    int32_t want_wait;  /* blocks spent waiting, bounded by SAYIT_WANT_BLOCKS */
};

/* How long a note may wait for its token's stream. 48 blocks is 32 ms: a UI
 * tick or two, and far inside the length of the shortest word, so a note that
 * is actually going to be planned always gets there. Past this the token is
 * dropped rather than left holding the track. */
#define SAYIT_WANT_BLOCKS 48

#define SAYIT_FADE 48        /* samples (~1 ms at 48 kHz) */

static void sayit_voice_init(struct sayit_voice *s)
{
    if (!s)
        return;
    s->active = 0;
    s->track = 0;
    s->gen = 0;
    s->pos = 0;
    s->prev = 128;
    s->cur = 128;
    s->fade = 0;
    s->level = 0;
    s->note = 0;
    s->want_idx = -1;
    s->want_note = 0;
    s->want_wait = 0;
    s->set.speed = 0;
    s->set.pitch = 0;
    s->set.mouth = 0;
    s->set.throat = 0;
    s->set.sing = 0;
    s->set.period_q15 = 0;
}

/* The bookkeeping a token that has begun needs, shared by the text start below
 * and the plan start after it. */
static void sayit_voice_begun(struct sayit_voice *s, int32_t engine, int32_t first,
                              const struct sam_settings *st, int32_t gen)
{
    s->track = engine;
    s->active = 1;
    s->gen = gen;
    s->pos = 0;
    s->prev = first;
    s->cur = first;
    s->fade = 0;
    s->level = 0;
    s->want_idx = -1;
    s->set = *st;
}

/* Start a token on track `engine`'s own SAM engine: text/len is the (already
 * expanded) token and the four controls set the voice. pit_steps is in
 * semitones (see sayit_resolve). Returns 1 if anything will sound.
 *
 * A machine never calls this: reciting a token costs SAM's reciter and its
 * parsers, which no audio block can hold, so a machine starts from the plan
 * (sayit_voice_start_plan) and waits for it when it is not there yet. The
 * host tests keep it as the reference the plan path is compared against, so a
 * machine build defines SAYIT_NO_RECITE and this is not compiled. */
#ifndef SAYIT_NO_RECITE
static int32_t sayit_voice_start(struct sayit_voice *s, int32_t engine,
                                 const char *text, int32_t len,
                                 int32_t spd_knob, int32_t pit_steps,
                                 int32_t thr_knob, int32_t mou_knob,
                                 int32_t gen)
{
    struct sam_settings st;
    int32_t first;
    if (!s)
        return 0;
    engine = sayit_clamp32(engine, 0, SAM_ENGINES - 1);
    sayit_resolve(spd_knob, pit_steps, thr_knob, mou_knob, &st);
    if (!sam_voice_begin_tr(engine, text, len, &st)) {
        s->active = 0;
        return 0;
    }
    first = sam_voice_next_tr(engine);
    if (first < 0) {
        s->active = 0;
        return 0;
    }
    sayit_voice_begun(s, engine, first, &st, gen);
    return 1;
}
#endif /* SAYIT_NO_RECITE */

/* Start a token from a stream the UI task recited ahead of the note
 * (sayit_plan): there is no reciting here, only the chunk's frames. */
static int32_t sayit_voice_start_plan(struct sayit_voice *s, int32_t engine,
                                      const uint8_t *plan, int32_t nph,
                                      int32_t spd_knob, int32_t pit_steps,
                                      int32_t thr_knob, int32_t mou_knob,
                                      int32_t gen)
{
    struct sam_settings st;
    int32_t first;
    if (!s)
        return 0;
    engine = sayit_clamp32(engine, 0, SAM_ENGINES - 1);
    sayit_resolve(spd_knob, pit_steps, thr_knob, mou_knob, &st);
    if (!sam_voice_begin_plan_tr(engine, plan, nph, &st)) {
        s->active = 0;
        return 0;
    }
    first = sam_voice_next_tr(engine);
    if (first < 0) {
        s->active = 0;
        return 0;
    }
    sayit_voice_begun(s, engine, first, &st, gen);
    return 1;
}

/* ---- the plan: the recitation done ahead of the trigger ------------------ */
/* Starting a token costs SAM's reciter and its parsers, which walk thousands of
 * bytes of tables and buffers. That is not work an audio block can hold: the
 * render runs at interrupt level, 1500 blocks a second, a 667 us block, and a
 * machine's code and tables are in DDR, where each of those walks is a bus trip
 * with no cache in front of it. Reciting inside the render therefore spends a
 * block's whole budget and more on one trigger, which the unit shows as a DSP
 * spike and which clips another track's voice.
 *
 * So the UI task recites instead: each phrase's tokens become phoneme streams
 * once, a token a tick (sayit_plan_step), and a note start only copies the
 * stream it needs into the track's engine (sayit_voice_start_plan). A plan
 * belongs to the phrase and the mode it was built for; when either moves it is
 * dropped and built again.
 *
 * The render never recites, not even for a token the plan has not reached: a
 * note that finds no stream goes silent for a block or two and asks for that
 * token (sayit_plan_want_token), which the next tick builds first. A word can
 * therefore start a millisecond late; it can never stall an audio block.
 *
 * The storage lives in the machine (engine.c declares one per track): this
 * header is included by more than one file of a machine, so it holds the
 * helpers and not the plans. */
#ifndef SAYIT_PLAN_BYTES
#define SAYIT_PLAN_BYTES 3072      /* per track, in bytes of packed phonemes */
#endif
#define SAYIT_PLAN_NONE  0xffff

struct sayit_plan {
    int32_t  mode;                          /* the mode it was built for, or -1 */
    uint8_t  done;                          /* every token built */
    uint8_t  count;                         /* tokens in the phrase */
    uint8_t  next;                          /* the token the next step builds */
    uint16_t used;                          /* bytes of `pool` in use */
    uint16_t toff[SAYIT_TOKENS_MAX];        /* token -> its text in the phrase */
    uint8_t  tlen[SAYIT_TOKENS_MAX];
    uint16_t off[SAYIT_TOKENS_MAX];         /* token -> its stream, or NONE */
    uint8_t  nph[SAYIT_TOKENS_MAX];
    uint8_t  pool[SAYIT_PLAN_BYTES];
};

/* Forget the plan: the phrase or the mode it was built for has moved. The
 * machine calls this where the phrase is set, and the next tick builds again. */
static void sayit_plan_drop(struct sayit_plan *p)
{
    int32_t i;
    if (!p)
        return;
    p->mode = -1;
    p->done = 0;
    p->count = 0;
    p->next = 0;
    p->used = 0;
    for (i = 0; i < SAYIT_TOKENS_MAX; i++) {
        p->toff[i] = 0;
        p->tlen[i] = 0;
        p->off[i] = SAYIT_PLAN_NONE;
        p->nph[i] = 0;
    }
}

/* Can the plan answer for this mode? A plan is built a token a tick, so it
 * answers for the tokens it has built before the walk is done (see
 * sayit_plan_stream); `done` only says there is nothing left to build. */
static int32_t sayit_plan_ready(const struct sayit_plan *p, int32_t mode)
{
    return p && p->mode == mode;
}

/* The plan's stream for a token, or 0 when it has none for it: the plan is not
 * built for this mode yet, that token is later than the walk has reached, or
 * the pool ran out before it. A note that finds no stream does not recite the
 * token in the render any more (see sayit_plan_want_token): it asks for that
 * token and waits. */
static int32_t sayit_plan_stream(const struct sayit_plan *p, int32_t mode, int32_t idx,
                                 const uint8_t **stream, uint8_t *nph)
{
    if (!sayit_plan_ready(p, mode) || idx < 0 || idx >= (int32_t)p->count)
        return 0;
    if (p->off[idx] == SAYIT_PLAN_NONE)
        return 0;
    *stream = p->pool + p->off[idx];
    *nph = p->nph[idx];
    return 1;
}

/* Build one token's stream into the plan. 0 when it could not be built (the
 * pool is full), 1 otherwise; an entry that says nothing is built as an empty
 * stream, which is a token that plays silence rather than one that is late. */
static int32_t sayit_plan_token(struct sayit_plan *p, const char *phrase, int32_t i)
{
    int32_t k, len, exp_len, n, room;
    char text[SAYIT_EXPAND_MAX];

    if (!p || i < 0 || i >= (int32_t)p->count)
        return 0;
    if (p->off[i] != SAYIT_PLAN_NONE)
        return 1;                       /* already built */
    len = p->tlen[i];
    /* The same text the note start would recite: one character is a letter
     * name, anything longer is the word itself (see the machines' render). */
    if (len == 1) {
        exp_len = sayit_expand_token(phrase + p->toff[i], len, text, sizeof(text));
    } else {
        exp_len = len < (int32_t)sizeof(text) - 1
            ? len : (int32_t)sizeof(text) - 1;
        for (k = 0; k < exp_len; k++)
            text[k] = phrase[p->toff[i] + k];
        text[exp_len] = 0;
    }
    if (exp_len <= 0) {                 /* says nothing: an empty entry, not none */
        p->off[i] = p->used;
        p->nph[i] = 0;
        return 1;
    }
    room = (int32_t)sizeof(p->pool) - (int32_t)p->used;
    n = sam_plan_build(text, exp_len, p->pool + p->used, room);
    if (n < 0)
        return 0;                       /* the pool is full: no stream */
    p->off[i] = p->used;
    p->nph[i] = (uint8_t)n;
    p->used = (uint16_t)(p->used + 3 * n);
    return 1;
}

/* One token a call: the UI task builds a phrase's plan a token a tick, so a
 * phrase or mode change costs a few hundred microseconds a frame rather than a
 * stall. Returns 1 while there is more to build. A token whose stream does not
 * fit the pool is left without one - the render waits for it and then gives
 * up, which is a dropped token rather than a stall in the audio block. */
static int32_t sayit_plan_step(struct sayit_plan *p, const char *phrase, int32_t mode)
{
    int32_t i;

    if (!p)
        return 0;
    if (p->mode != mode) {              /* a new phrase, or a moved MODE */
        sayit_plan_drop(p);
        p->mode = mode;
        p->count = (uint8_t)sayit_tokenise(phrase, mode, p->toff, p->tlen,
                                           SAYIT_TOKENS_MAX);
    }
    if (p->done)
        return 0;
    while (p->next < p->count && p->off[p->next] != SAYIT_PLAN_NONE)
        p->next++;                      /* built already, by a want */
    if (p->next >= p->count) {
        p->done = 1;
        return 0;
    }
    i = p->next++;
    sayit_plan_token(p, phrase, i);
    return 1;
}

/* Build one token out of its turn, for a note the render is waiting on.
 *
 * A note can arrive before the walk has reached its token: a phrase typed a
 * moment ago, a project just loaded, a token later in a long phrase than the
 * walk has got to. The render may not recite it any more - that is the block
 * the unit cannot hold - so it goes silent and asks for that token, and the
 * next tick builds that one alone (a few hundred microseconds) instead of the
 * walk's turn. One tick of delay, then the word starts. */
static int32_t sayit_plan_want_token(struct sayit_plan *p, const char *phrase,
                                     int32_t mode, int32_t idx)
{
    if (!p)
        return 0;
    if (p->mode != mode) {
        sayit_plan_drop(p);
        p->mode = mode;
        p->count = (uint8_t)sayit_tokenise(phrase, mode, p->toff, p->tlen,
                                           SAYIT_TOKENS_MAX);
    }
    if (idx < 0 || idx >= (int32_t)p->count)
        return 0;
    return sayit_plan_token(p, phrase, idx);
}

/* ---- the start of a fresh note ------------------------------------------ */

/* What a fresh note is to start: the plan's stream when the UI task has one for
 * its token, else the text to recite here. */
struct sayit_start {
    const uint8_t *plan;    /* the plan's stream, or 0 */
    uint8_t  nph;           /* how many phonemes it holds */
    const char *text;       /* the text to recite, or 0 */
    int32_t  len;
    int32_t  count;         /* tokens in the phrase, for the note map */
    int32_t  idx;           /* the token the note names */
};

/* Resolve `note` on a track: which token it is, and where the stream for it
 * comes from. Returns 0 when the note has nothing to say - an empty phrase, or
 * a note outside C5..C5+count-1 - and the render writes silence. offs, lens and
 * expanded are the caller's scratch, used only on the paths that recite. */
static int32_t sayit_start_resolve(struct sayit_start *st, const struct sayit_plan *p,
                                   const char *phrase, int32_t mode, int32_t note,
                                   uint16_t *offs, uint8_t *lens,
                                   char *expanded, int32_t expanded_sz)
{
    int32_t count, idx, have;
#ifndef SAYIT_NO_RECITE
    int32_t tlen, exp_len, i;
    const char *tok;
#endif

    if (!st)
        return 0;
    st->plan = 0;
    st->nph = 0;
    st->text = 0;
    st->len = 0;
    st->idx = -1;
    st->count = 0;
    have = sayit_plan_ready(p, mode);
    if (have)
        count = (int32_t)p->count;      /* no tokenising in the audio callback */
    else
        count = sayit_tokenise(phrase, mode, offs, lens, SAYIT_TOKENS_MAX);
    st->count = count;
    idx = sayit_token_for_note(note, SAYIT_BASE, count);
    if (idx < 0)
        return 0;
    st->idx = idx;
    if (sayit_plan_stream(p, mode, idx, &st->plan, &st->nph))
        return 1;                       /* recited ahead of the note */
#ifdef SAYIT_NO_RECITE
    /* A machine does not recite here: the stream is coming, because the note
     * asked the UI task for that token (sayit_plan_want_token), and until it
     * arrives the voice is silent. Nothing below runs in a machine build. */
    (void)phrase;
    (void)offs;
    (void)lens;
    (void)expanded;
    (void)expanded_sz;
    return 1;
#else
    if (have) {                         /* the plan knows the token's text */
        tok = phrase + p->toff[idx];
        tlen = p->tlen[idx];
    } else {
        tok = phrase + offs[idx];
        tlen = lens[idx];
    }
    if (tlen == 1) {
        exp_len = sayit_expand_token(tok, tlen, expanded, expanded_sz);
    } else {
        exp_len = tlen < expanded_sz - 1 ? tlen : expanded_sz - 1;
        for (i = 0; i < exp_len; i++)
            expanded[i] = tok[i];
        expanded[exp_len] = 0;
    }
    if (exp_len <= 0)
        return 0;
    st->text = expanded;
    st->len = exp_len;
    return 1;
#endif /* SAYIT_NO_RECITE */
}

/* One Q15 sample at 48 kHz. Bounded work: at most one source sample and one
 * interpolation per call. The stream ends with a ~1 ms fade so the last
 * sample does not step to silence. */
static int32_t sayit_frame(struct sayit_voice *s)
{
    int32_t n, out;
    if (!s || !s->active)
        return 0;
    s->pos += SAYIT_STEP;
    while (s->pos >= 65536) {
        s->pos -= 65536;
        s->prev = s->cur;
        n = sam_voice_next_tr(s->track);
        if (n < 0) {
            /* SAM's stream is done: hold the last sample and fade out. */
            if (s->fade == 0)
                s->fade = SAYIT_FADE;
            break;
        }
        s->cur = n;
    }
    out = s->prev + (((s->cur - s->prev) * s->pos) >> 16);   /* 0..255 */
    out = (out - 128) << 8;                                 /* Q15 */
    if (s->fade > 0) {
        out = (out * s->fade) / SAYIT_FADE;
        s->level = out;
        s->fade--;
        if (s->fade == 0)
            s->active = 0;
    } else {
        s->level = out;
    }
    return out;
}

#endif /* SAYIT_H */
