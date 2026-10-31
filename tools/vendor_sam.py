#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate sam/sam_port.c and sam/reciter.c from 3rdparty/SAM.

The port is a literal copy of s-macke/SAM's engine with a short, explicit list
of changes; this script performs exactly those and fails loudly when the
upstream text does not match, so a future SAM update cannot silently diverge.

    python3 tools/vendor_sam.py
"""
import os

SRC = "3rdparty/SAM/src"


def one(text, a, b, what):
    """Replace exactly one occurrence, or fail."""
    n = text.count(a)
    assert n == 1, "%s: %d sites for %r" % (what, n, a[:60])
    return text.replace(a, b, 1)


def cut(text, a, b, what):
    i = text.index(a)
    j = text.index(b, i)
    assert i < j, what
    return text[:i] + text[j:]


# ---------------------------------------------------------------- reciter ---
rec = open(os.path.join(SRC, "reciter.c")).read()
rec = rec[rec.index("static unsigned char inputtemp[256];"):]
for fn in ("void Code37055(", "void Code37066(", "unsigned char GetRuleByte("):
    rec = one(rec, fn, "static " + fn, "reciter helper " + fn)

REC_HEAD = """/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SAYIT: SAM's reciter (English text -> phoneme string with stress).
 *
 * Generated from s-macke/SAM src/reciter.c by tools/vendor_sam.py. The logic
 * is SAM's 1982 text-to-phoneme rule table, line by line; the only changes are
 * that the printf debug path is compiled out, the 6502 register names A/X/Y
 * and the helpers are file-static, and the tables live in ReciterTabs.h.
 * Provenance and licence: sam/README.md.
 */
#include <stdint.h>

#include "ReciterTabs.h"

int TextToPhonemes(unsigned char *input);

static unsigned char A, X, Y;
#define debug 0
static void PrintRule(unsigned short r) { (void)r; }

"""
open("sam/reciter.c", "w").write(REC_HEAD + rec)

# ---------------------------------------------------------------- engine ----
core = open(os.path.join(SRC, "sam.c")).read()
core = core[core.index('#include "SamTabs.h"') + len('#include "SamTabs.h"'):]
core = core.replace("extern int debug;\n", "")

core = one(core, "char input[256]; //tab39445",
           "static unsigned char input[256]; //tab39445", "input")
core = cut(core, "void SetInput(char *_input)", "\nvoid SetSpeed", "SetInput")
core = cut(core, "void SetSpeed", "\nvoid Init();", "settings setters")
core = cut(core, "void Init();\n",
           "void SetMouthThroat(unsigned char mouth, unsigned char throat);\n",
           "forward declarations")
core = core.replace("""// contains the final soundbuffer
int bufferpos=0;
char *buffer = NULL;
""", "")
core = one(core, """    bufferpos = 0;
    // TODO, check for free the memory, 10 seconds of output should be more than enough
    buffer = malloc(22050*10);
""", "", "Init buffer")

# SAMMain no longer renders: the streaming API drives the chunks.
core = one(core, "    PrepareOutput();\n\n    return 1;", "    return 1;",
           "SAMMain tail")

# PrepareOutput() -> one chunk per call; 254 is InsertBreath's breath marker,
# 255 the end of the phoneme list.
i = core.index("void PrepareOutput()")
j = core.index("//void Code48431()")
assert i < j and "pos48551" in core[i:j], "PrepareOutput boundaries"
core = core[:i] + """// One chunk of the utterance at a time. InsertBreath() leaves a 254 marker in
// the phoneme list; SAM rendered everything up to the next 254 into its buffer
// and then carried on with the rest. Keeping that split is what lets the
// renderer stream: each chunk gets its own frames and its own sample pump.
static unsigned char sam_phpos;   /* cursor in the phoneme list */

static int sam_prepare_chunk(void)
{
    if (sam_chunk_done) return 0;
    Y = 0;
    while(1)
    {
        A = phonemeindex[sam_phpos];
        if (A == 255)
        {
            phonemeIndexOutput[Y] = 255;
            sam_chunk_done = 1;
            return 1;
        }
        if (A == 254)
        {
            sam_phpos++;
            phonemeIndexOutput[Y] = 255;
            return 1;
        }
        if (A == 0)
        {
            sam_phpos++;
            continue;
        }
        phonemeIndexOutput[Y] = A;
        phonemeLengthOutput[Y] = phonemeLength[sam_phpos];
        stressOutput[Y] = stress[sam_phpos];
        sam_phpos++;
        Y++;
    }
}

