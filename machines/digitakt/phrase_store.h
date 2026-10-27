/* SPDX-License-Identifier: GPL-2.0-or-later */
/* How a SAY IT phrase is carried in a Digitakt sound, so that it survives the
 * unit being switched off.
 *
 * A machine on this device has one place the unit saves: its own sound. A
 * sound is 53 parameter slots (docs/ADAPTING.md, "Sound slots on the Digitakt
 * mk1"), the stock parameters own 1-45, machine-pages 1.2 owns 46-52 as the
 * seven "extra values" a machine can declare, and slot 0 is kept for a
 * machine's id. SAY IT owns slots 22-24 as blank parameters F-H and the seven
 * extras: ten words, of which machine-pages' version-4 record keeps 16 bits
 * each for a machine's own slots and 15 for an extra value, 153 bits.
 *
 * Two characters a word is what fits without any of that going wrong: a word
 * holds c0 * 90 + c1 + 1, so 1..8100, comfortably inside the extras' 0..0x7F00
 * (a larger value would be replaced by the value's default when the sound is
 * saved, which would silently eat the phrase). A sound whose words are not all
 * in 1..8100 carries no phrase: a fresh one, and one saved by a build that kept
 * its phrases in RAM, in which the extras and F-H still hold whatever the
 * previous machine left there. That is why "written" asks every word, not one.
 *
 * Pure and host-testable: tools/sayit_host_test.py drives it.
 */
#ifndef SAYIT_PHRASE_STORE_H
#define SAYIT_PHRASE_STORE_H

#include <stdint.h>
#include "../../sayit_chars.h"

#define SAYIT_STORE_CHARS 20                       /* two to a word */
#define SAYIT_STORE_WORDS 10
#define SAYIT_STORE_BASE  SAYIT_CHARSET_N          /* 90 */
#define SAYIT_STORE_MAX   (SAYIT_STORE_BASE * SAYIT_STORE_BASE)   /* 8100 */

/* The sound's slots the phrase lives in: the blank parameters F-H, then the
 * seven extra values machine-pages gives a machine that declares them. */
static const int16_t sayit_store_slot[SAYIT_STORE_WORDS] = {
    22, 23, 24, 46, 47, 48, 49, 50, 51, 52
};

/* A character's place in the editor's alphabet; anything else is a space, so a
 * phrase can never store a code the alphabet does not have. */
static int32_t sayit_store_pos(char c)
{
    int32_t i = sayit_ed_charset_pos(c);
    return i < 0 ? SAYIT_CHARSET_N - 1 : i;
}

/* The phrase, padded with spaces to SAYIT_STORE_CHARS, into SAYIT_STORE_WORDS
 * words. A word nothing lands in still gets its spaces, so a saved phrase never
 * reads as never-written. */
static void sayit_store_encode(const char *phrase, int16_t *w)
{
    int32_t i, n = 0;
    if (phrase) {
        while (phrase[n])
            n++;
    }
    for (i = 0; i < SAYIT_STORE_WORDS; i++) {
        int32_t a = sayit_store_pos(2 * i < n ? phrase[2 * i] : ' ');
        int32_t b = sayit_store_pos(2 * i + 1 < n ? phrase[2 * i + 1] : ' ');
        w[i] = (int16_t)(1 + a * SAYIT_STORE_BASE + b);
    }
}

/* The words back into a phrase: at most SAYIT_STORE_CHARS characters, trailing
 * spaces dropped. A word that never held a pair contributes nothing. */
static void sayit_store_decode(const int16_t *w, char *out, int32_t out_sz)
{
    int32_t i, n = 0;

    if (!out || out_sz < 1)
        return;
    for (i = 0; i < SAYIT_STORE_WORDS && n < out_sz - 1; i++) {
        int32_t v = w[i];
        if (v < 1 || v > SAYIT_STORE_MAX)
            continue;
        v--;
        if (n < out_sz - 1)
            out[n++] = SAYIT_CHARSET[v / SAYIT_STORE_BASE];
        if (n < out_sz - 1)
            out[n++] = SAYIT_CHARSET[v % SAYIT_STORE_BASE];
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

/* Every word can have come from sayit_store_encode: the sound carries a phrase.
 * One word outside the range and it carries none - a fresh sound, or one whose
 * words are another machine's leftovers - and the machine writes its own. */
static int32_t sayit_store_valid(const int16_t *w)
{
    int32_t i;
    for (i = 0; i < SAYIT_STORE_WORDS; i++) {
        if (w[i] < 1 || w[i] > SAYIT_STORE_MAX)
            return 0;
    }
    return 1;
}

static int32_t sayit_store_same(const int16_t *a, const int16_t *b)
{
    int32_t i;
    for (i = 0; i < SAYIT_STORE_WORDS; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

#endif /* SAYIT_PHRASE_STORE_H */
