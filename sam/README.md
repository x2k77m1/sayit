# SAM, ported

This directory is a port of **Software Automatic Mouth (S.A.M.)**, the 1982
Commodore 64 speech synthesiser by Don't Ask Software, in the C translation by
Sebastian Macke:

- https://github.com/s-macke/SAM (the copy this port is built from lives in
  `3rdparty/SAM`)

| file | what it is |
| --- | --- |
| `sam_port.c` | `src/sam.c` + `src/render.c`: phoneme preprocessing and the formant synthesiser |
| `reciter.c` | `src/reciter.c`: English text to phonemes |
| `SamTabs.h`, `RenderTabs.h`, `ReciterTabs.h` | SAM's original tables, unchanged |
| `sam_api.h` | the small interface the machine uses |

`tools/vendor_sam.py` regenerates `sam_port.c` and `reciter.c` from
`3rdparty/SAM` and fails if the upstream text has moved, so every edit is an
explicit, reviewable step rather than a rewrite. Run `scripts/setup.sh` first
(it fetches SAM), then `python3 tools/vendor_sam.py`. The tables are copied in
unchanged; `sam_api.h`, `sam_inst.h`, `sam_multi.c` and `sam_engine_1..7.c` are
this project's own code and are written by hand.

## What was changed

The logic and the tables are SAM's, line for line. What had to change to run
inside a synth engine:

1. **No allocation, no printf.** The 220 KB `malloc` output buffer became a
   256-byte ring that the audio callback drains; the five-sample overlapping
   writes per glottal step are kept exactly, so the sample stream is unchanged.
2. **Resumable.** `Render()` is split into `render_setup()` (frames,
   transitions, contour, for one chunk of phonemes) and `render_pump()` (the
   glottal loop), and `RenderSample()` can pause between bits. A voiced sampled
   consonant is up to 256 × 8 glottal steps; rendering it in one go would stall
   an audio block. The machine renders 32 samples at a time.
3. **Chunked.** `PrepareOutput()` hands over one chunk per 254 breath marker,
   as the original did, with the phoneme cursor kept in its own variable (the
   original saved `X` across each `Render()` call).
4. **Silent first bytes.** The original leaves its first three output bytes
   uninitialised (malloc garbage, a click). This port starts them at 128.
5. **File-static globals.** SAM's names stay, but private to the module.
6. **Transposition.** `settings.period_q15` scales the glottal period by a Q15
   factor: each pulse SAM fetches as `pitches[Y]` (5 samples per unit) becomes
   `pitches[Y] * period_q15 / 32768` in `gl_len`, and the 3/4 mark that lets a
   voiced sampled consonant enter mid-pulse follows it. This is SAYIT's PIT and
   note transposition, and it has to happen here: SAM adds `pitch` *before*
   subtracting half of frame F1, so a `pitch` value is not a fixed interval -
   halving it at 64 moved the voice by a twelfth-and-a-half, not an octave.
   Scaling the pulse is exact for every frame, and keeping the factor on the
   pulse count rather than on the pitch bytes lets two octaves down (four times
   SAM's byte) exist, which the byte could not hold. Unison (32768, or 0)
   returns SAM's byte unchanged, so the port is byte-identical to SAM there.
7. **22050 → 48000 Hz.** Not in this directory: `sayit.h` resamples the 8-bit
   stream with linear interpolation, because the machine renders at 48 kHz.
8. **Live speed and pitch.** `sam_voice_live()` writes `speed` and
   `period_q15` of a phrase that is already speaking, which SAM never needed:
   it rendered a whole buffer at once, while the machine reads its knobs (and
   the firmware's LFOs on them) every block. Both are read by the engine as it
   goes - the speed counter once per frame, the glottal period once per pulse -
   so a vibrato or a speed wobble reaches a word in progress. Mouth and throat
   are not live: a breath chunk copies its formant frequencies from SAM's
   tables when it starts, so the machine hands those over with each token.

Everything else - the reciter's rule tables, the formant tables, the mouth and
throat `trans()` arithmetic, the `timetable` quirks - is untouched.

## One engine per track

The port keeps its state at file scope, because it is a port of a program that
did: `sam_port.c` stays recognisable, and its tables and tables-of-state are
what upstream has. The machine can sit on every track at once, and each note a
track plays must not disturb the others' words, so the port is compiled once
per track instead of rewritten: `sam_engine_1..7.c` each `#define SAM_SUFFIX`
and include `sam_inst.h` (which renames the port's 29 globals) and then
`sam_port.c`. `sam_port.c` compiled on its own is engine 0; `sam_multi.c`
is the only file that knows the copies exist and dispatches
`sam_voice_begin_tr()` / `sam_voice_next_tr()` / `sam_voice_live_tr()`.

How many copies a build takes is the machine's: `SAM_ENGINES` in its mod.json
(`-DSAM_ENGINES=8`, and `-DSAYIT_TRACKS=8`, for the Digitakt's eight audio
tracks; the Digitone's four is the default). `sam_multi.c` references only the
engines a build has sources for, so the Digitone pays for four and the Digitakt
for eight. Cost, measured: the Digitone mod is 76 KB of code and 16 KB of state,
the Digitakt's 118 KB and 29 KB, and the linked builds leave room to spare on
their device. The reciter stays one copy: it runs only at note start, and its
scratch is written through the buffer it is handed.

## The voice

`sayit.h` runs SAM at its own stock values - speed 72, pitch 64, throat 128,
mouth 128 - the settings s-macke/SAM starts with. PIT, SPD, THRT and MOUT are
offsets around them, so there is one voice, and THRT and MOUT shape it without
being able to leave the range SAM's own tables cover. SPD is centred at 64; PIT
is a bipolar semitone transpose centred at 0, two octaves each way, applied as
the `period_q15` multiplier above rather than through SAM's `pitch` control. The
played note adds to PIT: C5 is the voice's own pitch and each key above it is a
semitone higher.

## Proof that it is SAM

`tools/sam_parity.py` builds the upstream program from `3rdparty/SAM`, renders
the same phrases with both, and compares the 8-bit streams sample by sample.
20 cases covering the reciter, the formant paths, voiced sampled consonants,
sing mode and multiple chunks are byte-identical; the only permitted
difference is the three silent bytes above.

## Licence

Neither this port nor the upstream C translation carries an explicit licence:
SAM's vendor no longer exists, and as Sebastian Macke puts it, "The company no
longer exists. Any attempt to contact the original authors failed. Hence S.A.M.
can be best described as Abandonware."

So the licence of this directory is not SAY IT's to give. The port is SAM's
text with the explicit changes listed above, generated from Macke's translation
by `tools/vendor_sam.py`; the code SAY IT adds around it (`sam_api.h`,
`sam_inst.h`, `sam_multi.c`, `sam_engine_1..7.c`, that script) is
GPL-2.0-or-later like the rest of the project. The provenance is stated plainly
and the original authors are credited. If an author or rights holder objects,
open an issue and it will be removed.
