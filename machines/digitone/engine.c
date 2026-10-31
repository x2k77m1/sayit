/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SAYIT - a SAM speech machine for the Digitone mk1.
 *
 * The voice is SAM itself: sam/sam_port.c and sam/reciter.c are the
 * s-macke/SAM engine and its reciter, ported with their original tables and
 * proven byte-identical to that program by tools/sam_parity.py. Every word
 * you type goes through SAM's own text-to-phoneme rules and formant
 * synthesiser.
 *
 * Typed phrases become triggerable tokens: WORD mode splits on whitespace,
 * CHAR mode plays one non-whitespace character per token. The phrase starts
 * at C5 and each semitone above it is the next token, so C (the C key at
 * the Keyboard's default octave +/-0, whose keys run G4-G5) begins the
 * phrase and the speech climbs with the key. Notes below C5, or past the
 * end of the phrase, play nothing. Note-off never truncates: tokens play to
 * completion under the stock AMP envelope. Newest note wins with a short
 * fade.
 *
 * The knobs are the stock FM parameters' own: their sound slots, ranges and
 * p-locks are FM's, which is what makes a p-lock and an LFO reach the voice.
 * p-locks arrive in dnm_knob(); the two system LFOs SAY IT adds itself, from
 * fw_lfo_state (sayit_knob), because an LFO is applied to FM's working copy
 * and a machine's voices never take FM's path. Speed and the glottal period
 * are read by the engine as it goes, so a turned knob or an LFO moves a word
 * already speaking (sayit_live_controls); THRT, MOUT and MODE are SAM's from
 * the token's start and change with the next one.
 *
 * Knob map (borrowed stock params, no param:182-184 minted), all on the
 * lower encoder row E-F so the page has one band of wide cells:
 *   SYN1 E (knob 4, HARM 37..89) PIT  pitch offset, +/-24 semitones
 *   SYN1 F (knob 5, DTUN 0-127)  SPD  speed, centred on SAM's own
 *   SYN1 G (knob 6, FDBK 0-127)  MODE WORD (lower half) / CHAR (upper)
 *   SYN2 E (knob 12, 0-127)      THRT throat offset
 *   SYN2 F (knob 13, 0-127)      MOUT mouth offset
 * SYN1 A-D and H and SYN2 G and H stay unused. No DNM_PAGES2: normal 16-knob
 * layout. Each function is on the knob of the cell that shows it, in the
 * encoder row the stock page puts it in, so the encoder under a cell edits
 * that cell.
 * The page header shows the last note played (N<note>), so a user can see
 * which note a key sent and why it spoke the token it did.
 *
 * Keep SAYIT_ID equal to mod.json's "dnmachine:<id>" claim.
 */
#include "digitone-mk1/machines.h"
#include "../../sayit.h"
#include "dn_knobs.h"

/* overlay.c reads the kit through these; the knob formatters need the same
 * view of the voice. */
int32_t sayit_ui_knob(int32_t k);
int32_t sayit_knob_slot(int32_t k);

#define SAYIT_ID 77             /* must match mod.json: "dnmachine:77" */

/* The K_* knob indices (sayit.h) are the whole layout contract: a function
 * must sit on the knob of the cell that shows it, in the encoder row the
 * stock page places that knob in (A-D upper, E-H lower). A cell drawn in the
 * wrong band sits over an encoder that edits another parameter, so every
 * control here is on the lower row, under E-H. SYN1 B-D are ratio indices and
 * must never be borrowed as 0..127 controls. */

/* Everything that belongs to an instance of the machine is per track: the
 * phrase, the SAM engine behind it, and which voice owns that engine. SAY IT
 * on T1 and SAY IT on T2 are two instruments: one shared phrase and one shared
 * engine would make T2 speak T1's words and cut T1 off mid-word. The project
 * data holds four phrases (overlay.c). */
static char sayit_phrase[SAYIT_TRACKS][SAYIT_PHRASE_MAX] = {
    "say it", "say it", "say it", "say it"
};

static struct sayit_voice sayit_v[8];
static int32_t sayit_ready = 0;    /* the voices were initialised */
static int32_t sayit_owner[SAYIT_TRACKS] = { -1, -1, -1, -1 }; /* per track */
static int32_t sayit_gen[SAYIT_TRACKS];   /* newest-note generation, per track */
static int32_t sayit_last_tok[SAYIT_TRACKS] = { -1, -1, -1, -1 };
static int32_t sayit_last_n[SAYIT_TRACKS];
static int32_t sayit_last_note[SAYIT_TRACKS] = { -1, -1, -1, -1 };
/* The plan: each track's phrase recited ahead of the trigger, a token a tick,
 * so that the audio callback never has to recite (sayit.h, "the plan"). The
 * storage is the machine's because sayit.h is included by more than one file
 * of a machine. */
static struct sayit_plan sayit_pl[SAYIT_TRACKS];
static int32_t sayit_plan_slot;
static int32_t sayit_prime_slot;   /* which engine's formant tables a tick makes */
/* The token a track's render is silent for, or -1: a note that found no stream
 * in its plan. The next tick builds that token first (sayit_plan_tick), so the
 * wait is a tick rather than the rest of the walk. */
static int32_t sayit_want[SAYIT_TRACKS] = { -1, -1, -1, -1 };

static int32_t sayit_track(int32_t t)
{
    return sayit_clamp32(t, 0, SAYIT_TRACKS - 1);
}

static void put_int(char *buf, int32_t v)
{
    char t[8];
    int32_t n = 0;
    if (v < 0) {
        *buf++ = '-';
        v = -v;
    }
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 7);
    while (n)
        *buf++ = t[--n];
    *buf = 0;
}

