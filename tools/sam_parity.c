/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host harness: render text through the ported SAM engine and write a WAV.
 * Used by tools/sam_parity.py to compare the port with 3rdparty/SAM byte for
 * byte. Not part of the machine build.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sam_api.h"

static void put32(FILE *f, unsigned v)
{
    fputc(v & 255, f); fputc((v >> 8) & 255, f);
    fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f);
}
static void put16(FILE *f, unsigned v)
{
    fputc(v & 255, f); fputc((v >> 8) & 255, f);
}

int main(int argc, char **argv)
{
    static unsigned char pcm[22050 * 12];
    struct sam_settings st;
    const char *out, *text;
    int n = 0, v;
    FILE *f;

    if (argc < 8) {
        fprintf(stderr, "usage: sam_parity out.wav pitch speed mouth throat sing text\n");
        return 2;
    }
    out = argv[1];
    st.pitch = (unsigned char)atoi(argv[2]);
    st.speed = (unsigned char)atoi(argv[3]);
    st.mouth = (unsigned char)atoi(argv[4]);
    st.throat = (unsigned char)atoi(argv[5]);
    st.sing = (unsigned char)atoi(argv[6]);
    st.period_q15 = 0;        /* unity: parity is SAM unchanged */
    text = argv[7];

    if (!sam_voice_begin(text, (int)strlen(text), &st)) {
        fprintf(stderr, "nothing to say\n");
        return 1;
    }
    while ((v = sam_voice_next()) >= 0 && n < (int)sizeof(pcm))
        pcm[n++] = (unsigned char)v;
    /* what SPOKEN phoneme string came out, for diagnostics */
    fprintf(stderr, "phonemes:");
    for (v = 0; ((const unsigned char *)sam_voice_phonemes())[v] != 155; v++)
        fprintf(stderr, " %02x", ((const unsigned char *)sam_voice_phonemes())[v]);
    fprintf(stderr, "\nsamples %d\n", n);

    f = fopen(out, "wb");
    if (!f) return 1;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + n); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1);
    put32(f, 22050); put32(f, 22050); put16(f, 1); put16(f, 8);
    fwrite("data", 1, 4, f); put32(f, n); fwrite(pcm, 1, n, f);
    fclose(f);
    return 0;
}
