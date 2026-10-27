/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SAYIT's alphabet: the 90 characters a phrase can hold, in the order the
 * editor steps through and the pop-up grid lays out. On its own so that the
 * code that only needs the alphabet - the Digitakt machine's phrase store -
 * does not pull in the whole editor (sayit_edit.h). */
#ifndef SAYIT_CHARS_H
#define SAYIT_CHARS_H

#include <stdint.h>

/* Upper case, lower case, digits, punctuation, space: 90 unique slots. */
static const char SAYIT_CHARSET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    ".,!?'-:;/()&+*\"<=>@#$%_[]~| ";
#define SAYIT_CHARSET_N ((int32_t)(sizeof(SAYIT_CHARSET) - 1))

/* Position of c in the alphabet, or -1. */
static int32_t sayit_ed_charset_pos(char c)
{
    int32_t i;
    for (i = 0; i < SAYIT_CHARSET_N; i++) {
        if (SAYIT_CHARSET[i] == c)
            return i;
        if (!SAYIT_CHARSET[i])
            break;
    }
    return -1;
}

#endif /* SAYIT_CHARS_H */