static void num_text(int32_t value, char *buf) { put_int(buf, value >> 8); }

static void mode_text(int32_t value, char *buf)
{
    const char *s = sayit_map_mode(value) == SAYIT_WORD ? "WORD" : "CHAR";
    while (*s)
        *buf++ = *s++;
    *buf = 0;
}

/* A signed integer with a leading '+' above zero, so a transpose reads as
 * a direction rather than a bare number. */
static void put_signed(int32_t v, char *buf)
{
    if (v > 0)
        *buf++ = '+';
    put_int(buf, v);
}

/* PIT and SPD are offsets, so their displays show the offset the player set
 * (PIT in semitones, SPD around SAM's own speed). THRT and MOUT show SAM's
 * own 0..255 formant value. */
static void resolved_text(int32_t k, int32_t value, char *buf)
{
    struct sam_settings st;
    int32_t v = value >> 8;

    sayit_resolve(k == K_SPD ? v : sayit_ui_knob(K_SPD) >> 8,
                  k == K_PIT ? sayit_map_pitch(v)
                             : sayit_map_pitch(sayit_ui_knob(K_PIT) >> 8),
                  k == K_THRT ? v : sayit_ui_knob(K_THRT) >> 8,
                  k == K_MOUT ? v : sayit_ui_knob(K_MOUT) >> 8, &st);
    switch (k) {
    case K_SPD:   put_signed(v - 64, buf); break;
    case K_PIT:   put_signed(sayit_map_pitch(v), buf); break;
    case K_THRT:  put_int(buf, st.throat); break;
    default:      put_int(buf, st.mouth);  break;
    }
}

/* Shared formatter for the overlay's own page and the descriptor knobs. */
int32_t sayit_fmt_knob(int32_t k, int32_t value, char *buf)
{
    if (!buf)
        return -1;
    buf[0] = 0;
    switch (k) {
    case K_MODE:  mode_text(value, buf);     return 0;
    case K_SPD:
    case K_PIT:
    case K_THRT:
    case K_MOUT:  resolved_text(k, value, buf); return 0;
    default:      num_text(value, buf);      return 0;
    }
}

/* struct dnm_knob's formatter is handed the value but not the knob, so each
 * resolved control gets a thunk that names itself. */
static void spd_text(int32_t value, char *buf)  { resolved_text(K_SPD, value, buf); }
static void pit_text(int32_t value, char *buf)  { resolved_text(K_PIT, value, buf); }
static void thrt_text(int32_t value, char *buf) { resolved_text(K_THRT, value, buf); }
static void mout_text(int32_t value, char *buf) { resolved_text(K_MOUT, value, buf); }

