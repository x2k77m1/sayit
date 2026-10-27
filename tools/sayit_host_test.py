#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""SAYIT host tests: compile the real C core (sayit.h, sayit_edit.h and the
SAM port) natively with stub companion helpers and drive every
Definition-of-Done case.

Covers: whitespace tokenisation, WORD/CHAR modes, letter-name handling, the
C4-relative note mapping (no wrap: below C4 and past the phrase are silent),
empty input, overlong input,
maximum token count, the SAM reciter's phoneme output, the four voice
controls, voice lifecycle (bounded, deterministic, no onset click),
the factory naming editor (cursor, character stepping, delete/space, pop-up
grid), and repository hygiene. The port's audio is compared against the
upstream SAM program by tools/sam_parity.py, not here.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EL_INC = os.path.join(ROOT, "3rdparty", "elekloader", "elekloader", "sdk", "include")

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "digitone-mk1/machines.h"
#include "sayit.h"
#include "sayit_edit.h"
#include "machines/digitakt/phrase_store.h"

static int32_t knob_vals[DNM_KNOBS];
static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) { printf("PASS %s\n", name); } \
    else { printf("FAIL %s\n", name); fails++; } } while (0)

int32_t dnm_knob(int32_t voice, int32_t knob) {
    (void)voice;
    if (knob < 0 || knob >= DNM_KNOBS) return 0;
    return knob_vals[knob];
}
void dnm_knob_range(int32_t knob, int32_t *min, int32_t *max) {
    (void)knob;
    if (min) *min = 0;
    if (max) *max = 127 << 8;
}
uint32_t dnm_phase_inc(int32_t pitch) { (void)pitch; return 1; }
int32_t dnm_sin(uint32_t phase) {
    double x = (double)phase * (6.28318530717958647692 / 4294967296.0);
    return (int32_t)(sin(x) * 32767.0);
}

static int32_t ph_is(const uint8_t *ph, int32_t n, const uint8_t *want, int32_t wn) {
    int32_t i;
    if (n != wn) return 0;
    for (i = 0; i < n; i++)
        if (ph[i] != want[i]) return 0;
    return 1;
}

