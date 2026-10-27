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
 * a byte once no later write can still reach it. */
#define SAM_RING 256
static unsigned char sam_ring[SAM_RING];
static uint32_t sam_wr, sam_rd, sam_valid;
static uint32_t sam_bufpos;
static unsigned char oldtimetableindex;

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
    if (sam_ring_space() < 16) { rs_resume = 1; return 1; }
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
        if (sam_ring_space() < 16) { rs_resume = 2; return 1; }
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

// The glottal-pulse loop. Returns 1 when the ring is full (call again later;
// every bit of state is intact) and 0 when the chunk has been rendered out.
static int render_pump(void)
{
    for(;;)
    {
        if (sam_ring_space() < 16) return 1;
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

int sam_voice_begin(const char *text, int len, const struct sam_settings *s)
{
    int i;
    if (!text || !s || len <= 0)
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
    speed = s->speed;
    pitch = s->pitch;
    mouth = s->mouth;
    throat = s->throat;
    singmode = s->sing ? 1 : 0;
    period_q15 = s->period_q15 ? (unsigned int)s->period_q15 : 32768u;
    if (!SAMMain())
        return 0;
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

int sam_voice_next(void)
{
    for (;;) {
        if (sam_rd < sam_valid)
            return sam_ring[sam_rd++ & (SAM_RING - 1)];
        if (!sam_running)
            return -1;
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

open("sam/sam_port.c", "w").write(HEAD + core + "\n" + rend + API)
print("wrote sam/sam_port.c")