""" + core[j:]

for ln in ("unsigned char speed = 72;", "unsigned char pitch = 64;",
           "unsigned char mouth = 128;", "unsigned char throat = 128;",
           "int singmode = 0;", "unsigned char mem39;", "unsigned char mem44;",
           "unsigned char mem47;", "unsigned char mem49;", "unsigned char mem50;",
           "unsigned char mem51;", "unsigned char mem53;", "unsigned char mem56;",
           "unsigned char mem59=0;", "unsigned char A, X, Y;",
           "unsigned char stress[256];", "unsigned char phonemeLength[256];",
           "unsigned char phonemeindex[256];",
           "unsigned char phonemeIndexOutput[60];",
           "unsigned char stressOutput[60];",
           "unsigned char phonemeLengthOutput[60];"):
    core = one(core, ln, "static " + ln, "global " + ln.split()[1])

for fn in ("void Init(", "int Parser1(", "void Parser2(", "int SAMMain(",
           "void CopyStress(", "void SetPhonemeLength(", "void AdjustLengths(",
           "void Code41240(", "void Insert(", "void InsertBreath("):
    core = one(core, fn, "static " + fn, "function " + fn)

# A chunk's formant tables are only remade when the mouth or the throat moved:
# they depend on those two bytes and nothing else, and remaking them is 32
# trans() calls on every note start (sam_set_mouth_throat).
core = one(core, """static void Init()
{
    int i;
    SetMouthThroat( mouth, throat);""", """static void sam_set_mouth_throat(unsigned char mouth, unsigned char throat)
{
    /* The formant tables the mouth and throat shape are rewritten from 32
     * table entries through trans() every time a chunk starts, and a phrase
     * played at one THRT/MOUT setting only needs them once: they depend on
     * those two bytes and nothing else. Keeping the last pair turns a note
     * start into a compare when the controls have not moved, which is what a
     * sequencer repeating a word does. */
    static unsigned char last_mouth, last_throat;
    static int have;
    if (have && last_mouth == mouth && last_throat == throat)
        return;
    /* Cleared first: the rebuild can be interrupted by the audio block, and a
     * note start that arrives mid-rebuild must make the tables itself rather
     * than read half of one pair and half of another (sam_voice_prime). */
    have = 0;
    SetMouthThroat(mouth, throat);
    last_mouth = mouth;
    last_throat = throat;
    have = 1;
}

static void Init()
{
    int i;
    sam_set_mouth_throat( mouth, throat);""", "mouth/throat cache")

# The same tables, made on the UI task ahead of the note (sam_voice_prime), so
# the block that starts a note does not carry them.
core = one(core, """    have = 1;
}

static void Init()""", """    have = 1;
}

/* The same tables, made ahead of the note: a machine's UI task calls this with
 * the settings its knobs hold now, so the note that follows finds the pair in
 * place and the block that starts it carries only the chunk's frames. The
 * tables depend on those two bytes alone, so nothing here is wasted when the
 * controls have not moved - it is the compare above and a return. */
void sam_voice_prime(const struct sam_settings *s)
{
    if (s)
        sam_set_mouth_throat(s->mouth, s->throat);
}

static void Init()""", "formant prime")

# ----------------------------------------------------------------- render ---
rend = open(os.path.join(SRC, "render.c")).read()
rend = rend[rend.index("unsigned char wait1 = 7;"):]
rend = rend.replace("""unsigned char wait1 = 7;
unsigned char wait2 = 6;

extern unsigned char A, X, Y;
extern unsigned char mem44;
extern unsigned char mem47;
extern unsigned char mem49;
extern unsigned char mem39;
extern unsigned char mem50;
extern unsigned char mem51;
extern unsigned char mem53;
extern unsigned char mem56;

extern unsigned char speed;
extern unsigned char pitch;
extern int singmode;


extern unsigned char phonemeIndexOutput[60]; //tab47296
extern unsigned char stressOutput[60]; //tab47365
extern unsigned char phonemeLengthOutput[60]; //tab47416
""", "")
rend = rend.replace("static unsigned oldtimetableindex = 0;\n", "")

# SAM's buffer writer becomes the ring writer.
o0 = rend.index("void Output8BitAry")
o1 = rend.index("//written by me because of different table positions.")
assert o0 < o1, "writer boundaries"
rend = rend[:o0] + """/* SAM wrote its 8-bit output into a 220 KB malloc'd buffer indexed by
 * bufferpos/50 at 22.05 kHz: five samples per glottal step, advancing three or
 * four, so the writes overlap on purpose. The ring keeps that exactly and only
 * bounds how far ahead of the reader the renderer may run. sam_valid releases
 * a byte once no later write can still reach it.
 *
 * How far ahead is the whole of a block's cost. The reader asks for a byte and,
 * when it has caught up with the writer, has the writer make more; a writer that
 * runs to the end of the ring makes SAM's own several thousand cycles in that
 * one call, which lands in a single audio block - the unit shows it as a DSP
 * spike, and at a word trigger it clips whatever else is playing. SAM_LOOKAHEAD
 * is what a call may run ahead instead, SAM_LOOKAHEAD (sam_api.h): at 22.05 kHz
 * and 48 kHz out the reader takes about 15 bytes a block, so a lookahead of 32
 * covers two blocks and each refill is eight glottal steps instead of seventy.
 * sam_ring_full() is the one place that says so; the writer suspends there and
 * the caller resumes it (render_pump and RenderSample keep all their state). */
#define SAM_RING 256
static unsigned char sam_ring[SAM_RING];
static uint32_t sam_wr, sam_rd, sam_valid;
static uint32_t sam_bufpos;
static unsigned char oldtimetableindex;

static int sam_ring_full(void)
{
    if ((int)(sam_wr - sam_rd) >= SAM_LOOKAHEAD)
        return 1;
    return sam_ring_space() < 16;   /* never with the lookahead above: belt and braces */
}

static void Output8BitAry(int index, unsigned char ary[5])
{
    int k;
    uint32_t base;
    sam_bufpos += timetable[oldtimetableindex][index];
    oldtimetableindex = index;
    base = sam_bufpos / 50;
    for(k=0; k<5; k++)
        sam_ring[(base + k) & (SAM_RING - 1)] = ary[k];
    if (base + 5 > sam_wr) sam_wr = base + 5;
    if (base > 5 && base - 5 > sam_valid) sam_valid = base - 5;
}

