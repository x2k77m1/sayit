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

/* How far ahead of the reader the port's writer may run, in bytes of SAM's own
 * 22.05 kHz stream. This is what bounds one call's work: the reader asks for a
 * byte and, when it has caught up, has the writer make more, so the writer may
 * never make more than this ahead of it (sam_port.c, "How far ahead is the
 * whole of a block's cost"). It has to cover a block with room to spare, or a
 * block would ask for a byte that is not there and the writer would fill the
 * ring in that one call - which the unit shows as a DSP spike. The machine
 * renders 32 samples a block and takes SAYIT_STEP of a source sample per
 * output sample, so a block reads 15 bytes; the writer releases bytes 10 behind
 * its head. tools/sayit_host_test.py checks that relation. Overridable so a
 * host build can measure the same stream against a writer that fills the
 * ring.
 *
 * Two blocks' worth (32 bytes), not four: a refill makes its lookahead in one
 * call, so the lookahead IS the size of a block's burst of synthesis. At 64 a
 * refill was four blocks' worth of synthesis landing in one block (18 glottal
 * steps, ~14 800 cycles on the cycle model) every fourth block; at 32 the same
 * work arrives as nine steps every second block, which is the peak this is
 * for. Smaller than two blocks would leave no margin between the reader and
 * the writer within a block. */
#ifndef SAM_LOOKAHEAD
#define SAM_LOOKAHEAD 32
#endif

/* Next sample of the 22.05 kHz stream, -1 when the phrase has finished.
 * Bounded work per call: the writer runs at most SAM_LOOKAHEAD bytes ahead of
 * this reader, so a call that has to render makes about a lookahead's worth of
 * samples and no more, whatever the phrase is doing. */
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

/* Make the formant tables a note start would otherwise make: 32 table entries
 * through trans() for one mouth/throat pair, ~12 700 cycles, the last thing a
 * note start still carries in a single audio block. A machine calls this from
 * its UI task with the settings its knobs hold (sam_voice_prime_tr), so the
 * note that follows finds the pair ready and starts without the spike. Safe at
 * any time: the tables depend on those two bytes alone, and a note start that
 * finds them already made does only a compare. */
void sam_voice_prime(const struct sam_settings *s);

/* The raw phoneme string the reciter produced, for tests and diagnostics. */
const char *sam_voice_phonemes(void);

/* Is the chunk's table set ready? A note start writes the chunk's frames in
 * slices (sam_port.c, "a chunk's tables, made in slices") and sam_voice_next()
 * says silence until they are done, so the machine calls sam_voice_setup_tr()
 * once a block while this says no: that is what keeps one block from carrying
 * the whole chunk. A reader that only pulls samples gets the same work done a
 * unit per call and needs neither. */
int sam_voice_ready(void);
void sam_voice_setup(void);

/* ---- the plan: the recitation done ahead of the trigger ------------------ */
/* Starting a token costs SAM's reciter and its parsers, which walk thousands of
 * bytes of tables and buffers. That does not fit in an audio block: the render
 * runs at interrupt level, 1500 blocks a second, a 667 us block, and a
 * machine's code and tables are in DDR, where each of those walks is a bus trip
 * with no cache in front of it. So a machine has the recitation run ahead of the
 * trigger, on the UI task, and a note start only copies the result.
 *
 * sam_plan_build() is the recitation: text -> the packed phoneme stream, three
 * bytes a phoneme (index, length, stress) in the order sam_prepare_chunk walks
 * them, with SAMMain's 254 breath markers in place and no 255 terminator (the
 * note start writes that). Returns the phoneme count, 0 for a text that says
 * nothing, or -1 when the stream does not fit in max bytes.
 *
 * The planning copy of the engine does this work (sam/sam_engine_plan.c), one
 * per build and never a track's engine, so a phrase can be planned while every
 * track is speaking. */
int sam_plan_build(const char *text, int len, unsigned char *out, int max);

/* A note start from such a stream, on a track's own engine: no reciting, only
 * the chunk's frames. Same return as sam_voice_begin(). */
int sam_voice_begin_plan_tr(int engine, const unsigned char *plan, int n,
                            const struct sam_settings *s);

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
/* Engine 0's own stream start from a plan; the copies are reached through
 * sam_voice_begin_plan_tr below. */
int sam_voice_begin_plan(const unsigned char *plan, int n,
                         const struct sam_settings *s);
int sam_voice_next_tr(int engine);
void sam_voice_live_tr(int engine, const struct sam_settings *s);
int sam_voice_ready_tr(int engine);
void sam_voice_setup_tr(int engine);
/* A track engine's formant tables, made ahead of its note (sam_voice_prime). */
void sam_voice_prime_tr(int engine, const struct sam_settings *s);

#endif /* SAM_API_H */