static const struct dnm_knob sayit_knobs[DNM_KNOBS] = {
    [K_PIT]   = { "PIT",   pit_text, 0 },
    [K_SPD]   = { "SPD",   spd_text, 0 },
    [K_MODE]  = { "MODE",  mode_text, 0 },
    [K_THRT]  = { "THRT",  thrt_text, 0 },
    [K_MOUT]  = { "MOUT",  mout_text, 0 },
};

/* Phrase access for the overlay, project data and host tests, one phrase per
 * track. An out-of-range track reads and writes track 0 so a stray call can
 * never corrupt another track's phrase. */
void sayit_set_phrase(int32_t track, const char *p)
{
    int32_t t = sayit_track(track);
    char *dst = sayit_phrase[t];
    int32_t i = 0;
    /* The plan was built from the old phrase: drop it before the new one is in
     * place, so no note can start from a stream that belongs to what was there
     * before. The next tick builds it again. */
    sayit_plan_drop(&sayit_pl[t]);
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

const char *sayit_get_phrase(int32_t track) { return sayit_phrase[sayit_track(track)]; }
int32_t sayit_get_last_token(int32_t track) { return sayit_last_tok[sayit_track(track)]; }
int32_t sayit_get_last_count(int32_t track) { return sayit_last_n[sayit_track(track)]; }
int32_t sayit_get_last_note(int32_t track) { return sayit_last_note[sayit_track(track)]; }

/* ---- UI side: read the active track's SYN knob values (UI task) ---------- */
/* The SYN pages' parameter ids, A-H of SYN1 then A-H of SYN2 (the machines
 * companion's table). A slot's value in the kit is the same 8.8 value the
 * render sees through dnm_knob(). */
static const uint8_t sayit_knob_id[DNM_KNOBS] = {
    107, 108, 109, 110, 111, 112, 113, 114,
    119, 120, 121, 122, 123, 124, 125, 126
};

/* The sound slot behind a SAY IT knob, -1 when the knob is not one of the
 * machine's. Every control keeps the stock parameter's own slot, which is
 * why a p-lock (read by the overlay) and an LFO (added in the render) can
 * address it as FM's: the slot is in the parameter's record, +4. */
int32_t sayit_knob_slot(int32_t k)
{
    const uint8_t *rec;
    int32_t slot;
    if (k < 0 || k >= DNM_KNOBS)
        return -1;
    rec = fw_params + 60 * (int32_t)sayit_knob_id[k];
    slot = *(const int32_t *)(rec + 4);
    return (slot < 0 || slot > 78) ? -1 : slot;
}

/* A track's SYN knob value, for a UI reader that is not looking at the active
 * track: the plan is built for the mode every track will play. */
int32_t sayit_ui_knob_track(int32_t t, int32_t k)
{
    uint8_t *kit = fw_kit;
    int32_t slot;
    if (!kit || t < 0 || t >= SAYIT_TRACKS)
        return 0;
    slot = sayit_knob_slot(k);
    if (slot < 0)
        return 0;
    return FW_SOUND_SLOT(kit, t, slot);
}

int32_t sayit_ui_knob(int32_t k)
{
    return sayit_ui_knob_track((int32_t)fw_active_track, k);
}

/* ---- the plan, one token a tick ----------------------------------------- */
/* The recitation the audio callback must not do (sayit.h, "the plan"): the UI
 * task builds one token of one track's plan per tick, round-robin over the
 * tracks, so a phrase change or a project load plans every track without any
 * single tick carrying more than one token's worth of work. */
void sayit_plan_tick(void)
{
    int32_t t, w;

    /* The formant tables, made here rather than in the note's first block:
     * THRT and MOUT rewrite 32 table entries through trans() (~12 700
     * cycles), which is what a note start still carries on its own. The UI
     * task has the time, and the note that follows finds the pair ready
     * (sam_voice_prime_tr), so its block holds only the chunk's frames. One
     * track a tick, so a load spells all four engines over four ticks rather
     * than putting four table builds in one. A pair a p-lock or an LFO moves
     * between the tick and the note is still made at the note start. */
    {
        struct sam_settings ps;
        int32_t p = sayit_track(sayit_prime_slot);
        sayit_prime_slot = (p + 1) % SAYIT_TRACKS;
        sayit_resolve(sayit_map_speed(sayit_ui_knob_track(p, K_SPD) >> 8), 0,
                      sayit_map_voice(sayit_ui_knob_track(p, K_THRT) >> 8),
                      sayit_map_voice(sayit_ui_knob_track(p, K_MOUT) >> 8),
                      &ps);
        sam_voice_prime_tr(p, &ps);
    }

    /* A note the render is silent for jumps the walk: build that one token
     * now, this tick, so the wait is a tick and not a turn of the round
     * robin (sayit.h, sayit_plan_want_token). */
    for (t = 0; t < SAYIT_TRACKS; t++) {
        w = sayit_want[t];
        if (w < 0)
            continue;
        sayit_want[t] = -1;
        sayit_plan_want_token(&sayit_pl[t], sayit_phrase[t],
                              sayit_map_mode(sayit_ui_knob_track(t, K_MODE)), w);
        return;
    }
    t = sayit_track(sayit_plan_slot);
    sayit_plan_slot = (t + 1) % SAYIT_TRACKS;
    sayit_plan_step(&sayit_pl[t], sayit_phrase[t],
                    sayit_map_mode(sayit_ui_knob_track(t, K_MODE)));
}

/* ---- the system LFOs ---------------------------------------------------- */
/* A knob's value as the voice hears it: dnm_knob() (base and p-locks) plus
 * the two system LFOs, each when its DEST is this knob's slot, clamped to the
 * knob's stock range -- what VAWT does for its slots, and what FM's own voices
 * get from the firmware. With no LFO assigned the value is returned
 * untouched, so nothing about the voice changes. */
static int32_t sayit_lfo_add(int32_t v, int32_t slot, int32_t dest, int32_t mod,
                             int32_t mn, int32_t mx)
{
    if (dest != slot)
        return v;
    v += mod;
    if (v < mn)
        return mn;
    if (v > mx)
        return mx;
    return v;
}

static int32_t sayit_knob(int32_t voice, int32_t k)
{
    int32_t v = dnm_knob(voice, k);
    int32_t slot = sayit_knob_slot(k);
    int32_t d0, m0, d1, m1, mn = 0, mx = 0x7fffffff;
    if (slot < 0)
        return v;
    d0 = (int32_t)FW_LFO(voice, 0)[FW_LFO_DEST];
    d1 = (int32_t)FW_LFO(voice, 1)[FW_LFO_DEST];
    if (d0 != slot && d1 != slot)
        return v;
    dnm_knob_range(k, &mn, &mx);
    m0 = (int32_t)FW_LFO(voice, 0)[FW_LFO_MOD];
    m1 = (int32_t)FW_LFO(voice, 1)[FW_LFO_MOD];
    v = sayit_lfo_add(v, slot, d0, m0, mn, mx);
    v = sayit_lfo_add(v, slot, d1, m1, mn, mx);
    return v;
}

/* ---- knobs that move while a word speaks -------------------------------- */
/* PIT and SPD reach a word in progress: SAM reads the glottal period and the
 * speed counter as it goes (sam_api.h). So a turned knob -- or an LFO on it,
 * which is the same value here -- bends a phrase that is already speaking,
 * which is what the LFO pages expect of an FM voice.
 *
 * THRT and MOUT are not sent: SAM bakes them into its formant tables and a
 * breath chunk copies the frequencies when it starts, so they arrive with the
 * next token. MODE is per token by nature. Nothing is sent while the two live
 * values match what the engine has, so a voice with no LFO and no turning
 * never reaches the port: the render is exactly what it was. */
static void sayit_live_controls(int32_t voice, struct sayit_voice *s)
{
    struct sam_settings st;
    int32_t spd, pit;

    if (!s->active)
        return;
    /* Resolved the same way a note start resolves them, so the two cannot
     * drift: the knobs as they are now, with the LFOs on top. */
    spd = sayit_map_speed(sayit_knob(voice, K_SPD) >> 8);
    pit = sayit_pitch_steps_for_note(s->note, SAYIT_BASE,
                                     sayit_knob(voice, K_PIT) >> 8);
    sayit_resolve(spd, pit, sayit_map_voice(sayit_knob(voice, K_THRT) >> 8),
                  sayit_map_voice(sayit_knob(voice, K_MOUT) >> 8), &st);
    if (st.speed == s->set.speed && st.period_q15 == s->set.period_q15)
        return;
    sam_voice_live_tr(s->track, &st);
    s->set.speed = st.speed;
    s->set.period_q15 = st.period_q15;
}

/* ---- the render (interrupt level, 1500 blocks/s) ------------------------ */
static void sayit_silence(int32_t *block)
{
    int32_t i;
    for (i = 0; i < 32; i++)
        block[i] = 0;
}

static void sayit_render(int32_t voice, int32_t *block, const struct dnm_voice *v)
{
    int32_t tr, note, mode, count, idx, i;
    uint16_t offs[SAYIT_TOKENS_MAX];
    uint8_t lens[SAYIT_TOKENS_MAX];
    char expanded[SAYIT_EXPAND_MAX];
    const char *phrase;
    int32_t spd, pit, thr, mou;
    struct sayit_start start;
    struct sayit_voice *s;

    if (voice < 0 || voice > 7 || !block || !v)
        return;
    if (!sayit_ready) {
        for (i = 0; i < 8; i++)
            sayit_voice_init(&sayit_v[i]);
        sayit_ready = 1;
    }
    s = &sayit_v[voice];
    /* The track decides the phrase and the engine; each track is its own
     * instrument and its own monophonic voice. */
    tr = sayit_track(v->track);
    phrase = sayit_phrase[tr];

    if (v->fresh) {
        mode = sayit_map_mode(sayit_knob(voice, K_MODE));
        note = (v->pitch >> 16) & 127;
        sayit_last_note[tr] = note;
        /* The token the note names, and the stream for it: the UI task's plan
         * when it has one, which is the note start with no reciting in this
         * callback at all (sayit.h, "the plan"). No reciting means that a
         * token the plan has not reached yet leaves the voice waiting, not
         * stalled: see the want below. */
        if (!sayit_start_resolve(&start, &sayit_pl[tr], phrase, mode, note,
                                 offs, lens, expanded, (int32_t)sizeof(expanded))) {
            /* An empty phrase, or a note outside C5..C5+count-1, is
             * silent; a fresh silent note must not steal this track's
             * owner (another track keeps sounding regardless). */
            s->active = 0;
            if (sayit_owner[tr] == voice)
                sayit_owner[tr] = -1;
            sayit_silence(block);
            return;
        }
        count = start.count;
        idx = start.idx;
        /* newest note wins on this track only: the other tracks'
         * voices and engines are untouched, so T1 keeps talking
         * while T2 starts. */
        s->track = tr;                  /* so a newer note can clear this one */
        for (i = 0; i < 8; i++)
            if (i != voice && sayit_v[i].track == tr) {
                sayit_v[i].active = 0;
                sayit_v[i].want_idx = -1;
            }
        spd = sayit_map_speed(sayit_knob(voice, K_SPD) >> 8);
        /* C5 is both token 0 and the untransposed speech note, so
         * token sequencing and the pitch climb together. */
        pit = sayit_pitch_steps_for_note(
            note, SAYIT_BASE, sayit_knob(voice, K_PIT) >> 8);
        thr = sayit_map_voice(sayit_knob(voice, K_THRT) >> 8);
        mou = sayit_map_voice(sayit_knob(voice, K_MOUT) >> 8);
        sayit_gen[tr]++;
        sayit_owner[tr] = voice;
        sayit_last_tok[tr] = idx;
        sayit_last_n[tr] = count;
        s->note = note;
        if (start.plan) {
            s->want_idx = -1;
            sayit_voice_start_plan(s, tr, start.plan, start.nph, spd, pit, thr,
                                   mou, sayit_gen[tr]);
            if (!s->active)
                sayit_owner[tr] = -1;   /* nothing to say: free the track */
        } else {
            /* The plan has no stream for this token yet. Reciting it here is
             * the block the unit cannot hold, so the voice is silent and the
             * UI task is asked for that token instead; the word starts a tick
             * later (SAYIT_WANT_BLOCKS bounds the wait). */
            s->active = 0;
            s->want_idx = idx;
            s->want_note = note;
            s->want_wait = 0;
            sayit_want[tr] = idx;
        }
    }

    /* Mono per track: only this track's newest note's voice sounds. */
    if (sayit_owner[tr] != voice) {
        sayit_silence(block);
        return;
    }

    /* A note waiting for its stream: start it the moment the plan has one. */
    if (!s->active && s->want_idx >= 0) {
        const uint8_t *plan;
        uint8_t nph;
        mode = sayit_map_mode(sayit_knob(voice, K_MODE));
        if (sayit_plan_stream(&sayit_pl[tr], mode, s->want_idx, &plan, &nph)) {
            spd = sayit_map_speed(sayit_knob(voice, K_SPD) >> 8);
            pit = sayit_pitch_steps_for_note(
                s->want_note, SAYIT_BASE, sayit_knob(voice, K_PIT) >> 8);
            thr = sayit_map_voice(sayit_knob(voice, K_THRT) >> 8);
            mou = sayit_map_voice(sayit_knob(voice, K_MOUT) >> 8);
            s->want_idx = -1;
            sayit_voice_start_plan(s, tr, plan, nph, spd, pit, thr, mou,
                                   sayit_gen[tr]);
            if (!s->active) {
                sayit_owner[tr] = -1;
                sayit_silence(block);
                return;
            }
        } else if (++s->want_wait > SAYIT_WANT_BLOCKS) {
            s->want_idx = -1;           /* nothing to say: give the track back */
            sayit_owner[tr] = -1;
            sayit_silence(block);
            return;
        } else {
            sayit_silence(block);
            return;
        }
    }
    if (!s->active) {
        sayit_silence(block);
        return;
    }

    /* A token's tables (its frames, their blend tables, the contour) are made
     * a slice a block, not all in the block that starts the note: the engine
     * says silence until they are ready, so the word starts a millisecond or
     * two late instead of stalling this block (sam_port.c, "a chunk's tables,
     * made in slices"). Called every block, not only while they are being
     * made: it is also the tick that lets the ring's writer back in, which is
     * what keeps a slice and a refill out of the same block. */
    sam_voice_setup_tr(tr);

    sayit_live_controls(voice, s);

    for (i = 0; i < 32; i++) {
        int32_t q15 = sayit_frame(s);
        /* SAM's 8-bit stream arrives as Q15 and peaks near full scale; the
         * machine's FM voices sit lower, so trim it to about -4 dBFS.
         * Q15 -> Q1.31 with the shift, clamped first. */
        q15 = (q15 * 5) >> 3;
        if (q15 > 32767) q15 = 32767;
        if (q15 < -32768) q15 = -32768;
        block[i] = q15 << 12;
        if (!s->active) {
            int32_t j;
            for (j = i + 1; j < 32; j++)
                block[j] = 0;
            if (sayit_owner[tr] == voice)
                sayit_owner[tr] = -1;
            break;
        }
    }
}

/* Defaults on machine pick (8.8; -1 leaves the knob): WORD mode, PIT at
 * unison (HARM's own centre, 63), SPD at SAM's own speed, THRT/MOUT at
 * their centres, so a fresh SAYIT is SAM's stock voice with no detune. */
static const int16_t sayit_defaults[DNM_KNOBS] = {
    [0] = -1, [1] = -1, [2] = -1, [3] = -1,
    [K_PIT] = 63 << 8, [K_SPD] = 64 << 8, [K_MODE] = 0,
    [8] = -1, [9] = -1, [10] = -1, [11] = -1,
    [K_THRT] = 64 << 8, [K_MOUT] = 64 << 8, [14] = -1, [15] = -1,
};

/* A mouth: upper and lower lips with the line between them, from the icon
 * drawn for the machine (16 x 16, bit 15 the left pixel). */
static const uint16_t sayit_icon[16] = {
    0x0000, 0x0000, 0x0000, 0x0000,
    0x1e78, 0x2184, 0x4002, 0xbffd,
    0x4812, 0x27e4, 0x1008, 0x0c30,
    0x03c0, 0x0000, 0x0000, 0x0000
};

const struct dnm_machine sayit_machine = {
    SAYIT_ID, "SAY IT", sayit_icon, sayit_knobs, sayit_render, 0, sayit_defaults,
};