static void Output8Bit(int index, unsigned char A)
{
    unsigned char ary[5] = {A,A,A,A,A};
    Output8BitAry(index, ary);
}


""" + rend[o1:]

# Transposition (see "glottal period" below). Every frame's glottal pulse
# lasts pitches[i] * 5 samples, so scaling the pulse is an exact musical
# interval; scaling `pitch` would not be, because that value is added BEFORE
# half of F1 is subtracted here. The pitch bytes stay exactly as SAM wrote
# them -- at two octaves down the pulse is four times SAM's own 8-bit byte,
# which no byte can hold, so the factor is applied where the pulse is
# fetched and the pulse length becomes a wider type of its own.
assert "pitches[i] -= (frequency1[i] >> 1);" in rend, "pitch contour"

# RenderSample(): resumable. A voiced sampled consonant (V, DH, Z, ZH) is up
# to 256 * 8 glottal steps; running that to completion would stall a block.
rend = one(rend, """// Code48227()
void RenderSample(unsigned char *mem66)
{
    int tempA;
    // current phoneme's index
    mem49 = Y;
""", """// Code48227()
/* Returns 1 when it paused because the ring is full, 0 when the sampled
 * consonant has been rendered out. SAM rendered a voiced sampled consonant in
 * one go, up to 256 * 8 glottal steps, which no audio block can afford; the
 * two bit loops can now be suspended and re-entered. */
static unsigned char rs_resume;   /* 1 = resume at 48280, 2 = at 48327 */
static unsigned char rs_after;    /* which pump call site resumes it */
static unsigned char rs_phase;
static int RenderSample(unsigned char *mem66)
{
    int tempA;
    if (rs_resume == 1) goto pos48280;
    if (rs_resume == 2) goto pos48327;
    // current phoneme's index
    mem49 = Y;
""", "RenderSample head")

rend = one(rend, "pos48280:\n", """pos48280:
    if (sam_ring_full()) { rs_resume = 1; return 1; }
""", "unvoiced pause")

rend = one(rend, """    unsigned char phase1;

pos48315:
// handle voiced samples here

   // number of samples?
    phase1 = A ^ 255;
""", """pos48315:
// handle voiced samples here

   // number of samples?
    rs_phase = A ^ 255;
""", "voiced head")

rend = one(rend, """        // loop 8 times
        //pos48327:
        do
""", """        // loop 8 times
        //pos48327:
pos48327:
        if (sam_ring_full()) { rs_resume = 2; return 1; }
        do
""", "voiced pause")
rend = one(rend, """        // move ahead in the table
        Y++;

        // continue until counter done
        phase1++;
""", """        // move ahead in the table
        Y++;

        // continue until counter done
        rs_phase++;
""", "voiced incr")
rend = rend.replace("    } while (phase1 != 0);\n", "    } while (rs_phase != 0);\n")

for exit_code in ("    mem44 = 1;\n    Y = mem49;\n    return;\n",
                  "    mem44 = 1;\n    *mem66 = Y;\n    Y = mem49;\n    return;\n"):
    assert rend.count(exit_code) == 1, "RenderSample exit"
    rend = rend.replace(exit_code, exit_code.replace(
        "    mem44 = 1;\n", "    mem44 = 1;\n    gl_len = 1;\n").replace(
        "    return;", "    rs_resume = 0;\n    return 0;"))

# Render() -> render_setup() + render_pump()
rend = one(rend, """void Render()
{
    unsigned char phase1 = 0;  //mem43
    unsigned char phase2=0;
    unsigned char phase3=0;
    unsigned char mem66=0;
    unsigned char mem38=0;
    unsigned char mem40=0;
    unsigned char speedcounter=0; //mem45
    unsigned char mem48=0;
    int i;
    if (phonemeIndexOutput[0] == 255) return; //exit if no data
""", """/* Frames, transitions, pitch contour and amplitude rescale for one chunk.
 * The pulse loop that used to be the tail of Render() is render_pump(); its
 * locals live here as file state so an audio block can suspend the pump in
 * the middle of a glottal pulse and resume it on the next block. */
static unsigned char phase1;   //mem43
static unsigned char phase2;
static unsigned char phase3;
static unsigned char mem66;
static unsigned char mem38;
static unsigned char mem40;
static unsigned char speedcounter; //mem45
static unsigned char mem48;

static void render_setup(void)
{
    int i;
    phase1 = phase2 = phase3 = 0;
    mem66 = mem38 = mem40 = speedcounter = mem48 = 0;
    if (phonemeIndexOutput[0] == 255) return; //exit if no data
""", "Render head")

# The glottal period, with PIT's factor on it (the pitches[] bytes stay as
# SAM wrote them). Two fetch sites -- the first pulse and each following one
# -- move from the byte mem44 to gl_len, and the 3/4 mark that lets a voiced
# sampled consonant enter mid-pulse moves with it. At unison sayit_glottal()
# returns the byte unchanged, so SAM's output is untouched.
rend = one(rend, """    Y = 0;
    A = pitches[0];
    mem44 = A;
    X = A;
    mem38 = A - (A>>2);     // 3/4*A ???
""", """    Y = 0;
    A = pitches[0];
    mem44 = A;
    X = A;
    gl_len = sayit_glottal(A);
    gl_third = gl_len - (gl_len >> 2);   // 3/4*A ???
""", "glottal setup")

rend = one(rend, """        // decrement the remaining length of the glottal pulse
        mem44--;

        // finished with a glottal pulse?
        if(mem44 == 0)
        {
pos48159:
            // fetch the next glottal pulse length
            A = pitches[Y];
            mem44 = A;
            A = A - (A>>2);
            mem38 = A;
""", """        // decrement the remaining length of the glottal pulse
        gl_len--;

        // finished with a glottal pulse?
        if(gl_len == 0)
        {
pos48159:
            // fetch the next glottal pulse length
            A = pitches[Y];
            gl_len = sayit_glottal(A);
            gl_third = gl_len - (gl_len >> 2);
""", "glottal fetch")

rend = one(rend, """        // decrement the count
        mem38--;

        // is the count non-zero and the sampled flag is zero?
        if((mem38 != 0) || (mem39 == 0))
""", """        // decrement the count
        gl_third--;

        // is the count non-zero and the sampled flag is zero?
        if((gl_third != 0) || (mem39 == 0))
""", "glottal third")

rend = one(rend, """    //finally the loop for sound output
    //pos48078:
    while(1)
    {
""", """}

// The glottal-pulse loop. Returns 1 when the writer is far enough ahead (call
// again later; every bit of state is intact) and 0 when the chunk has been
// rendered out.
static int render_pump(void)
{
    for(;;)
    {
        if (sam_ring_full()) return 1;
        if (mem48 == 0) return 0;      /* render_setup found nothing to say */
        if (rs_resume) {
            if (RenderSample(&mem66)) return 1;
            if (rs_after == 1) {
                Y += 2;
                mem48 -= 2;
                goto pos48152;
            }
            goto pos48159;
        }
""", "render split")

rend = one(rend, """            // render the sample for the phoneme
            RenderSample(&mem66);
""", """            // render the sample for the phoneme
            rs_after = 1;
            if (RenderSample(&mem66)) return 1;
            rs_after = 0;
""", "pump sample call")

rend = one(rend, """        // voiced sampled phonemes interleave the sample with the
        // glottal pulse. The sample flag is non-zero, so render
        // the sample for the phoneme.
        RenderSample(&mem66);
        goto pos48159;
""", """        // voiced sampled phonemes interleave the sample with the
        // glottal pulse. The sample flag is non-zero, so render
        // the sample for the phoneme.
        rs_after = 2;
        if (RenderSample(&mem66)) return 1;
        rs_after = 0;
        goto pos48159;
""", "pump interleave call")

rend = one(rend, """        // if the frame count is zero, exit the loop
        if(mem48 == 0)  return;
""", """pos48152:
        // if the frame count is zero, exit the loop
        if(mem48 == 0)  return 0;
""", "pump end")

k = rend.index("    } //while\n\n\n    // The following code is never reached.")
rend = rend[:k] + "    }\n}\n\n" + rend[rend.index(
    "// Create a rising or falling inflection"):]

for fn in ("void AddInflection(", "void SetMouthThroat(",
           "unsigned char trans("):
    assert fn in rend, "render helper " + fn
    rend = rend.replace(fn, "static " + fn)
for ln in ("unsigned char pitches[256];", "unsigned char frequency1[256];",
           "unsigned char frequency2[256];", "unsigned char frequency3[256];",
           "unsigned char amplitude1[256];", "unsigned char amplitude2[256];",
           "unsigned char amplitude3[256];",
           "unsigned char sampledConsonantFlag[256];"):
    rend = one(rend, ln, "static " + ln, "render global " + ln)
rend = one(rend, "int timetable[5][5] =", "static int timetable[5][5] =",
           "timetable")

HEAD = """/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SAYIT: SAM's synthesis engine, ported.
 *
 * Generated from s-macke/SAM (src/sam.c, src/render.c, SamTabs.h,
 * RenderTabs.h) by tools/vendor_sam.py; the tables are the original SAM data.
 * The code stays deliberately literal -- same globals, same 6502-shaped
 * control flow, same arithmetic -- so its output can be compared byte for byte
 * against the original program; tools/sam_parity.py does exactly that.
 *
 * The changes, all of them:
 *   - printf debug output (always off) is compiled out.
 *   - The 220 KB malloc'd output buffer is a 256-byte ring drained by the
 *     audio callback; the five-sample overlapping writes are unchanged.
 *   - Render() is split into render_setup() (one chunk) and render_pump()
 *     (resumable glottal loop), and RenderSample() can suspend between bits.
 *   - PrepareOutput() hands over one chunk per 254 breath marker.
 *   - sam_voice_live() moves the speed and the pitch factor of a phrase that
 *     is already speaking, which SAM never needed (it rendered a whole buffer
 *     at once): the machine's knobs and LFOs are read every block.
 *   - A note start is split at the phoneme stream: sam_text_begin() (reciter and
 *     parsers, the half that costs the bus) and sam_render_begin() (the chunk
 *     and its frames). A machine can have the first half run ahead of the
 *     trigger on the UI task (sam_plan_build / sam_voice_begin_plan) instead of
 *     inside an audio block, where it does not fit.
 *   - The writer stops SAM_LOOKAHEAD bytes ahead of the reader, not at the end
 *     of the ring, so one call makes an eighth of the samples it used to and
 *     no audio block carries a whole ring of synthesis (see SAM_LOOKAHEAD).
 *   - A chunk's tables (its frames, their blend tables, the pitch contour and
 *     the amplitude rescale) are made a slice at a time (render_setup starts a
 *     chunk; render_setup_slice advances it), so the block that starts a note
 *     does not carry the whole chunk.
 *   - The first three output bytes, left uninitialised by the original, are
 *     silence instead of a click.
 *   - SAM's globals are file-static, so the port can share a module.
 * Provenance and licence: sam/README.md.
 */
#include <stdint.h>

#include "sam_api.h"

#define debug 0
static void sam_dbg(const char *fmt, ...) { (void)fmt; }
static void sam_dbg_phonemes(const unsigned char *a, const unsigned char *b,
                             const unsigned char *c)
{ (void)a; (void)b; (void)c; }
#define printf(...) sam_dbg(__VA_ARGS__)
#define PrintPhonemes(a, b, c) sam_dbg_phonemes(a, b, c)
#define PrintOutput(a, b, c, d, e, f, g, h) ((void)(a), (void)(b), (void)(c), \\
    (void)(d), (void)(e), (void)(f), (void)(g), (void)(h))
#define abs(v) ((v) < 0 ? -(v) : (v))

#include "SamTabs.h"
#include "RenderTabs.h"

int TextToPhonemes(unsigned char *input);

/* Glottal-period multiplier (Q15, 32768 = unison); sam_api.h. */
static unsigned int period_q15 = 32768;

/* SAYIT: the glottal period in SAM's 5-sample steps, with that factor on it.
 * 32768 is unison and returns SAM's own byte untouched, so the port stays
 * byte-identical to SAM there. Scaling the pulse count rather than the pitch
 * byte is what makes the interval exact for every frame whatever F1
 * subtracted, and it is the only way two octaves down (four times SAM's
 * byte) can exist: the pitch bytes themselves stay exactly as SAM wrote
 * them. Never 0: SAM's pulse counter is a countdown that would spin. */
static unsigned int gl_len;    /* remaining length of this glottal pulse */
static unsigned int gl_third;  /* its 3/4 mark: where a sampled consonant enters */

static unsigned int sayit_glottal(unsigned char p)
{
    unsigned int v;
    if (period_q15 == 32768u)
        return p;
    v = ((unsigned int)p * period_q15 + 16384u) >> 15;
    if (v < 1u) v = 1u;
    if (v > 4096u) v = 4096u;
    return v;
}

static int RenderSample(unsigned char *mem66);
static void SetMouthThroat(unsigned char mouth, unsigned char throat);
static unsigned char trans(unsigned char a, unsigned char b);
static void AddInflection(unsigned char mem48, unsigned char phase1);
static void render_setup(void);
static int render_pump(void);
static int sam_ring_space(void);
static int sam_prepare_chunk(void);
static int sam_chunk_done;
static unsigned char sam_phpos;
static void Init(void);
static int Parser1(void);
static void Parser2(void);
static int SAMMain(void);
static void CopyStress(void);
static void SetPhonemeLength(void);
static void AdjustLengths(void);
static void Code41240(void);
static void Insert(unsigned char position, unsigned char mem60,
                   unsigned char mem59, unsigned char mem58);
static void InsertBreath(void);

"""

API = """

/* ---- the streaming interface (sam_api.h) --------------------------------- */

static int sam_running;

static void sam_ring_reset(void)
{
    int i;
    sam_rd = 0;
    sam_wr = 3;           /* the reference leaves bytes 0..2 uninitialised */
    sam_valid = 3;
    sam_bufpos = 0;
    oldtimetableindex = 0;
    rs_resume = rs_after = rs_phase = 0;
    for (i = 0; i < 3; i++)
        sam_ring[i] = 128;    /* silence instead of malloc garbage: no click */
}

static int sam_ring_space(void)
{
    return (int)(SAM_RING - (sam_wr - sam_rd));
}

const char *sam_voice_phonemes(void) { return (const char *)input; }

/* The front half of a note start: the text into the phoneme stream SAMMain
 * leaves in phonemeindex/phonemeLength/stress. This is the half that costs the
 * DSP - SAM's reciter and its parsers walk thousands of bytes of tables and
 * buffers - so the UI task can run it ahead of the trigger (sam_plan_build) and
 * a note start only copy the result back (sam_voice_begin_plan).
 *
 * `s` is the voice's settings, or 0 to leave them as they are: none of them
 * reaches this half. Speed and the pitch factor are read by the render as it
 * goes, and mouth and throat make the formant tables, which SetMouthThroat does
 * (in Init, inside SAMMain). */
static int sam_text_begin(const char *text, int len, const struct sam_settings *s)
{
    int i;
    if (!text || len <= 0)
        return 0;
    if (len > 200)
        len = 200;
    for (i = 0; i < 255; i++)
        input[i] = 0;
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch >= 'a' && ch <= 'z')
            ch = (unsigned char)(ch - 32);
        input[i] = ch;
    }
    input[len] = '[';
    if (!TextToPhonemes(input))
        return 0;
    if (s) {
        speed = s->speed;
        pitch = s->pitch;
        mouth = s->mouth;
        throat = s->throat;
        singmode = s->sing ? 1 : 0;
        period_q15 = s->period_q15 ? (unsigned int)s->period_q15 : 32768u;
    }
    if (!SAMMain())
        return 0;
    return 1;
}

/* The render half: the stream is in place, so only the chunk and its frames
 * have to be made. Both note starts below end here. */
static int sam_render_begin(void)
{
    X = 0;
    Y = 0;
    sam_phpos = 0;
    sam_chunk_done = 0;
    sam_ring_reset();
    if (!sam_prepare_chunk())
        return 0;
    sam_running = 1;
    render_setup();
    return phonemeIndexOutput[0] != 255;   /* spaces say nothing */
}

int sam_voice_begin(const char *text, int len, const struct sam_settings *s)
{
    if (!s || !sam_text_begin(text, len, s))
        return 0;
    return sam_render_begin();
}

/* A note start from a stream sam_plan_build packed: the UI task's work, copied
 * back. n phonemes, three bytes each (index, length, stress), in the order
 * sam_prepare_chunk walks them, with SAMMain's 254 breath markers in place and
 * no 255 terminator - this writes it. Everything else is what a note start does
 * anyway, including mouth and throat, which make the formant tables here
 * because the plan does not carry them. */
int sam_voice_begin_plan(const unsigned char *plan, int n,
                         const struct sam_settings *s)
{
    int i;
    if (!s || !plan || n < 0 || n > 254)
        return 0;
    for (i = 0; i < n; i++) {
        phonemeindex[i] = plan[3 * i + 0];
        phonemeLength[i] = plan[3 * i + 1];
        stress[i] = plan[3 * i + 2];
    }
    phonemeindex[n] = 255;
    phonemeindex[255] = 255;         /* the guard SAMMain leaves */
    speed = s->speed;
    pitch = s->pitch;
    mouth = s->mouth;
    throat = s->throat;
    singmode = s->sing ? 1 : 0;
    period_q15 = s->period_q15 ? (unsigned int)s->period_q15 : 32768u;
    /* Init()'s other half: the formant frequencies the mouth and throat set.
     * Its zeroing of the arrays is the parsers' own business and is not needed
     * here - the stream below is written whole, and every reader of it stops at
     * the terminator. A note that repeats the last chunk's THRT/MOUT skips the
     * rewrite (sam_set_mouth_throat). */
    sam_set_mouth_throat(mouth, throat);
    return sam_render_begin();
}

#ifdef SAM_PLAN_BUILD
/* One token's phoneme stream, packed for a machine to keep: the half of a note
 * start that costs the DSP, run on the UI task (sam_api.h). Three bytes a
 * phoneme into `out`; returns how many, 0 for a text that says nothing, or -1
 * when it does not fit in `max` bytes - the caller then leaves that token to the
 * render, which recites it as it did before. */
int sam_plan_build(const char *text, int len, unsigned char *out, int max)
{
    int i, n = 0;
    if (!out || max < 3)
        return -1;
    if (!sam_text_begin(text, len, 0))
        return 0;
    for (i = 0; i < 255; i++) {
        if (phonemeindex[i] == 255)
            break;
        if (3 * n + 3 > max)
            return -1;
        out[3 * n + 0] = phonemeindex[i];
        out[3 * n + 1] = phonemeLength[i];
        out[3 * n + 2] = stress[i];
        n++;
    }
    return n;
}
#endif /* SAM_PLAN_BUILD */

int sam_voice_next(void)
{
    for (;;) {
        if (sam_rd < sam_valid)
            return sam_ring[sam_rd++ & (SAM_RING - 1)];
        if (!sam_running)
            return -1;
        if (sam_setup) {
            /* The chunk's tables are still being made. The machine makes them
             * a slice a block (sam_voice_setup_tr), so this call is one of the
             * samples that waits for them: silence, and no time spent here.
             * A plain reader makes them now, in this call, as before. */
            if (sam_sliced)
                return 128;
            render_setup_slice(1 << 20);
            continue;
        }
        if (sam_hold)
            /* The block that finished the tables holds the writer back: its
             * first refill waits for the next block, so no block carries the
             * slice and a refill together (sam_voice_setup). */
            return 128;
        if (render_pump())
            continue;
        if (sam_prepare_chunk()) {
            render_setup();
            continue;
        }
        sam_running = 0;
        sam_valid = sam_bufpos / 50;   /* release the overlapping lookahead */
    }
}

void sam_voice_live(const struct sam_settings *s)
{
    if (!s || !sam_running)
        return;
    /* Only the two SAM reads as it goes: the glottal period (period_q15,
     * fetched for every pulse) and the speed counter (`speed`, read for
     * every frame). THRT and MOUT are not here: SAM bakes them into its
     * formant tables at SetMouthThroat, and the frequencies a breath chunk
     * uses are copied out of those tables when the chunk is prepared, so a
     * new mouth cannot reach a chunk already being rendered. The machine
     * hands them over with each token instead, which starts a new chunk. */
    speed = s->speed;
    period_q15 = s->period_q15 ? (unsigned int)s->period_q15 : 32768u;
}
"""


# ---- a chunk's tables, made in slices --------------------------------------
# Render()'s setup half is split into steps (sam_port.c, "a chunk's tables,
# made in slices"): the frame list a phoneme at a time, the blends a table at a
# time, the contour and the rescale in slices, then the pulse's start state. The
# blend-table body is lifted from the code being transformed, not retyped.
SETUP = r"""/* ---- a chunk's tables, made in slices ------------------------------------ */
/* SAM made a chunk's whole frame list (up to 600 frames), blended every
 * phoneme boundary across seven tables, and then walked the frame slots twice,
 * all before the first sample and all inside the audio block of the note
 * start. On the cycle model that is ~12 400 cycles - six refills' worth - in
 * one block: the peak the unit shows as a CPU spike and another track's voice
 * hears as a click. It is the same work, resumed. render_setup() starts a
 * chunk; render_setup_slice() advances it by a few units; the machine calls
 * that once a block (sam_voice_setup_tr), so a block carries a slice of the
 * chunk instead of the chunk. Nothing reads the frame tables until the pulse
 * loop starts, and that only happens once the phase below is the last, so
 * sam_voice_next() says silence meanwhile: a word can start a millisecond or
 * two late, but no block carries the chunk.
 *
 * A caller that keeps asking for samples and never calls the block hook - the
 * host tests, the parity harness, anything that just reads the stream - still
 * gets there: sam_voice_next() advances a unit per call, so those runs make
 * exactly the samples they made before. */

#define SAM_SETUP_UNITS 6

static int sam_setup;        /* 1 while a chunk's tables are being made */
static int sam_setup_phase;  /* 0 frames, 1 blends, 2 contour, 3 rescale, 4 pulse */
static int sam_setup_pos;    /* the contour's and the rescale's position */
static int sam_frames;       /* frame slots this chunk wrote */
static int sam_blend_open;   /* a boundary's seven tables are being walked */
/* 1 in the block whose slice finished a chunk's tables: the ring's first
 * refill then waits for the next block, so a block carries the slice or the
 * refill, never both. sam_voice_setup() sets it and clears it again, which is
 * why a machine calls that hook every block and not only while the tables are
 * being made. */
static int sam_hold;
/* Who advances the setup. A machine calls sam_voice_setup_tr() once a block,
 * so the samples it asks for meanwhile are silence (SAM_SETUP_SLICED, its
 * mod.json). Everything else - the host tests, the parity harness, any plain
 * reader - gets the chunk made inside the call that first needs a sample, as
 * before, so those streams are byte for byte what they always were. */
#ifdef SAM_SETUP_SLICED
static int sam_sliced = 1;
#else
static int sam_sliced;
#endif

/* Begin a chunk: everything the tables are built from is in place (the chunk's
 * phonemes are in phonemeIndexOutput), and nothing is made yet. */
static void render_setup(void)
{
    phase1 = phase2 = phase3 = 0;
    mem66 = mem38 = mem40 = speedcounter = mem48 = 0;
    sam_setup = 0;
    sam_frames = 0;
    sam_setup_pos = 0;
    sam_blend_open = 0;
    sam_hold = 0;
    if (phonemeIndexOutput[0] == 255)
        return;                          /* nothing to say */
    A = 0;
    X = 0;
    mem44 = 0;
    sam_setup_phase = 0;
    sam_setup = 1;
}

/* CREATE FRAMES: one phoneme of the chunk per unit. A phoneme is a handful of
 * frames, each seven table reads and seven writes. */
static int render_frames_step(void)
{
    Y = mem44;
    A = phonemeIndexOutput[mem44];
    mem56 = A;
    if (A == 255) {                      /* end of the chunk */
        sam_frames = X;
        return 0;
    }
    /* period phoneme *.: rising inflection */
    if (A == 1) {
        mem48 = 1;
        AddInflection(mem48, phase1);
    }
    /* question mark phoneme?: falling inflection */
    if (A == 2) {
        mem48 = 255;
        AddInflection(mem48, phase1);
    }
    /* get the stress amount (more stress = higher pitch) */
    phase1 = tab47492[stressOutput[Y] + 1];
    /* get number of frames to write */
    phase2 = phonemeLengthOutput[Y];
    Y = mem56;
    /* copy from the source to the frames list */
    do {
        frequency1[X] = freq1data[Y];     /* F1 frequency */
        frequency2[X] = freq2data[Y];     /* F2 frequency */
        frequency3[X] = freq3data[Y];     /* F3 frequency */
        amplitude1[X] = ampl1data[Y];     /* F1 amplitude */
        amplitude2[X] = ampl2data[Y];     /* F2 amplitude */
        amplitude3[X] = ampl3data[Y];     /* F3 amplitude */
        sampledConsonantFlag[X] = sampledConsonantFlags[Y];  /* sampled consonants */
        pitches[X] = pitch + phase1;      /* pitch */
        X++;
        phase2--;
    } while (phase2 != 0);
    mem44++;
    return 1;
}

/* CREATE TRANSITIONS: one table of one phoneme boundary per unit. The boundary
 * itself (which blend lengths apply, where the interpolation starts) is opened
 * on the unit that starts it. */
static int render_blend_step(void)
{
    if (!sam_blend_open) {
        /* get the current and following phoneme */
        Y = phonemeIndexOutput[X];
        A = phonemeIndexOutput[X + 1];
        X++;

        /* exit loop at end token */
        if (A == 255)
            return 0;

        /* get the ranking of each phoneme */
        X = A;
        mem56 = blendRank[A];
        A = blendRank[Y];

        /* compare the rank - lower rank value is stronger */
        if (A == mem56) {
            /* same rank, so use out blend lengths from each phoneme */
            phase1 = outBlendLength[Y];
            phase2 = outBlendLength[X];
        } else if (A < mem56) {
            /* first phoneme is stronger, so use its blend lengths */
            phase1 = inBlendLength[X];
            phase2 = outBlendLength[X];
        } else {
            /* second phoneme is stronger, so use its blend lengths;
             * note the out/in are swapped */
            phase1 = outBlendLength[Y];
            phase2 = inBlendLength[Y];
        }

        Y = mem44;
        A = mem49 + phonemeLengthOutput[mem44];   /* mem49 + length */
        mem49 = A;                       /* mem49 now holds length + position */
        A = A + phase2;
        speedcounter = A;
        phase3 = mem49 - phase1;
        A = phase1 + phase2;             /* total transition */
        mem38 = A;

        X = A;
        X -= 2;
        if ((X & 128) != 0) {            /* nothing to interpolate */
            mem44++;
            X = mem44;
            return 1;
        }
        mem47 = 168;
        sam_blend_open = 1;
    }

    /* one table pass; mem47 names the table (Read/Write):
     * 168 pitches, 169-171 frequency1..3, 172-174 amplitude1..3 */
    {
@BODY@
    }
    if (mem47 == 175) {
        sam_blend_open = 0;
        mem44++;
        X = mem44;
    }
    return 1;
}

/* ASSIGN PITCH CONTOUR, in slices: subtract half the F1 frequency from the
 * pitch, so a word has a contour instead of one flat pitch. */
static void render_contour_slice(void)
{
    int i = sam_setup_pos, n = 0;
    while (i < sam_frames && n < 32) {
        pitches[i] -= (frequency1[i] >> 1);
        i++;
        n++;
    }
    sam_setup_pos = i;
}

/* RESCALE AMPLITUDE, in slices: linear amplitude to decibels, from the last
 * frame to the first, as SAM did. */
static void render_rescale_slice(void)
{
    int i = sam_setup_pos, n = 0;
    while (i >= 0 && n < 16) {
        amplitude1[i] = amplitudeRescale[amplitude1[i]];
        amplitude2[i] = amplitudeRescale[amplitude2[i]];
        amplitude3[i] = amplitudeRescale[amplitude3[i]];
        i--;
        n++;
    }
    sam_setup_pos = i;
}

/* The pulse's start state, for the chunk whose tables are ready. */
static void render_pulse_begin(void)
{
    Y = 0;
    A = pitches[0];
    mem44 = A;
    X = A;
    gl_len = sayit_glottal(A);
    gl_third = gl_len - (gl_len >> 2);   /* 3/4*A ??? */

if (debug)
{
    PrintOutput(sampledConsonantFlag, frequency1, frequency2, frequency3, amplitude1, amplitude2, amplitude3, pitches);
}

// PROCESS THE FRAMES
//
// In traditional vocal synthesis, the glottal pulse drives filters, which
// are attenuated to the frequencies of the formants.
//
// SAM generates these formants directly with sin and rectangular waves.
// To simulate them being driven by the glottal pulse, the waveforms are
// reset at the beginning of each glottal pulse.

    sam_setup = 0;
}

/* Advance the chunk's tables by `units` (a phoneme, one table of one blend, or
 * a handful of frames of the contour or the rescale). Returns 1 while the
 * tables are still being made. */
static int render_setup_slice(int units)
{
    while (units-- > 0 && sam_setup) {
        switch (sam_setup_phase) {
        case 0:                          /* frame list */
            if (!render_frames_step()) {
                X = 0;
                mem44 = 0;
                mem49 = 0;
                A = 0;
                sam_blend_open = 0;
                sam_setup_phase = 1;
            }
            break;
        case 1:                          /* phoneme boundaries */
            if (!render_blend_step()) {
                /* add the length of this phoneme */
                mem48 = mem49 + phonemeLengthOutput[mem44];
                phase1 = 0;
                phase2 = 0;
                phase3 = 0;
                mem49 = 0;
                speedcounter = 72;       /* SAM's standard speed */
                sam_setup_phase = 2;
                sam_setup_pos = 0;
            }
            break;
        case 2:                          /* pitch contour */
            if (singmode)
                sam_setup_pos = sam_frames;
            render_contour_slice();
            if (sam_setup_pos >= sam_frames) {
                sam_setup_phase = 3;
                sam_setup_pos = sam_frames - 1;
            }
            break;
        case 3:                          /* amplitude rescale */
            render_rescale_slice();
            if (sam_setup_pos < 0)
                sam_setup_phase = 4;
            break;
        default:                         /* the pulse can start */
            render_pulse_begin();
            break;
        }
    }
    return sam_setup;
}

/* For the machine: 1 when the chunk's tables are ready. */
int sam_voice_ready(void) { return !sam_setup; }
/* The machine's per-block hook, called every block and not only while the
 * tables are being made: one slice of them while they are not ready, and in the
 * block whose slice finishes them, sam_hold, so the ring's first refill lands in
 * the next block instead. A block then carries ~1 000-2 600 cycles of slice or
 * one refill, not both - the two together were the note start's worst block. */
void sam_voice_setup(void)
{
    sam_hold = 0;
    if (!sam_setup)
        return;
    if (!render_setup_slice(SAM_SETUP_UNITS))
        sam_hold = 1;
}"""


def slice_setup(text):
    """Replace Render()'s one-shot setup with the sliced one."""
    start = text.index("static void render_setup(void)\n{\n")
    end = text.index("// The glottal-pulse loop.")
    region = text[start:end].split("\n")
    do_i = next(i for i, ln in enumerate(region) if "do   //while No. 2" in ln)
    assert region[do_i + 1].strip() == "{", region[do_i + 1]
    close_i = next(i for i, ln in enumerate(region)
                   if "} while (mem47 != 175);" in ln)
    body = [(ln[4:] if ln.startswith("    ") else ln)
            for ln in region[do_i + 2:close_i]]
    return text[:start] + SETUP.replace("@BODY@", "\n".join(body)) + "\n\n" + text[end:]


rend = slice_setup(rend)

open("sam/sam_port.c", "w").write(HEAD + core + "\n" + rend + API)
print("wrote sam/sam_port.c")
