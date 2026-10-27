/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SAYIT: the SAM port's interface to the machine.
 *
 * The port renders SAM's own 8-bit stream at 22050 Hz, one byte per call.
 * Everything above it (resampling to 48 kHz, the envelope, the phrase
 * bookkeeping) lives in sayit.h; this header is only the SAM engine.
 *
 * Settings are SAM's own: speed 72, pitch 64, mouth 128, throat 128 are the
 * stock values s-macke/SAM starts with (sam -speed 72 -pitch 64).
 */
#ifndef SAM_API_H
#define SAM_API_H

struct sam_settings {
    unsigned char speed;    /* SAM speed 0..255, 72 stock */
    unsigned char pitch;    /* SAM pitch 0..255, 64 stock */
    unsigned char mouth;    /* mouth formant 0..255, 128 stock */
    unsigned char throat;   /* throat formant 0..255, 128 stock */
    unsigned char sing;     /* 1 = no pitch contour (SAM -sing) */
    /* Transposition: every glottal period is multiplied by this Q15 factor
     * (32768 = unison, 16384 = one octave up, 65536 = one octave down);
     * 0 means unity as well, so a zeroed struct is SAM unchanged.
     *
     * This is not the same job as `pitch`. SAM subtracts half of frame F1
     * from each frame's pitch before the pulse length is used, so a given
     * `pitch` value does not land on a fixed interval, and moving `pitch`
     * alone moves the whole contour by a step while the formant subtraction
     * stays put -- a fourth of a turn at 64 is not a fourth of a turn at 100.
     * Scaling the finished periods is exact for every frame. */
    unsigned long period_q15;
};

/* Text is ordinary English (any case), digits already spelled out.
 * Returns 0 when the text produced nothing to say. */
int sam_voice_begin(const char *text, int len, const struct sam_settings *s);

/* Next sample of the 22.05 kHz stream, -1 when the phrase has finished.
 * Bounded work per call: the renderer suspends whenever its ring is full. */
int sam_voice_next(void);

/* Move the controls of a phrase that is already speaking: the machine reads
 * its knobs (and the firmware's LFOs on them) every block, so turning PIT or
 * SPD, or an LFO on either, must reach a word in progress. SAM reads both as
 * it goes -- the glottal period for every pulse, the speed counter for every
 * frame -- so they follow at once.
 *
 * mouth and throat are NOT live: SAM bakes them into its formant tables, and
 * a breath chunk copies the frequencies out of those tables when it starts,
 * so the machine hands them over with each token (each token is a new chunk).
 * pitch and sing shape the phrase before it starts, at sam_voice_begin. */
void sam_voice_live(const struct sam_settings *s);

/* The raw phoneme string the reciter produced, for tests and diagnostics. */
const char *sam_voice_phonemes(void);

/* Say it on a track's own engine. The machine can sit on every track at once,
 * so each track gets an engine (sam/sam_multi.c): 0 is the port above, 1 and
 * up are copies with their own state. A phrase started on one engine keeps
 * sounding while another starts on another, and sam_voice_next_tr() only
 * moves the engine it is asked for.
 *
 * The machine's mod.json sets the count: the Digitone has four tracks and so
 * four engines, the Digitakt eight. Every engine past 0 costs its own copy of
 * the port (see sam/sam_inst.h), so the count is the machine's to choose. */
#ifndef SAM_ENGINES
#define SAM_ENGINES 4
#endif
#if SAM_ENGINES < 1 || SAM_ENGINES > 8
#error "SAM_ENGINES must be 1..8 (sam/sam_engine_1..7.c are the copies)"
#endif
int sam_voice_begin_tr(int engine, const char *text, int len,
                       const struct sam_settings *s);
int sam_voice_next_tr(int engine);
void sam_voice_live_tr(int engine, const struct sam_settings *s);

#endif /* SAM_API_H */
