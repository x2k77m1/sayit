/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The SAM engines, picked by track.
 *
 * sam_port.c is the whole engine and keeps its state at file scope, so engine
 * 0 is the plain port and engines 1-7 are copies with renamed globals (see
 * sam/sam_inst.h). This file is the only place that knows the copies exist:
 * sayit.h asks for one engine per track, and every track's phrase, pulse
 * stream and render position stay its own. That is what lets a note on T2
 * speak while T1 is still talking instead of cutting it off.
 *
 * SAM_ENGINES is the machine's (its mod.json sets it): 4 for the Digitone's
 * four tracks, 8 for the Digitakt's eight. Only the engines up to that count
 * have a source file in the build, so only those are referenced here - and a
 * machine with four compiles this file exactly as it was before the Digitakt
 * existed, which keeps its build byte for byte the same.
 */
#include "sam_api.h"

#if SAM_ENGINES > 1
/* The renamed copies (sam/sam_engine_1.c .. sam_engine_7.c). */
int sam_voice_begin_1(const char *text, int len, const struct sam_settings *s);
int sam_voice_next_1(void);
void sam_voice_live_1(const struct sam_settings *s);
#endif
#if SAM_ENGINES > 2
int sam_voice_begin_2(const char *text, int len, const struct sam_settings *s);
int sam_voice_next_2(void);
void sam_voice_live_2(const struct sam_settings *s);
#endif
#if SAM_ENGINES > 3
int sam_voice_begin_3(const char *text, int len, const struct sam_settings *s);
int sam_voice_next_3(void);
void sam_voice_live_3(const struct sam_settings *s);
#endif
#if SAM_ENGINES > 4
int sam_voice_begin_4(const char *text, int len, const struct sam_settings *s);
int sam_voice_next_4(void);
void sam_voice_live_4(const struct sam_settings *s);
#endif
#if SAM_ENGINES > 5
int sam_voice_begin_5(const char *text, int len, const struct sam_settings *s);
int sam_voice_next_5(void);
void sam_voice_live_5(const struct sam_settings *s);
#endif
#if SAM_ENGINES > 6
int sam_voice_begin_6(const char *text, int len, const struct sam_settings *s);
int sam_voice_next_6(void);
void sam_voice_live_6(const struct sam_settings *s);
#endif
#if SAM_ENGINES > 7
int sam_voice_begin_7(const char *text, int len, const struct sam_settings *s);
int sam_voice_next_7(void);
void sam_voice_live_7(const struct sam_settings *s);
#endif

int sam_voice_begin_tr(int engine, const char *text, int len,
                       const struct sam_settings *s)
{
    switch (engine) {
#if SAM_ENGINES > 1
    case 1:  return sam_voice_begin_1(text, len, s);
#endif
#if SAM_ENGINES > 2
    case 2:  return sam_voice_begin_2(text, len, s);
#endif
#if SAM_ENGINES > 3
    case 3:  return sam_voice_begin_3(text, len, s);
#endif
#if SAM_ENGINES > 4
    case 4:  return sam_voice_begin_4(text, len, s);
    case 5:  return sam_voice_begin_5(text, len, s);
    case 6:  return sam_voice_begin_6(text, len, s);
    case 7:  return sam_voice_begin_7(text, len, s);
#endif
    default: return sam_voice_begin(text, len, s);   /* engine 0 = the port */
    }
}

int sam_voice_next_tr(int engine)
{
    switch (engine) {
#if SAM_ENGINES > 1
    case 1:  return sam_voice_next_1();
#endif
#if SAM_ENGINES > 2
    case 2:  return sam_voice_next_2();
#endif
#if SAM_ENGINES > 3
    case 3:  return sam_voice_next_3();
#endif
#if SAM_ENGINES > 4
    case 4:  return sam_voice_next_4();
    case 5:  return sam_voice_next_5();
    case 6:  return sam_voice_next_6();
    case 7:  return sam_voice_next_7();
#endif
    default: return sam_voice_next();
    }
}

void sam_voice_live_tr(int engine, const struct sam_settings *s)
{
    switch (engine) {
#if SAM_ENGINES > 1
    case 1:  sam_voice_live_1(s); break;
#endif
#if SAM_ENGINES > 2
    case 2:  sam_voice_live_2(s); break;
#endif
#if SAM_ENGINES > 3
    case 3:  sam_voice_live_3(s); break;
#endif
#if SAM_ENGINES > 4
    case 4:  sam_voice_live_4(s); break;
    case 5:  sam_voice_live_5(s); break;
    case 6:  sam_voice_live_6(s); break;
    case 7:  sam_voice_live_7(s); break;
#endif
    default: sam_voice_live(s);   break;   /* engine 0 = the port */
    }
}
