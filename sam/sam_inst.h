/* SPDX-License-Identifier: GPL-2.0-or-later */
/* One more copy of the SAM engine.
 *
 * SAY IT can be on every track at once, and each track must keep its own
 * voice: one engine for the whole machine would let a note on T2 cut T1's
 * word, with both reading the same phrase. The engine's state is file-scope
 * in sam_port.c, which is generated from the upstream source and has to stay
 * recognisable, so rather than rewrite it into a struct, the port is compiled
 * once per track: sam_engine_1.c .. sam_engine_3.c define SAM_SUFFIX and
 * include this header, which renames every symbol the port defines at file
 * scope, then include sam_port.c. Statics need no renaming (they are already
 * per-compilation); these are the 29 globals the port defines.
 *
 * Cost: three extra copies of the port, about 10.5 KB of RAM and 38 KB of
 * .run. The image has ~1.2 MB of RAM free and 1.6 MB of flash, so four
 * engines fit with room to spare. The reciter (sam/reciter.c) stays one
 * copy: it is called only at note start, and it writes through the input
 * buffer it is handed, so its static scratch cannot be shared across a call.
 */
#ifndef SAM_INST_H
#define SAM_INST_H

#ifndef SAM_SUFFIX
#error "define SAM_SUFFIX (e.g. _1) before including sam_inst.h"
#endif

#define SAM_GLUE2(a, b) a##b
#define SAM_GLUE(a, b) SAM_GLUE2(a, b)
#define SAM_NAME(a) SAM_GLUE(a, SAM_SUFFIX)

/* the engine's entry points */
#define sam_voice_begin    SAM_NAME(sam_voice_begin)
#define sam_voice_next     SAM_NAME(sam_voice_next)
#define sam_voice_live     SAM_NAME(sam_voice_live)
#define sam_voice_phonemes SAM_NAME(sam_voice_phonemes)
#define sam_voice_begin_plan SAM_NAME(sam_voice_begin_plan)
#define sam_voice_ready   SAM_NAME(sam_voice_ready)
#define sam_voice_setup   SAM_NAME(sam_voice_setup)
#define sam_voice_prime   SAM_NAME(sam_voice_prime)
/* Only the planning copy defines it (SAM_PLAN_BUILD, sam/sam_engine_plan.c);
 * naming it here costs the track copies nothing. */
#define sam_plan_build     SAM_NAME(sam_plan_build)

/* 6502-style helpers the port leaves global */
#define Code47503 SAM_NAME(Code47503)
#define Read      SAM_NAME(Read)
#define Write     SAM_NAME(Write)

/* the port's tables (tables are duplicated per engine; they are read-only) */
#define ampl1data             SAM_NAME(ampl1data)
#define ampl2data             SAM_NAME(ampl2data)
#define ampl3data             SAM_NAME(ampl3data)
#define amplitudeRescale      SAM_NAME(amplitudeRescale)
#define blendRank             SAM_NAME(blendRank)
#define flags                 SAM_NAME(flags)
#define flags2                SAM_NAME(flags2)
#define freq1data             SAM_NAME(freq1data)
#define freq2data             SAM_NAME(freq2data)
#define freq3data             SAM_NAME(freq3data)
#define inBlendLength         SAM_NAME(inBlendLength)
#define outBlendLength        SAM_NAME(outBlendLength)
#define phonemeLengthTable    SAM_NAME(phonemeLengthTable)
#define phonemeStressedLengthTable SAM_NAME(phonemeStressedLengthTable)
#define rectangle             SAM_NAME(rectangle)
#define sampleTable           SAM_NAME(sampleTable)
#define sampledConsonantFlags SAM_NAME(sampledConsonantFlags)
#define signInputTable1       SAM_NAME(signInputTable1)
#define signInputTable2       SAM_NAME(signInputTable2)
#define sinus                 SAM_NAME(sinus)
#define stressInputTable      SAM_NAME(stressInputTable)
#define tab47492              SAM_NAME(tab47492)
#define tab48426              SAM_NAME(tab48426)

#endif /* SAM_INST_H */