int main(void) {
    uint16_t offs[32];
    uint8_t lens[32];
    char exp[24];
    int32_t n, i, s, peak;
    struct sayit_voice sv;

    /* ---- tokenising ---- */
    n = sayit_tokenise("a  b   c d", SAYIT_WORD, offs, lens, 32);
    CHECK(n == 4, "word-split-a-b-c-d");
    CHECK(lens[0] == 1 && lens[3] == 1, "word-lens");
    n = sayit_tokenise("hello, world!", SAYIT_WORD, offs, lens, 32);
    CHECK(n == 2 && lens[0] == 6 && lens[1] == 6, "punct-attached");
    n = sayit_tokenise("a b c", SAYIT_CHAR, offs, lens, 32);
    CHECK(n == 3, "char-mode-count");
    n = sayit_tokenise("hi!", SAYIT_CHAR, offs, lens, 32);
    CHECK(n == 3, "char-mode-punct-each");
    CHECK(sayit_tokenise("", SAYIT_WORD, offs, lens, 32) == 0, "empty");
    CHECK(sayit_tokenise("   ", SAYIT_WORD, offs, lens, 32) == 0, "spaces-only");
    {
        char longw[80];
        memset(longw, 'x', sizeof(longw) - 1);
        longw[sizeof(longw) - 1] = 0;
        n = sayit_tokenise(longw, SAYIT_WORD, offs, lens, 32);
        CHECK(n == 1 && lens[0] == SAYIT_TOKEN_MAX, "overlong-trunc");
    }
    n = sayit_tokenise("a a a a a a a a a a a a a a a a a a a a "
                       "a a a a a a a a a a a a a a a a a a a a",
                       SAYIT_WORD, offs, lens, 32);
    CHECK(n == 32, "max-tokens");

    /* ---- letter names, note mapping, scaling ---- */
    CHECK(sayit_expand_token("B", 1, exp, sizeof(exp)) == 3
          && strcmp(exp, "bee") == 0, "letter-B-bee");
    CHECK(sayit_expand_token("a", 1, exp, sizeof(exp)) == 2
          && strcmp(exp, "ay") == 0, "letter-a-ay");
    CHECK(sayit_expand_token("5", 1, exp, sizeof(exp)) == 4
          && strcmp(exp, "five") == 0, "digit-5-five");
    CHECK(sayit_expand_token("hi", 2, exp, sizeof(exp)) == 2
          && strcmp(exp, "hi") == 0, "multi-no-expand");
    /* The root is C5, which the unit names 60 (it calls 72 C6). A base of 72
     * would be C6 and the first word would sit an octave too high. */
    CHECK(SAYIT_BASE == 60 &&
          sayit_token_for_note(60, SAYIT_BASE, 2) == 0 &&
          sayit_token_for_note(61, SAYIT_BASE, 2) == 1 &&
          sayit_token_for_note(62, SAYIT_BASE, 2) == -1 &&
          sayit_token_for_note(59, SAYIT_BASE, 2) == -1,
          "c5-key-starts-phrase");
    {
        char nm[5];
        CHECK(strcmp(sayit_note_name(60, nm), "C5") == 0,
              "note-name-C5-is-60");
        CHECK(strcmp(sayit_note_name(72, nm), "C6") == 0,
              "note-name-C6-is-72");
        CHECK(strcmp(sayit_note_name(61, nm), "C#5") == 0,
              "note-name-sharp");
        CHECK(strcmp(sayit_note_name(0, nm), "C0") == 0 &&
              strcmp(sayit_note_name(127, nm), "G10") == 0,
              "note-name-ends");
        CHECK(strcmp(sayit_note_name(-5, nm), "C0") == 0 &&
              strcmp(sayit_note_name(200, nm), "G10") == 0,
              "note-name-clamped");
    }
    CHECK(sayit_token_for_note(60, 60, 4) == 0, "base-selects-0");
    CHECK(sayit_token_for_note(63, 60, 4) == 3, "semitone-selects-3");
    CHECK(sayit_token_for_note(64, 60, 4) == -1, "past-phrase-silent");
    CHECK(sayit_token_for_note(59, 60, 4) == -1, "below-root-silent");
    CHECK(sayit_token_for_note(60, 60, 2) == 0 &&
          sayit_token_for_note(61, 60, 2) == 1 &&
          sayit_token_for_note(62, 60, 2) == -1,
          "keyboard-climbs-tokens");
    CHECK(sayit_token_for_note(60, 60, 0) == -1, "empty-silent");
    CHECK(sayit_token_for_note(59, 60, 4) == -1, "below-base-silent");
    CHECK(sayit_map_speed(-5) == 0 && sayit_map_speed(200) == 127, "speed-clamp");
    /* PIT rides HARM, whose kit value is the parameter's position 37..89
     * with 63 at the centre: the full turn is two octaves each way. */
    CHECK(sayit_map_pitch(63) == 0, "pit-centre-unison");
    CHECK(sayit_map_pitch(89) == 24 && sayit_map_pitch(37) == -24,
          "pit-full-turn-is-two-octaves");
    CHECK(sayit_map_pitch(64) == 1 && sayit_map_pitch(65) == 2 &&
          sayit_map_pitch(67) == 4 && sayit_map_pitch(62) == -1 &&
          sayit_map_pitch(61) == -2, "pit-one-semitone-steps");
    CHECK(sayit_map_pitch(0) == -24 && sayit_map_pitch(127) == 24,
          "pit-clamp");
    /* Transposition is a Q15 multiplier on the glottal period, applied by the
     * port to every pulse it fetches: +12 halves it, +24 quarters it, -12
     * doubles it, -24 quadruples it. SAM's own `pitch` cannot do this -- it
     * is added before half of F1 is subtracted, so a value is not a fixed
     * interval. */
    CHECK(sayit_period_q15(0) == 32768, "transpose-unison");
    CHECK(sayit_period_q15(12) == 16384 && sayit_period_q15(24) == 8192 &&
          sayit_period_q15(-12) == 65536 && sayit_period_q15(-24) == 131072,
          "transpose-octaves");
    CHECK(sayit_period_q15(1) < 32768 && sayit_period_q15(-1) > 32768 &&
          sayit_period_q15(1) > sayit_period_q15(2),
          "transpose-one-semitone");
    CHECK(sayit_period_q15(0) != 0 && sayit_period_q15(48) != 0 &&
          sayit_period_q15(-48) != 0, "transpose-never-zero");
    CHECK(sayit_pitch_steps_for_note(72, 60, 63) == 12 &&
          sayit_pitch_steps_for_note(60, 60, 63) == 0,
          "c4-sets-speech-pitch");
    CHECK(sayit_pitch_steps_for_note(63, 60, 67) == 7 &&
          sayit_pitch_steps_for_note(63, 60, 61) == 1,
          "pitch-knob-is-semitones");
    CHECK(sayit_pitch_steps_for_note(120, 60, 89) == 48 &&
          sayit_pitch_steps_for_note(0, 60, 37) == -48,
          "pitch-steps-bounded");
    CHECK(sayit_map_voice(64) == 64, "voice-mid");
    CHECK(sayit_map_mode(0) == SAYIT_WORD, "mode-word");
    CHECK(sayit_map_mode(-30 << 8) == SAYIT_WORD, "mode-word-negative");
    CHECK(sayit_map_mode(64 << 8) == SAYIT_CHAR &&
          sayit_map_mode(127 << 8) == SAYIT_CHAR, "mode-char");

    /* ---- SAM's reciter: the port must speak what SAM speaks ---- */
    {
        struct sam_settings st = { 72, 64, 128, 128, 0 };
        const unsigned char *ph;
        CHECK(sam_voice_begin("say", 3, &st) == 1, "sam-say-speaks");
        ph = (const unsigned char *)sam_voice_phonemes();
        CHECK(ph[0] == 32 && ph[1] == 'S' && ph[2] == 'E' && ph[3] == 'Y' &&
              ph[4] == '5', "reciter-say");
        CHECK(sam_voice_begin("it", 2, &st) == 1, "sam-it-speaks");
        ph = (const unsigned char *)sam_voice_phonemes();
        CHECK(ph[0] == 32 && ph[1] == 'I' && ph[2] == 'H' && ph[3] == 'T',
              "reciter-it");
        CHECK(sam_voice_begin("hello", 5, &st) == 1, "sam-hello-speaks");
        ph = (const unsigned char *)sam_voice_phonemes();
        CHECK(ph[0] == 32 && ph[1] == '/' && ph[2] == 'H' && ph[3] == 'E' &&
              ph[4] == 'H' && ph[5] == 'L' && ph[6] == 'O' && ph[7] == 'W',
              "reciter-hello");
        /* digits go through SAM's own number rules ("two zero ...") */
        CHECK(sam_voice_begin("2024", 4, &st) == 1, "sam-digits-speak");
        ph = (const unsigned char *)sam_voice_phonemes();
        CHECK(ph[2] == 'T' && ph[3] == 'U' && ph[4] == 'W', "reciter-digits");
        CHECK(sam_voice_begin("   ", 3, &st) == 0, "sam-silence-for-spaces");
        CHECK(sam_voice_begin("", 0, &st) == 0, "sam-empty");
    }

    /* ---- the SAM voice and its four controls ---- */
    {
        struct sam_settings st;
        /* Centred controls must give SAM's own stock settings. */
        sayit_resolve(64, 0, 64, 64, &st);    /* PIT is in semitones */
        CHECK(st.pitch == 64 && st.mouth == 128 && st.throat == 128 &&
              st.speed == 72 && st.sing == 0 &&
              st.period_q15 == 32768, "sam-stock-voice");
        sayit_resolve(64, 12, 64, 64, &st);   /* PIT +12 */
        CHECK(st.pitch == 64 && st.period_q15 == 16384, "pit-octave-up");
        sayit_resolve(64, -12, 64, 64, &st);  /* PIT -12 */
        CHECK(st.pitch == 64 && st.period_q15 == 65536, "pit-octave-down");
        sayit_resolve(64, 1, 64, 64, &st);
        CHECK(st.period_q15 < 32768, "pit-one-semitone-up");
        sayit_resolve(74, 0, 64, 64, &st);    /* +10 rate -> -10 SAM */
        CHECK(st.speed == 62, "speed-offset");
        sayit_resolve(64, 0, 74, 74, &st);
        CHECK(st.throat == 148 && st.mouth == 148, "formant-offset");
        sayit_resolve(127, 12, 127, 127, &st);
        CHECK(st.speed == 9 && st.period_q15 == 16384 &&
              st.throat == 254 && st.mouth == 254, "controls-clamp-high");
        sayit_resolve(0, -12, 0, 0, &st);
        CHECK(st.speed == SAYIT_SPEED_MAX && st.period_q15 == 65536 &&
              st.throat == 0 && st.mouth == 0, "controls-clamp-low");
    }

    /* PIT through the engine's exact path: HARM's 37..89 position is mapped
     * to semitones ONCE, the note offset is added, and the total is
     * transposed at resolve. Mapping the semitone count a second time, as if it
     * were still a HARM position, clamps everything below 37 to -12: the page
     * value moves and the pitch does not. */
    {
        struct sam_settings s0, s1, s2;
        int32_t p0 = sayit_pitch_steps_for_note(SAYIT_BASE, SAYIT_BASE, 63);
        int32_t p1 = sayit_pitch_steps_for_note(SAYIT_BASE, SAYIT_BASE, 89);
        int32_t p2 = sayit_pitch_steps_for_note(SAYIT_BASE, SAYIT_BASE, 37);
        int32_t p3 = sayit_pitch_steps_for_note(SAYIT_BASE, SAYIT_BASE, 67);
        CHECK(p0 == 0 && p1 == 24 && p2 == -24 && p3 == 4,
              "pit-engine-steps");
        sayit_resolve(64, p0, 64, 64, &s0);
        sayit_resolve(64, p1, 64, 64, &s1);
        sayit_resolve(64, p2, 64, 64, &s2);
        CHECK(s0.period_q15 == 32768 && s1.period_q15 == 8192 &&
              s2.period_q15 == 131072,
              "pit-engine-every-knob-step-moves-pitch");
    }

    /* ---- the voice: bounded, deterministic, no click ---- */
    sayit_voice_init(&sv);
    CHECK(    sayit_voice_start(&sv, 0, "say", 3, 64, 0, 64, 64, 1) == 1,
          "voice-starts");
    peak = 0;
    for (i = 0; i < 200000; i++) {
        s = sayit_frame(&sv);
        if (s < 0) s = -s;
        if (s > peak) peak = s;
        if (!sv.active) break;
    }
    CHECK(peak > 100, "voice-sounds");
    CHECK(peak <= 32767, "voice-bounded");
    CHECK(!sv.active && i < 200000, "voice-finishes-bounded");
    CHECK(sayit_frame(&sv) == 0, "voice-inactive-silent");
    /* SAM is AC-coupled by construction: a stream that parks on a constant
     * is a stuck glottal pulse, heard as a thump. Count crossings. */
    {
        int zc = 0, prev = 0, pk = 0;
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 0, "say", 3, 64, 0, 64, 64, 11);
        for (i = 0; i < 24000; i++) {
            s = sayit_frame(&sv);
            if ((prev < 0) != (s < 0)) zc++;
            prev = s;
            if (s < 0) s = -s;
            if (s > pk) pk = s;
        }
        CHECK(zc > 100, "voice-is-ac-not-dc");
        CHECK(pk > 100 && pk < 32768, "voice-level-usable");
    }
    /* A voice the engine gets from .bss (all zeros) must be safe too. */
    {
        int zc = 0, prev = 0, pk = 0;
        memset(&sv, 0, sizeof(sv));
        sayit_voice_start(&sv, 0, "say", 3, 64, 0, 64, 64, 12);
        for (i = 0; i < 24000; i++) {
            s = sayit_frame(&sv);
            if ((prev < 0) != (s < 0)) zc++;
            prev = s;
            if (s < 0) s = -s;
            if (s > pk) pk = s;
        }
        CHECK(zc > 100, "voice-ac-from-bss");
        CHECK(pk < 32768, "voice-no-dc-saturation");
    }
    /* Two octaves down is four times SAM's own pulse -- longer than the byte
     * the pitch lives in, which is why the factor sits on the pulse. The
     * voice must still sound, stay bounded and finish. */
    {
        int32_t tops[2] = { 24, -24 };
        int32_t t;
        for (t = 0; t < 2; t++) {
            int pk = 0, n = 0;
            sayit_voice_init(&sv);
            CHECK(sayit_voice_start(&sv, 0, "say", 3, 64, tops[t], 64, 64,
                                    40 + t) == 1, "pit-extreme-starts");
            for (i = 0; i < 24000 && sv.active; i++) {
                s = sayit_frame(&sv);
                if (s < 0) s = -s;
                if (s > pk) pk = s;
                n++;
            }
            CHECK(pk > 100 && pk <= 32767 && n > 1000,
                  "pit-extreme-sounds-bounded");
        }
    }
    /* SAM left its first three output bytes uninitialised; the port starts
     * them at silence, so a word must not open with a click. */
    sayit_voice_init(&sv);
    sayit_voice_start(&sv, 0, "say", 3, 64, 0, 64, 64, 2);
    s = sayit_frame(&sv);
    if (s < 0) s = -s;
    CHECK(s == 0, "no-click-onset");
    /* Retrigger: the newest note wins and owns the engine. */
    sayit_voice_init(&sv);
    sayit_voice_start(&sv, 0, "say", 3, 64, 0, 64, 64, 3);
    for (i = 0; i < 100; i++)
        sayit_frame(&sv);
    CHECK(sayit_voice_start(&sv, 0, "it", 2, 100, 0, 64, 64, 4) == 1,
          "retrigger-starts");
    CHECK(sv.active && sv.gen == 4, "retrigger-newest");
    /* The same word twice renders the same samples: no hidden state. */
    {
        static int32_t a[24000], b[24000];
        int same = 1;
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 0, "hello", 5, 64, 0, 64, 64, 9);
        for (i = 0; i < 24000; i++)
            a[i] = sayit_frame(&sv);
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 0, "hello", 5, 64, 0, 64, 64, 9);
        for (i = 0; i < 24000; i++)
            b[i] = sayit_frame(&sv);
        for (i = 0; i < 24000; i++)
            if (a[i] != b[i]) same = 0;
        CHECK(same, "deterministic");
    }
    /* Each track has its own SAM engine: a word started on engine 1 must not
     * disturb what engine 0 is rendering, and both must sound. One engine for
     * every track would let T2 cut T1 off. */
    {
        static int32_t solo[6000], mixed[6000], other[6000];
        struct sayit_voice u, w;
        int32_t same = 1, p0 = 0, p1 = 0, spoke = 0, s;

        sayit_voice_init(&u);
        CHECK(sayit_voice_start(&u, 0, "say", 3, 64, 0, 64, 64, 101) == 1,
              "engine0-starts");
        for (i = 0; i < 6000; i++)
            solo[i] = sayit_frame(&u);

        sayit_voice_init(&u);
        sayit_voice_init(&w);
        sayit_voice_start(&u, 0, "say", 3, 64, 0, 64, 64, 101);
        for (i = 0; i < 6000; i++) {
            if (i == 1500)
                CHECK(sayit_voice_start(&w, 1, "it", 2, 64, 0, 64, 64, 102) == 1,
                      "engine1-starts-while-engine0-speaks");
            mixed[i] = sayit_frame(&u);
            other[i] = sayit_frame(&w);
            if (mixed[i] != solo[i])
                same = 0;
            s = mixed[i] < 0 ? -mixed[i] : mixed[i];
            if (s > p0) p0 = s;
            if (i > 1500) {
                s = other[i] < 0 ? -other[i] : other[i];
                if (s > p1) p1 = s;
                if (other[i] != 0) spoke = 1;
            }
        }
        CHECK(same, "engine0-unaffected-by-engine1");
        CHECK(p0 > 100, "engine0-keeps-sounding");
        CHECK(spoke && p1 > 100 && w.active, "engine1-speaks-too");
        CHECK(solo[0] == mixed[0] && solo[5999] == mixed[5999],
              "engine0-same-first-and-last");
    }
    /* Speed and pitch actually change the render. */
    {
        int32_t fast = 0, slow = 0;
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 0, "speech", 6, 127, 0, 64, 64, 21);
        for (i = 0; i < 200000 && sv.active; i++, fast++)
            sayit_frame(&sv);
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 0, "speech", 6, 1, 0, 64, 64, 22);
        for (i = 0; i < 200000 && sv.active; i++, slow++)
            sayit_frame(&sv);
        CHECK(slow > fast * 2, "speed-changes-length");
    }
    /* PIT must reach the engine: the same word one octave up is a different
     * stream. (The resolved period at +12/-12 is checked above; duration and
     * zero crossings barely move because SAM's formants, not its glottal
     * period, dominate the waveform.) */
    {
        int32_t diff = 0;
        static int32_t a[48000], b[48000];
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 0, "say", 3, 64,
                          sayit_pitch_steps_for_note(SAYIT_BASE, SAYIT_BASE, 63),
                          64, 64, 31);
        for (i = 0; i < 48000 && sv.active; i++)
            a[i] = sayit_frame(&sv);
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 0, "say", 3, 64,
                          sayit_pitch_steps_for_note(SAYIT_BASE, SAYIT_BASE, 89),
                          64, 64, 32);
        for (i = 0; i < 48000 && sv.active; i++)
            b[i] = sayit_frame(&sv);
        for (i = 0; i < 48000; i++)
            if (a[i] != b[i]) diff = 1;
        CHECK(diff, "pit-changes-render");
    }
    /* ---- live controls: a knob turned, or an LFO, during a word ---------- */
    /* sam_voice_live() moves speed and the pitch factor of a phrase that is
     * already speaking (the two SAM reads as it goes); mouth and throat are
     * per breath chunk and must NOT move. The harness reaches it directly, as
     * the render does every block a knob or an LFO moves. The window is 4096
     * frames because the port renders a few hundred samples ahead into its
     * ring, so a change lands a little after the call. */
    {
        static int32_t quiet[4096], up[4096], mouthed[4096];
        struct sam_settings st;
        int32_t k;
#define LIVE_WINDOW 4096

        st.speed = 72;
        st.pitch = 64;
        st.mouth = 128;
        st.throat = 128;
        st.sing = 0;
        st.period_q15 = 32768u;
        /* baseline: 4096 frames in, 4096 more with nothing touched */
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 0, "say", 3, 64, 0, 64, 64, 41);
        for (k = 0; k < LIVE_WINDOW; k++) sayit_frame(&sv);
        for (k = 0; k < LIVE_WINDOW; k++) quiet[k] = sayit_frame(&sv);
        /* the same settings handed to the live setter mid-word: the stream
         * must not move (it also proves the render's registers survive) */
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 0, "say", 3, 64, 0, 64, 64, 42);
        for (k = 0; k < LIVE_WINDOW; k++) sayit_frame(&sv);
        sam_voice_live(&st);
        for (k = 0; k < LIVE_WINDOW; k++) up[k] = sayit_frame(&sv);
        CHECK(memcmp(quiet, up, sizeof(quiet)) == 0,
              "live-noop-keeps-stream");
        /* an octave up from the middle of the word */
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 0, "say", 3, 64, 0, 64, 64, 43);
        for (k = 0; k < LIVE_WINDOW; k++) sayit_frame(&sv);
        st.period_q15 = 16384u;
        sam_voice_live(&st);
        for (k = 0; k < LIVE_WINDOW; k++) up[k] = sayit_frame(&sv);
        CHECK(memcmp(quiet, up, sizeof(quiet)) != 0, "live-pitch-moves");
        /* and speed, the other control SAM reads as it goes */
        st.period_q15 = 32768u;
        st.speed = 100;
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 0, "say", 3, 64, 0, 64, 64, 44);
        for (k = 0; k < LIVE_WINDOW; k++) sayit_frame(&sv);
        sam_voice_live(&st);
        for (k = 0; k < LIVE_WINDOW; k++) mouthed[k] = sayit_frame(&sv);
        CHECK(memcmp(quiet, mouthed, sizeof(quiet)) != 0,
              "live-speed-moves");
        /* mouth and throat are baked into SAM's tables per breath chunk, so
         * the live setter must leave them alone: same settings, same stream */
        st.speed = 72;
        st.mouth = 200;
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 0, "say", 3, 64, 0, 64, 64, 45);
        for (k = 0; k < LIVE_WINDOW; k++) sayit_frame(&sv);
        sam_voice_live(&st);
        for (k = 0; k < LIVE_WINDOW; k++) mouthed[k] = sayit_frame(&sv);
        CHECK(memcmp(quiet, mouthed, sizeof(quiet)) == 0,
              "live-formant-not-sent");
        /* the settings reach the phrase's own engine, not another track's */
        sayit_voice_init(&sv);
        sayit_voice_start(&sv, 2, "say", 3, 64, 0, 64, 64, 46);
        CHECK(sv.track == 2, "live-engine-is-track");
    }

    /* ---- the phrase in a sound (Digitakt) ---- */
    {
        int16_t w[SAYIT_STORE_WORDS];
        char back[SAYIT_STORE_CHARS + 1];
        const char *longp = "   leading and trailing   ";
        int i, ok;

        CHECK(SAYIT_STORE_WORDS * 2 == SAYIT_STORE_CHARS, "store-two-a-word");
        CHECK(SAYIT_STORE_MAX <= 0x7f00, "store-inside-extra-range");
        sayit_store_encode("say it", w);
        sayit_store_decode(w, back, (int32_t)sizeof back);
        CHECK(strcmp(back, "say it") == 0, "store-round-trip");
        CHECK(sayit_store_valid(w), "store-valid");
        ok = 1;
        for (i = 0; i < SAYIT_STORE_WORDS; i++)
            if (w[i] < 1 || w[i] > SAYIT_STORE_MAX) ok = 0;
        CHECK(ok, "store-words-in-range");
        sayit_store_encode("ALIEN ELECTRO!", w);
        sayit_store_decode(w, back, (int32_t)sizeof back);
        CHECK(strcmp(back, "ALIEN ELECTRO!") == 0, "store-punctuation");
        sayit_store_encode("abcdefghijklmnopqrstuvwxyz", w);
        sayit_store_decode(w, back, (int32_t)sizeof back);
        CHECK(strcmp(back, "abcdefghijklmnopqrst") == 0, "store-cap-20");
        sayit_store_encode(longp, w);
        sayit_store_decode(w, back, (int32_t)sizeof back);
        CHECK(strncmp(back, longp, SAYIT_STORE_CHARS) == 0 && back[SAYIT_STORE_CHARS] == 0,
              "store-spaces-kept");
        /* an empty phrase is stored, not left blank: a blank word means the
         * sound has never carried one, which is how an old pattern reads */
        sayit_store_encode("", w);
        CHECK(sayit_store_valid(w), "store-empty-is-valid");
        sayit_store_decode(w, back, (int32_t)sizeof back);
        CHECK(back[0] == 0, "store-empty-reads-empty");
        for (i = 0; i < SAYIT_STORE_WORDS; i++)
            w[i] = 0;
        CHECK(!sayit_store_valid(w), "store-invalid-when-never-written");
        sayit_store_decode(w, back, (int32_t)sizeof back);
        CHECK(back[0] == 0, "store-invalid-reads-empty");
        /* a sound of the build that kept its phrase in RAM: F-H hold the
         * previous machine's values and the extras were never set, so one word
         * is out of range and the sound carries no phrase */
        for (i = 0; i < SAYIT_STORE_WORDS; i++)
            w[i] = i < 3 ? 500 : 0;
        CHECK(!sayit_store_valid(w), "store-invalid-leftovers");
        w[0] = SAYIT_STORE_MAX + 1;
        for (i = 1; i < SAYIT_STORE_WORDS; i++)
            w[i] = 1;
        CHECK(!sayit_store_valid(w), "store-invalid-over-range");
        /* the words are a comparison, so a loaded sound is seen as changed */
        sayit_store_encode("one", w);
        {
            int16_t w2[SAYIT_STORE_WORDS];
            sayit_store_encode("two", w2);
            CHECK(!sayit_store_same(w, w2), "store-differs");
            sayit_store_encode("one", w2);
            CHECK(sayit_store_same(w, w2), "store-same");
            /* every alphabet character survives in every position */
            ok = 1;
            for (i = 0; i < SAYIT_CHARSET_N; i += SAYIT_STORE_CHARS) {
                char phr[SAYIT_STORE_CHARS + 1];
                int32_t j;
                for (j = 0; j < SAYIT_STORE_CHARS; j++)
                    phr[j] = SAYIT_CHARSET[(i + j) % SAYIT_CHARSET_N];
                phr[SAYIT_STORE_CHARS] = 0;
                sayit_store_encode(phr, w2);
                sayit_store_decode(w2, back, (int32_t)sizeof back);
                if (strcmp(back, phr) != 0) ok = 0;
            }
            CHECK(ok, "store-alphabet-round-trip");
        }
    }

    /* ---- factory naming editor ---- */
    {
        struct sayit_editor ed;
        sayit_ed_init(&ed);
        CHECK(ed.len == 0 && ed.cursor == 0 && ed.buf[0] == 0, "ed-init");
        sayit_ed_set(&ed, "say it");
        CHECK(ed.len == 6 && ed.cursor == 6, "ed-set");
        CHECK(sayit_ed_insert(&ed, '!') == 0 && ed.len == 7, "ed-insert");
        sayit_ed_move(&ed, -100);
        CHECK(ed.cursor == 0, "ed-move-clamp-lo");
        sayit_ed_move(&ed, 100);
        CHECK(ed.cursor == ed.len, "ed-move-clamp-hi");
        sayit_ed_move(&ed, -1);
        CHECK(sayit_ed_backspace(&ed) == 0 && ed.buf[ed.len] == 0, "ed-backspace");
        CHECK(sayit_ed_delete_at(&ed) == 0, "ed-delete-at");
        sayit_ed_init(&ed);
        CHECK(sayit_ed_backspace(&ed) == -1, "ed-backspace-empty");
        CHECK(sayit_ed_delete_at(&ed) == -1, "ed-delete-empty");
        CHECK(sayit_ed_insert(&ed, 1) == -1, "ed-insert-invalid");
        /* at end, one forward step appends a char and stays on it, so
         * turning the knob never piles up characters */
        sayit_ed_init(&ed);
        CHECK(sayit_ed_step(&ed, 1) == 0 && ed.buf[0] == 'A' && ed.len == 1 && ed.cursor == 0, "ed-step-append-A");
        CHECK(sayit_ed_step(&ed, 1) == 0 && ed.buf[0] == 'B' && ed.len == 1, "ed-step-append-cycles");
        CHECK(sayit_ed_step(&ed, 3) == 0 && ed.buf[0] == 'E' && ed.len == 1, "ed-step-append-multi");
        CHECK(sayit_ed_step(&ed, -1) == 0 && ed.buf[0] == 'D', "ed-step-back");
        sayit_ed_move(&ed, 1);
        CHECK(ed.cursor == 1, "ed-step-end-cursor");
        CHECK(sayit_ed_step(&ed, -1) == 0 && ed.buf[0] == 'D' && ed.len == 1, "ed-step-end-back");
        sayit_ed_move(&ed, 1);
        CHECK(sayit_ed_space(&ed) == 0 && ed.buf[1] == ' ', "ed-space");
        CHECK(sayit_ed_step(&ed, 2) == 0 && ed.len == 3 && ed.buf[2] == 'B',
              "ed-step-end-once");
        sayit_ed_move(&ed, -1);
        /* full buffer refuses more */
        {
            char full[256];
            memset(full, 'x', 255);
            full[255] = 0;
            sayit_ed_set(&ed, full);
            CHECK(ed.len == 255, "ed-full-len");
            CHECK(sayit_ed_insert(&ed, 'y') == -1, "ed-full-refuse");
            CHECK(sayit_ed_step(&ed, 1) == -1, "ed-full-step-refuse");
        }
        sayit_ed_set(&ed, "a\x01" "b");
        CHECK(ed.len == 3 && ed.buf[1] == ' ', "ed-sanitise");

        /* pop-up grid: 15 x 6, every slot unique */
        CHECK((int32_t)SAYIT_CHARSET_N == SAYIT_GRID_COLS * SAYIT_GRID_ROWS,
              "grid-size");
        {
            int32_t a, b, dup = 0;
            for (a = 0; a < SAYIT_CHARSET_N; a++)
                for (b = a + 1; b < SAYIT_CHARSET_N; b++)
                    if (SAYIT_CHARSET[a] == SAYIT_CHARSET[b])
                        dup = 1;
            CHECK(!dup, "grid-unique");
        }
        CHECK(sayit_ed_grid_char(0, 0) == 'A', "grid-A");
        CHECK(sayit_ed_grid_char(SAYIT_GRID_ROWS - 1, SAYIT_GRID_COLS - 1) == ' ',
              "grid-space");
        sayit_ed_set(&ed, "B");
        sayit_ed_move(&ed, -1);
        sayit_ed_grid_open(&ed);
        CHECK(ed.grid_row == 0 && ed.grid_col == 1, "grid-open-B");
        sayit_ed_grid_move(&ed, -1, 0);
        CHECK(ed.grid_col == 0, "grid-move-left");
        sayit_ed_grid_accept(&ed);
        CHECK(ed.len == 2 && ed.buf[0] == 'A' && ed.buf[1] == 'B', "grid-accept");
        sayit_ed_grid_move(&ed, 0, -1);
        CHECK(ed.grid_row == SAYIT_GRID_ROWS - 1, "grid-wrap-up");
    }

    if (fails == 0)
        printf("ALL C TESTS PASS\n");
    return fails ? 1 : 0;
}
"""

EXE = os.path.join(ROOT, "out", "sayit_host_test")
SRC = os.path.join(ROOT, "out", "sayit_host_test_harness.c")


def main():
    os.makedirs(os.path.join(ROOT, "out"), exist_ok=True)
    with open(SRC, "w") as f:
        f.write(HARNESS)
    cc = os.environ.get("CC", "gcc")
    cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-O2",
           "-I" + ROOT, "-I" + EL_INC, SRC,
           os.path.join(ROOT, "sam", "sam_port.c"),
           os.path.join(ROOT, "sam", "sam_engine_1.c"),
           os.path.join(ROOT, "sam", "sam_engine_2.c"),
           os.path.join(ROOT, "sam", "sam_engine_3.c"),
           os.path.join(ROOT, "sam", "sam_multi.c"),
           os.path.join(ROOT, "sam", "reciter.c"), "-o", EXE, "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("COMPILE FAILED:\n" + r.stderr)
        return 1
    r = subprocess.run([EXE], capture_output=True, text=True)
    print(r.stdout, end="")
    if r.stderr:
        print(r.stderr, end="", file=sys.stderr)
    if r.returncode != 0:
        print("HOST TESTS FAILED")
        return 1

    # Repo hygiene: the port is vendored and wired up, IDs in sync, knob map
    # intact.
    fails = 0
    # The machines live in machines/<device>/; the speech they share
    # (sayit.h, sayit_edit.h, sam/) is at the root.
    DN = os.path.join(ROOT, "machines", "digitone")
    DT = os.path.join(ROOT, "machines", "digitakt")
    with open(os.path.join(DN, "mod.json")) as f:
        mod = f.read()
    with open(os.path.join(DN, "engine.c")) as f:
        eng = f.read()
    with open(os.path.join(ROOT, "sayit.h")) as f:
        core = f.read()
    with open(os.path.join(DN, "overlay.c")) as f:
        ovl = f.read()
    with open(os.path.join(DN, "dn_knobs.h")) as f:
        knobs = f.read()
    if '"dnmachine:77"' not in mod:
        print("FAIL mod.json dnmachine:77 claim"); fails += 1
    if "#define SAYIT_ID 77" not in eng:
        print("FAIL engine SAYIT_ID 77"); fails += 1
    if '"id": "sayit"' not in mod:
        print("FAIL mod id sayit"); fails += 1
    if '"projdata:SAYI"' not in mod:
        print("FAIL mod.json projdata:SAYI claim"); fails += 1
    if '"core": "4.0"' not in mod:
        print("FAIL mod.json core 4.0"); fails += 1
    # SAY IT belongs under MACHINES only; the phrase editor opens from its own
    # page. A core_menu contribution would put a second, top-level tile beside
    # MACHINES in the Mod Menu, which this mod must not do.
    if '"to": "core_menu"' in mod or "sayit_menu" in eng or "sayit_menu" in ovl:
        print("FAIL mod still contributes a Mod Menu entry"); fails += 1
    for kw in ["SPD", "PIT", "THRT", "MOUT", "MODE"]:
        if kw not in eng:
            print("FAIL knob %s missing" % kw); fails += 1
    for kw in ["K_ROOT", "K_VOC", "sayit_presets", "sayit_preset_name"]:
        if kw in eng or kw in ovl or kw in core:
            print("FAIL removed knob/material %s still in the build" % kw)
            fails += 1
    # A held step's locks and the two system LFOs: the page shows the first
    # (VAWT's lock table, read through the pattern pointer), the render adds
    # the second itself, since a machine's voices never take FM's path.
    with open(os.path.join(ROOT, "sam", "sam_api.h")) as f:
        api = f.read()
    for kw in ["LOCK_TAB_OFF", "sayit_held_step", "sayit_shown",
               "PATPTR_AT", "sayit_trigs"]:
        if kw not in ovl:
            print("FAIL overlay has no %s" % kw); fails += 1
    # Holding a trig must show that step's locks in every record mode. Gating
    # it on GRID/STEP does not work: the mode is not visible to a mod, and a
    # chorded RECORD+PLAY leaves such a gate shut, so a held trig shows the
    # kit and nothing looks locked. The overlay yields only to the chords
    # that raise a popup in the page's own view -- FUNC/PTN/PAGE + RECORD,
    # PLAY or STOP, i.e. copy, paste, clear -- never to a bare RECORD or to
    # RECORD+PLAY/STOP, which report in the top bar above the pages.
    for kw in ["sayit_grid", "sayit_live"]:
        if kw in ovl:
            print("FAIL overlay gates the lock display on %s" % kw)
            fails += 1
    if "if (sayit_func || sayit_ptn || sayit_page || sayit_trigs" in ovl:
        print("FAIL overlay yields the frame to more than a copy/paste/clear chord")
        fails += 1
    # OS 1.44 only: one stock file, one pattern pointer, no per-OS ports.
    if '"os": "1.44"' not in mod:
        print("FAIL mod.json does not target OS 1.44"); fails += 1
    if "-DPATPTR_AT=0x4138f214" not in mod:
        print("FAIL mod.json does not name 1.44's pattern pointer"); fails += 1
    if '"ports"' in mod:
        print("FAIL mod.json still carries per-OS ports"); fails += 1
    for kw in ["FW_LFO", "sam_voice_live_tr", "sayit_knob_slot"]:
        if kw not in eng:
            print("FAIL engine has no %s" % kw); fails += 1
    for kw in ["sam_voice_live", "sam_voice_live_tr"]:
        if kw not in api:
            print("FAIL sam_api.h has no %s" % kw); fails += 1
    # The cell a function is drawn in must sit on the encoder that edits it.
    # The Digitone's encoders are two rows of four, A-D above E-H, and the
    # overlay draws every SAYIT cell in the lower band; so every mapped knob
    # must be an E-H knob (index % 8 >= 4), and the pages' lists must be the
    # engine's K_* indices: a cell in the wrong row sits over an encoder that
    # edits another parameter.
    if "sayit_page_knobs[2][4]" not in ovl or \
            "sayit_draw_cell(bmp, SAYIT_CELL_X + i * 28" not in ovl:
        print("FAIL overlay does not lay its cells out as one lower band")
        fails += 1
    idx = {}
    for line in knobs.splitlines():
        p = line.split()
        if len(p) >= 3 and p[0] == "#define" and p[1].startswith("K_"):
            idx[p[1]] = int(p[2])
    want = {"K_PIT": 4, "K_SPD": 5, "K_MODE": 6,
            "K_THRT": 12, "K_MOUT": 13}
    if idx != want:
        print("FAIL engine knob map %r != %r" % (idx, want))
        fails += 1
    if idx and any(v % 8 < 4 for v in idx.values()):
        print("FAIL a SAYIT knob is on the upper encoder row A-D")
        fails += 1
    # The knob map is the machine's, not the shared core's: the Digitakt's
    # five controls are its own SRC-page knobs, 0-4.
    if "K_PIT" in core:
        print("FAIL the shared core still carries a machine's knob map"); fails += 1
    if '"../../sam/sam_port.c"' not in mod or '"../../sam/reciter.c"' not in mod:
        print("FAIL mod.json does not build the shared SAM port"); fails += 1
    for fn in ["sam/sam_port.c", "sam/reciter.c", "sam/sam_api.h",
               "sam/sam_inst.h", "sam/sam_multi.c", "sam/sam_engine_1.c",
               "sam/sam_engine_2.c", "sam/sam_engine_3.c",
               "sam/SamTabs.h", "sam/RenderTabs.h", "sam/ReciterTabs.h",
               "sam/README.md"]:
        if not os.path.exists(os.path.join(ROOT, fn)):
            print("FAIL missing %s" % fn); fails += 1
    # One engine per track: SAY IT on T1 and T2 must not share a voice. Every
    # engine copy has to be in the build, and the dispatcher has to cover the
    # four tracks the kit holds.
    for i in (1, 2, 3):
        if '"../../sam/sam_engine_%d.c"' % i not in mod:
            print("FAIL mod.json does not build engine %d" % i); fails += 1
    if '"../../sam/sam_multi.c"' not in mod:
        print("FAIL mod.json does not build the engine dispatcher"); fails += 1
    if "sam_voice_begin_tr" not in core or "SAM_ENGINES" not in core:
        print("FAIL sayit.h does not speak on a track's engine"); fails += 1
    if "sam/sam_api.h" not in core:
        print("FAIL sayit.h does not use the SAM port"); fails += 1
    if "sample-identical" not in mod:
        print("FAIL mod.json does not state the SAM provenance"); fails += 1

    # ---- the Digitakt machine (machines/digitakt) ------------------------
    # Same speech, a different device: one SAM engine per audio track, the
    # controls on the machine's own SRC page, and no firmware address of its
    # own (core 3.0's exports carry it, so the mod has no sites and no ports).
    with open(os.path.join(DT, "mod.json")) as f:
        dmod = f.read()
    with open(os.path.join(DT, "engine.c")) as f:
        deng = f.read()
    with open(os.path.join(DT, "overlay.c")) as f:
        dovl = f.read()
    if '"device": "digitakt-mk1"' not in dmod:
        print("FAIL digitakt mod.json device"); fails += 1
    if '"os": "1.54"' not in dmod:
        print("FAIL digitakt mod.json does not target OS 1.54"); fails += 1
    if '"id": 77' not in dmod or '"descriptor": "sayit_machine"' not in dmod:
        print("FAIL digitakt mod.json does not add machine 77"); fails += 1
    if '"machine:77"' not in dmod:
        print("FAIL digitakt mod.json does not claim machine:77"); fails += 1
    if "#define SAYIT_ID 77" not in deng or "#define SAYIT_ID 77" not in dovl:
        print("FAIL digitakt SAYIT_ID 77"); fails += 1
    # Eight audio tracks, and an engine for each: SAY IT on T1 and T8 must not
    # share a voice, exactly as on the Digitone's four.
    if '"-DSAYIT_TRACKS=8"' not in dmod or '"-DSAM_ENGINES=8"' not in dmod:
        print("FAIL digitakt does not build eight engines"); fails += 1
    for i in range(1, 8):
        if '"../../sam/sam_engine_%d.c"' % i not in dmod:
            print("FAIL digitakt mod.json does not build engine %d" % i)
            fails += 1
    for kw in ["sayit_voice_start", "fw_voice_params", "fw_render_machine",
               "fw_voice_start", "ev_render_voices", "sayit_render"]:
        if kw not in deng and kw not in dmod:
            print("FAIL digitakt engine has no %s" % kw); fails += 1
    # The page is machine-pages'; the machine only describes it. It names the
    # extended page because the stored phrase needs the seven extra values' room.
    if '"machine-pages"' not in dmod or "cm_ui_v32" not in deng:
        print("FAIL digitakt does not describe its SRC page"); fails += 1
    # The phrase lives in the track's sound, in the words the machine does not
    # use, and the editor's limit is that store's capacity: a phrase a player
    # cannot save, or a cap the store cannot hold, is a bug either way.
    if not os.path.exists(os.path.join(DT, "phrase_store.h")):
        print("FAIL digitakt has no phrase store"); fails += 1
    if '"-DSAYIT_EDIT_LEN=20"' not in dmod:
        print("FAIL digitakt does not cap the phrase at the store's capacity"); fails += 1
    if "SAYIT_STORE_CHARS" not in deng:
        print("FAIL digitakt engine does not build the phrase store"); fails += 1
    if "sayit_store_sync" not in deng or "sayit_store_sync" not in dovl:
        print("FAIL digitakt does not read the phrase back from the sound"); fails += 1
    if "does not survive a power cycle" in dmod or "held in RAM" in dmod:
        print("FAIL digitakt mod.json still says a phrase is lost"); fails += 1
    # No invented firmware addresses: everything comes from core 3.0's
    # exports, so the mod carries no sites, no ports and no defsym.
    for kw in ['"sites"', '"ports"', '"defsym"', '"cflags": [\n    "-DPATPTR_AT']:
        if kw in dmod:
            print("FAIL digitakt names a firmware address (%s)" % kw); fails += 1
    # The editor is modal: it takes input only while open, and it opens only
    # on a track whose sound is SAY IT. A key taken on a stock sound would
    # make the mod bite in someone else's machine.
    if "sayit_active_is_sayit" not in dovl or "FW_SOUND_MACHINE" not in dovl:
        print("FAIL digitakt overlay does not gate on the active track"); fails += 1
    if "if (sayit_open) {" not in dovl:
        print("FAIL digitakt overlay is not modal"); fails += 1
    # The save confirmation is drawn over the stock page, so it is a box: the
    # editor's black ground and outline around the message. Text alone ran
    # through the knobs underneath (the glitch seen in digiemu).
    draw = dovl.split("if (!sayit_open) {", 1)
    toast = draw[1].split("if (sayit_toast) {", 1) if len(draw) > 1 else [""]
    toast = toast[1].split("return;", 1)[0] if len(toast) > 1 else ""
    if "fw_fillrect" not in toast or "fw_framerect" not in toast:
        print("FAIL digitakt save confirmation is not drawn in a box"); fails += 1
    if fails:
        print("HYGIENE FAILED")
        return 1
    print("ALL HOST TESTS PASS (C + hygiene)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
