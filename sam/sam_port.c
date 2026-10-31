/* SPDX-License-Identifier: GPL-2.0-or-later */
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
#define PrintOutput(a, b, c, d, e, f, g, h) ((void)(a), (void)(b), (void)(c), \
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



static unsigned char input[256]; //tab39445
//standard sam sound
static unsigned char speed = 72;
static unsigned char pitch = 64;
static unsigned char mouth = 128;
static unsigned char throat = 128;
static int singmode = 0;


static unsigned char mem39;
static unsigned char mem44;
static unsigned char mem47;
static unsigned char mem49;
static unsigned char mem50;
static unsigned char mem51;
static unsigned char mem53;
static unsigned char mem56;

static unsigned char mem59=0;

static unsigned char A, X, Y;

static unsigned char stress[256]; //numbers from 0 to 8
static unsigned char phonemeLength[256]; //tab40160
static unsigned char phonemeindex[256];

static unsigned char phonemeIndexOutput[60]; //tab47296
static unsigned char stressOutput[60]; //tab47365
static unsigned char phonemeLengthOutput[60]; //tab47416








void SetMouthThroat(unsigned char mouth, unsigned char throat);

// 168=pitches
// 169=frequency1
// 170=frequency2
// 171=frequency3
// 172=amplitude1
// 173=amplitude2
// 174=amplitude3


static void sam_set_mouth_throat(unsigned char mouth, unsigned char throat)
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

static void Init()
{
    int i;
    sam_set_mouth_throat( mouth, throat);


    /*
    freq2data = &mem[45136];
    freq1data = &mem[45056];
    freq3data = &mem[45216];
    */
    //pitches = &mem[43008];
    /*
    frequency1 = &mem[43264];
    frequency2 = &mem[43520];
    frequency3 = &mem[43776];
    */
    /*
    amplitude1 = &mem[44032];
    amplitude2 = &mem[44288];
    amplitude3 = &mem[44544];
    */
    //phoneme = &mem[39904];
    /*
    ampl1data = &mem[45296];
    ampl2data = &mem[45376];
    ampl3data = &mem[45456];
    */

    for(i=0; i<256; i++)
    {
        stress[i] = 0;
        phonemeLength[i] = 0;
    }

    for(i=0; i<60; i++)
    {
        phonemeIndexOutput[i] = 0;
        stressOutput[i] = 0;
        phonemeLengthOutput[i] = 0;
    }
    phonemeindex[255] = 255; //to prevent buffer overflow // ML : changed from 32 to 255 to stop freezing with long inputs

}


//int Code39771()
static int SAMMain()
{
    Init();
    phonemeindex[255] = 32; //to prevent buffer overflow

    if (!Parser1()) return 0;
    if (debug)
        PrintPhonemes(phonemeindex, phonemeLength, stress);
    Parser2();
    CopyStress();
    SetPhonemeLength();
    AdjustLengths();
    Code41240();
    do
    {
        A = phonemeindex[X];
        if (A > 80)
        {
            phonemeindex[X] = 255;
            break; // error: delete all behind it
        }
        X++;
    } while (X != 0);

    //pos39848:
    InsertBreath();

    //mem[40158] = 255;
    if (debug)
    {
        PrintPhonemes(phonemeindex, phonemeLength, stress);
    }

    return 1;
}


//void Code48547()
// One chunk of the utterance at a time. InsertBreath() leaves a 254 marker in
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

//void Code48431()
static void InsertBreath()
{
    unsigned char mem54;
    unsigned char mem55;
    unsigned char index; //variable Y
    mem54 = 255;
    X++;
    mem55 = 0;
    unsigned char mem66 = 0;
    while(1)
    {
        //pos48440:
        X = mem66;
        index = phonemeindex[X];
        if (index == 255) return;
        mem55 += phonemeLength[X];

        if (mem55 < 232)
        {
            if (index != 254) // ML : Prevents an index out of bounds problem
            {
                A = flags2[index]&1;
                if(A != 0)
                {
                    X++;
                    mem55 = 0;
                    Insert(X, 254, mem59, 0);
                    mem66++;
                    mem66++;
                    continue;
                }
            }
            if (index == 0) mem54 = X;
            mem66++;
            continue;
        }
        X = mem54;
        phonemeindex[X] = 31;   // 'Q*' glottal stop
        phonemeLength[X] = 4;
        stress[X] = 0;
        X++;
        mem55 = 0;
        Insert(X, 254, mem59, 0);
        X++;
        mem66 = X;
    }

}

// Iterates through the phoneme buffer, copying the stress value from
// the following phoneme under the following circumstance:

//     1. The current phoneme is voiced, excluding plosives and fricatives
//     2. The following phoneme is voiced, excluding plosives and fricatives, and
//     3. The following phoneme is stressed
//
//  In those cases, the stress value+1 from the following phoneme is copied.
//
// For example, the word LOITER is represented as LOY5TER, with as stress
// of 5 on the diphtong OY. This routine will copy the stress value of 6 (5+1)
// to the L that precedes it.


//void Code41883()
static void CopyStress()
{
    // loop thought all the phonemes to be output
    unsigned char pos=0; //mem66
    while(1)
    {
        // get the phomene
        Y = phonemeindex[pos];

        // exit at end of buffer
        if (Y == 255) return;

        // if CONSONANT_FLAG set, skip - only vowels get stress
        if ((flags[Y] & 64) == 0) {pos++; continue;}
        // get the next phoneme
        Y = phonemeindex[pos+1];
        if (Y == 255) //prevent buffer overflow
        {
            pos++; continue;
        } else
        // if the following phoneme is a vowel, skip
        if ((flags[Y] & 128) == 0)  {pos++; continue;}

        // get the stress value at the next position
        Y = stress[pos+1];

        // if next phoneme is not stressed, skip
        if (Y == 0)  {pos++; continue;}

        // if next phoneme is not a VOWEL OR ER, skip
        if ((Y & 128) != 0)  {pos++; continue;}

        // copy stress from prior phoneme to this one
        stress[pos] = Y+1;

        // advance pointer
        pos++;
    }

}


//void Code41014()
static void Insert(unsigned char position/*var57*/, unsigned char mem60, unsigned char mem59, unsigned char mem58)
{
    int i;
    for(i=253; i >= position; i--) // ML : always keep last safe-guarding 255
    {
        phonemeindex[i+1] = phonemeindex[i];
        phonemeLength[i+1] = phonemeLength[i];
        stress[i+1] = stress[i];
    }

    phonemeindex[position] = mem60;
    phonemeLength[position] = mem59;
    stress[position] = mem58;
    return;
}

// The input[] buffer contains a string of phonemes and stress markers along
// the lines of:
//
//     DHAX KAET IHZ AH5GLIY. <0x9B>
//
// The byte 0x9B marks the end of the buffer. Some phonemes are 2 bytes
// long, such as "DH" and "AX". Others are 1 byte long, such as "T" and "Z".
// There are also stress markers, such as "5" and ".".
//
// The first character of the phonemes are stored in the table signInputTable1[].
// The second character of the phonemes are stored in the table signInputTable2[].
// The stress characters are arranged in low to high stress order in stressInputTable[].
//
// The following process is used to parse the input[] buffer:
//
// Repeat until the <0x9B> character is reached:
//
//        First, a search is made for a 2 character match for phonemes that do not
//        end with the '*' (wildcard) character. On a match, the index of the phoneme
//        is added to phonemeIndex[] and the buffer position is advanced 2 bytes.
//
//        If this fails, a search is made for a 1 character match against all
//        phoneme names ending with a '*' (wildcard). If this succeeds, the
//        phoneme is added to phonemeIndex[] and the buffer position is advanced
//        1 byte.
//
//        If this fails, search for a 1 character match in the stressInputTable[].
//        If this succeeds, the stress value is placed in the last stress[] table
//        at the same index of the last added phoneme, and the buffer position is
//        advanced by 1 byte.
//
//        If this fails, return a 0.
//
// On success:
//
//    1. phonemeIndex[] will contain the index of all the phonemes.
//    2. The last index in phonemeIndex[] will be 255.
//    3. stress[] will contain the stress value for each phoneme

// input[] holds the string of phonemes, each two bytes wide
// signInputTable1[] holds the first character of each phoneme
// signInputTable2[] holds te second character of each phoneme
// phonemeIndex[] holds the indexes of the phonemes after parsing input[]
//
// The parser scans through the input[], finding the names of the phonemes
// by searching signInputTable1[] and signInputTable2[]. On a match, it
// copies the index of the phoneme into the phonemeIndexTable[].
//
// The character <0x9B> marks the end of text in input[]. When it is reached,
// the index 255 is placed at the end of the phonemeIndexTable[], and the
// function returns with a 1 indicating success.
static int Parser1()
{
    int i;
    unsigned char sign1;
    unsigned char sign2;
    unsigned char position = 0;
    X = 0;
    A = 0;
    Y = 0;

    // CLEAR THE STRESS TABLE
    for(i=0; i<256; i++)
        stress[i] = 0;

  // THIS CODE MATCHES THE PHONEME LETTERS TO THE TABLE
    // pos41078:
    while(1)
    {
        // GET THE FIRST CHARACTER FROM THE PHONEME BUFFER
        sign1 = input[X];
        // TEST FOR 155 (�) END OF LINE MARKER
        if (sign1 == 155)
        {
           // MARK ENDPOINT AND RETURN
            phonemeindex[position] = 255;      //mark endpoint
            // REACHED END OF PHONEMES, SO EXIT
            return 1;       //all ok
        }

        // GET THE NEXT CHARACTER FROM THE BUFFER
        X++;
        sign2 = input[X];

        // NOW sign1 = FIRST CHARACTER OF PHONEME, AND sign2 = SECOND CHARACTER OF PHONEME

       // TRY TO MATCH PHONEMES ON TWO TWO-CHARACTER NAME
       // IGNORE PHONEMES IN TABLE ENDING WITH WILDCARDS

       // SET INDEX TO 0
        Y = 0;
pos41095:

         // GET FIRST CHARACTER AT POSITION Y IN signInputTable
         // --> should change name to PhonemeNameTable1
        A = signInputTable1[Y];

        // FIRST CHARACTER MATCHES?
        if (A == sign1)
        {
           // GET THE CHARACTER FROM THE PhonemeSecondLetterTable
            A = signInputTable2[Y];
            // NOT A SPECIAL AND MATCHES SECOND CHARACTER?
            if ((A != '*') && (A == sign2))
            {
               // STORE THE INDEX OF THE PHONEME INTO THE phomeneIndexTable
                phonemeindex[position] = Y;

                // ADVANCE THE POINTER TO THE phonemeIndexTable
                position++;
                // ADVANCE THE POINTER TO THE phonemeInputBuffer
                X++;

                // CONTINUE PARSING
                continue;
            }
        }

        // NO MATCH, TRY TO MATCH ON FIRST CHARACTER TO WILDCARD NAMES (ENDING WITH '*')

        // ADVANCE TO THE NEXT POSITION
        Y++;
        // IF NOT END OF TABLE, CONTINUE
        if (Y != 81) goto pos41095;

// REACHED END OF TABLE WITHOUT AN EXACT (2 CHARACTER) MATCH.
// THIS TIME, SEARCH FOR A 1 CHARACTER MATCH AGAINST THE WILDCARDS

// RESET THE INDEX TO POINT TO THE START OF THE PHONEME NAME TABLE
        Y = 0;
pos41134:
// DOES THE PHONEME IN THE TABLE END WITH '*'?
        if (signInputTable2[Y] == '*')
        {
// DOES THE FIRST CHARACTER MATCH THE FIRST LETTER OF THE PHONEME
            if (signInputTable1[Y] == sign1)
            {
                // SAVE THE POSITION AND MOVE AHEAD
                phonemeindex[position] = Y;

                // ADVANCE THE POINTER
                position++;

                // CONTINUE THROUGH THE LOOP
                continue;
            }
        }
        Y++;
        if (Y != 81) goto pos41134; //81 is size of PHONEME NAME table

// FAILED TO MATCH WITH A WILDCARD. ASSUME THIS IS A STRESS
// CHARACTER. SEARCH THROUGH THE STRESS TABLE

        // SET INDEX TO POSITION 8 (END OF STRESS TABLE)
        Y = 8;

       // WALK BACK THROUGH TABLE LOOKING FOR A MATCH
        while( (sign1 != stressInputTable[Y]) && (Y>0))
        {
  // DECREMENT INDEX
            Y--;
        }

        // REACHED THE END OF THE SEARCH WITHOUT BREAKING OUT OF LOOP?
        if (Y == 0)
        {
            //mem[39444] = X;
            //41181: JSR 42043 //Error
           // FAILED TO MATCH ANYTHING, RETURN 0 ON FAILURE
            return 0;
        }
// SET THE STRESS FOR THE PRIOR PHONEME
        stress[position-1] = Y;
    } //while
}




//change phonemelength depedendent on stress
//void Code41203()
static void SetPhonemeLength()
{
    unsigned char A;
    int position = 0;
    while(phonemeindex[position] != 255 )
    {
        A = stress[position];
        //41218: BMI 41229
        if ((A == 0) || ((A&128) != 0))
        {
            phonemeLength[position] = phonemeLengthTable[phonemeindex[position]];
        } else
        {
            phonemeLength[position] = phonemeStressedLengthTable[phonemeindex[position]];
        }
        position++;
    }
}


static void Code41240()
{
    unsigned char pos=0;

    while(phonemeindex[pos] != 255)
    {
        unsigned char index; //register AC
        X = pos;
        index = phonemeindex[pos];
        if ((flags[index]&2) == 0)
        {
            pos++;
            continue;
        } else
        if ((flags[index]&1) == 0)
        {
            Insert(pos+1, index+1, phonemeLengthTable[index+1], stress[pos]);
            Insert(pos+2, index+2, phonemeLengthTable[index+2], stress[pos]);
            pos += 3;
            continue;
        }

        do
        {
            X++;
            A = phonemeindex[X];
        } while(A==0);

        if (A != 255)
        {
            if ((flags[A] & 8) != 0)  {pos++; continue;}
            if ((A == 36) || (A == 37)) {pos++; continue;} // '/H' '/X'
        }

        Insert(pos+1, index+1, phonemeLengthTable[index+1], stress[pos]);
        Insert(pos+2, index+2, phonemeLengthTable[index+2], stress[pos]);
        pos += 3;
    };

}

// Rewrites the phonemes using the following rules:
//
//       <DIPHTONG ENDING WITH WX> -> <DIPHTONG ENDING WITH WX> WX
//       <DIPHTONG NOT ENDING WITH WX> -> <DIPHTONG NOT ENDING WITH WX> YX
//       UL -> AX L
//       UM -> AX M
//       <STRESSED VOWEL> <SILENCE> <STRESSED VOWEL> -> <STRESSED VOWEL> <SILENCE> Q <VOWEL>
//       T R -> CH R
//       D R -> J R
//       <VOWEL> R -> <VOWEL> RX
//       <VOWEL> L -> <VOWEL> LX
//       G S -> G Z
//       K <VOWEL OR DIPHTONG NOT ENDING WITH IY> -> KX <VOWEL OR DIPHTONG NOT ENDING WITH IY>
//       G <VOWEL OR DIPHTONG NOT ENDING WITH IY> -> GX <VOWEL OR DIPHTONG NOT ENDING WITH IY>
//       S P -> S B
//       S T -> S D
//       S K -> S G
//       S KX -> S GX
//       <ALVEOLAR> UW -> <ALVEOLAR> UX
//       CH -> CH CH' (CH requires two phonemes to represent it)
//       J -> J J' (J requires two phonemes to represent it)
//       <UNSTRESSED VOWEL> T <PAUSE> -> <UNSTRESSED VOWEL> DX <PAUSE>
//       <UNSTRESSED VOWEL> D <PAUSE>  -> <UNSTRESSED VOWEL> DX <PAUSE>


//void Code41397()
static void Parser2()
{
    if (debug) printf("Parser2\n");
    unsigned char pos = 0; //mem66;
    unsigned char mem58 = 0;


  // Loop through phonemes
    while(1)
    {
// SET X TO THE CURRENT POSITION
        X = pos;
// GET THE PHONEME AT THE CURRENT POSITION
        A = phonemeindex[pos];

// DEBUG: Print phoneme and index
        if (debug && A != 255) printf("%d: %c%c\n", X, signInputTable1[A], signInputTable2[A]);

// Is phoneme pause?
        if (A == 0)
        {
// Move ahead to the
            pos++;
            continue;
        }

// If end of phonemes flag reached, exit routine
        if (A == 255) return;

// Copy the current phoneme index to Y
        Y = A;

// RULE:
//       <DIPHTONG ENDING WITH WX> -> <DIPHTONG ENDING WITH WX> WX
//       <DIPHTONG NOT ENDING WITH WX> -> <DIPHTONG NOT ENDING WITH WX> YX
// Example: OIL, COW


// Check for DIPHTONG
        if ((flags[A] & 16) == 0) goto pos41457;

// Not a diphthong. Get the stress
        mem58 = stress[pos];

// End in IY sound?
        A = flags[Y] & 32;

// If ends with IY, use YX, else use WX
        if (A == 0) A = 20; else A = 21;    // 'WX' = 20 'YX' = 21
        //pos41443:
// Insert at WX or YX following, copying the stress

        if (debug) if (A==20) printf("RULE: insert WX following diphtong NOT ending in IY sound\n");
        if (debug) if (A==21) printf("RULE: insert YX following diphtong ending in IY sound\n");
        Insert(pos+1, A, mem59, mem58);
        X = pos;
// Jump to ???
        goto pos41749;



pos41457:

// RULE:
//       UL -> AX L
// Example: MEDDLE

// Get phoneme
        A = phonemeindex[X];
// Skip this rule if phoneme is not UL
        if (A != 78) goto pos41487;  // 'UL'
        A = 24;         // 'L'                 //change 'UL' to 'AX L'

        if (debug) printf("RULE: UL -> AX L\n");

pos41466:
// Get current phoneme stress
        mem58 = stress[X];

// Change UL to AX
        phonemeindex[X] = 13;  // 'AX'
// Perform insert. Note code below may jump up here with different values
        Insert(X+1, A, mem59, mem58);
        pos++;
// Move to next phoneme
        continue;

pos41487:

// RULE:
//       UM -> AX M
// Example: ASTRONOMY

// Skip rule if phoneme != UM
        if (A != 79) goto pos41495;   // 'UM'
        // Jump up to branch - replaces current phoneme with AX and continues
        A = 27; // 'M'  //change 'UM' to  'AX M'
        if (debug) printf("RULE: UM -> AX M\n");
        goto pos41466;
pos41495:

// RULE:
//       UN -> AX N
// Example: FUNCTION


// Skip rule if phoneme != UN
        if (A != 80) goto pos41503; // 'UN'

        // Jump up to branch - replaces current phoneme with AX and continues
        A = 28;         // 'N' //change UN to 'AX N'
        if (debug) printf("RULE: UN -> AX N\n");
        goto pos41466;
pos41503:

// RULE:
//       <STRESSED VOWEL> <SILENCE> <STRESSED VOWEL> -> <STRESSED VOWEL> <SILENCE> Q <VOWEL>
// EXAMPLE: AWAY EIGHT

        Y = A;
// VOWEL set?
        A = flags[A] & 128;

// Skip if not a vowel
        if (A != 0)
        {
// Get the stress
            A = stress[X];

// If stressed...
            if (A != 0)
            {
// Get the following phoneme
                X++;
                A = phonemeindex[X];
// If following phoneme is a pause

                if (A == 0)
                {
// Get the phoneme following pause
                    X++;
                    Y = phonemeindex[X];

// Check for end of buffer flag
                    if (Y == 255) //buffer overflow
// ??? Not sure about these flags
                        A = 65&128;
                    else
// And VOWEL flag to current phoneme's flags
                        A = flags[Y] & 128;

// If following phonemes is not a pause
                    if (A != 0)
                    {
// If the following phoneme is not stressed
                        A = stress[X];
                        if (A != 0)
                        {
// Insert a glottal stop and move forward
                            if (debug) printf("RULE: Insert glottal stop between two stressed vowels with space between them\n");
                            // 31 = 'Q'
                            Insert(X, 31, mem59, 0);
                            pos++;
                            continue;
                        }
                    }
                }
            }
        }


// RULES FOR PHONEMES BEFORE R
//        T R -> CH R
// Example: TRACK


// Get current position and phoneme
        X = pos;
        A = phonemeindex[pos];
        if (A != 23) goto pos41611;     // 'R'

// Look at prior phoneme
        X--;
        A = phonemeindex[pos-1];
        //pos41567:
        if (A == 69)                    // 'T'
        {
// Change T to CH
            if (debug) printf("RULE: T R -> CH R\n");
            phonemeindex[pos-1] = 42;
            goto pos41779;
        }


// RULES FOR PHONEMES BEFORE R
//        D R -> J R
// Example: DRY

// Prior phonemes D?
        if (A == 57)                    // 'D'
        {
// Change D to J
            phonemeindex[pos-1] = 44;
            if (debug) printf("RULE: D R -> J R\n");
            goto pos41788;
        }

// RULES FOR PHONEMES BEFORE R
//        <VOWEL> R -> <VOWEL> RX
// Example: ART


// If vowel flag is set change R to RX
        A = flags[A] & 128;
        if (debug) printf("RULE: R -> RX\n");
        if (A != 0) phonemeindex[pos] = 18;  // 'RX'

// continue to next phoneme
        pos++;
        continue;

pos41611:

// RULE:
//       <VOWEL> L -> <VOWEL> LX
// Example: ALL

// Is phoneme L?
        if (A == 24)    // 'L'
        {
// If prior phoneme does not have VOWEL flag set, move to next phoneme
            if ((flags[phonemeindex[pos-1]] & 128) == 0) {pos++; continue;}
// Prior phoneme has VOWEL flag set, so change L to LX and move to next phoneme
            if (debug) printf("RULE: <VOWEL> L -> <VOWEL> LX\n");
            phonemeindex[X] = 19;     // 'LX'
            pos++;
            continue;
        }

// RULE:
//       G S -> G Z
//
// Can't get to fire -
//       1. The G -> GX rule intervenes
//       2. Reciter already replaces GS -> GZ

// Is current phoneme S?
        if (A == 32)    // 'S'
        {
// If prior phoneme is not G, move to next phoneme
            if (phonemeindex[pos-1] != 60) {pos++; continue;}
// Replace S with Z and move on
            if (debug) printf("RULE: G S -> G Z\n");
            phonemeindex[pos] = 38;    // 'Z'
            pos++;
            continue;
        }

// RULE:
//             K <VOWEL OR DIPHTONG NOT ENDING WITH IY> -> KX <VOWEL OR DIPHTONG NOT ENDING WITH IY>
// Example: COW

// Is current phoneme K?
        if (A == 72)    // 'K'
        {
// Get next phoneme
            Y = phonemeindex[pos+1];
// If at end, replace current phoneme with KX
            if (Y == 255) phonemeindex[pos] = 75; // ML : prevents an index out of bounds problem
            else
            {
// VOWELS AND DIPHTONGS ENDING WITH IY SOUND flag set?
                A = flags[Y] & 32;
                if (debug) if (A==0) printf("RULE: K <VOWEL OR DIPHTONG NOT ENDING WITH IY> -> KX <VOWEL OR DIPHTONG NOT ENDING WITH IY>\n");
// Replace with KX
                if (A == 0) phonemeindex[pos] = 75;  // 'KX'
            }
        }
        else

// RULE:
//             G <VOWEL OR DIPHTONG NOT ENDING WITH IY> -> GX <VOWEL OR DIPHTONG NOT ENDING WITH IY>
// Example: GO


// Is character a G?
        if (A == 60)   // 'G'
        {
// Get the following character
            unsigned char index = phonemeindex[pos+1];

// At end of buffer?
            if (index == 255) //prevent buffer overflow
            {
                pos++; continue;
            }
            else
// If diphtong ending with YX, move continue processing next phoneme
            if ((flags[index] & 32) != 0) {pos++; continue;}
// replace G with GX and continue processing next phoneme
            if (debug) printf("RULE: G <VOWEL OR DIPHTONG NOT ENDING WITH IY> -> GX <VOWEL OR DIPHTONG NOT ENDING WITH IY>\n");
            phonemeindex[pos] = 63; // 'GX'
            pos++;
            continue;
        }

// RULE:
//      S P -> S B
//      S T -> S D
//      S K -> S G
//      S KX -> S GX
// Examples: SPY, STY, SKY, SCOWL

        Y = phonemeindex[pos];
        //pos41719:
// Replace with softer version?
        A = flags[Y] & 1;
        if (A == 0) goto pos41749;
        A = phonemeindex[pos-1];
        if (A != 32)    // 'S'
        {
            A = Y;
            goto pos41812;
        }
        // Replace with softer version
        if (debug) printf("RULE: S* %c%c -> S* %c%c\n", signInputTable1[Y], signInputTable2[Y],signInputTable1[Y-12], signInputTable2[Y-12]);
        phonemeindex[pos] = Y-12;
        pos++;
        continue;


pos41749:

// RULE:
//      <ALVEOLAR> UW -> <ALVEOLAR> UX
//
// Example: NEW, DEW, SUE, ZOO, THOO, TOO

//       UW -> UX

        A = phonemeindex[X];
        if (A == 53)    // 'UW'
        {
// ALVEOLAR flag set?
            Y = phonemeindex[X-1];
            A = flags2[Y] & 4;
// If not set, continue processing next phoneme
            if (A == 0) {pos++; continue;}
            if (debug) printf("RULE: <ALVEOLAR> UW -> <ALVEOLAR> UX\n");
            phonemeindex[X] = 16;
            pos++;
            continue;
        }
pos41779:

// RULE:
//       CH -> CH CH' (CH requires two phonemes to represent it)
// Example: CHEW

        if (A == 42)    // 'CH'
        {
            //        pos41783:
            if (debug) printf("CH -> CH CH+1\n");
            Insert(X+1, A+1, mem59, stress[X]);
            pos++;
            continue;
        }

pos41788:

// RULE:
//       J -> J J' (J requires two phonemes to represent it)
// Example: JAY


        if (A == 44) // 'J'
        {
            if (debug) printf("J -> J J+1\n");
            Insert(X+1, A+1, mem59, stress[X]);
            pos++;
            continue;
        }

// Jump here to continue
pos41812:

// RULE: Soften T following vowel
// NOTE: This rule fails for cases such as "ODD"
//       <UNSTRESSED VOWEL> T <PAUSE> -> <UNSTRESSED VOWEL> DX <PAUSE>
//       <UNSTRESSED VOWEL> D <PAUSE>  -> <UNSTRESSED VOWEL> DX <PAUSE>
// Example: PARTY, TARDY


// Past this point, only process if phoneme is T or D

        if (A != 69)    // 'T'
        if (A != 57) {pos++; continue;}       // 'D'
        //pos41825:


// If prior phoneme is not a vowel, continue processing phonemes
        if ((flags[phonemeindex[X-1]] & 128) == 0) {pos++; continue;}

// Get next phoneme
        X++;
        A = phonemeindex[X];
        //pos41841
// Is the next phoneme a pause?
        if (A != 0)
        {
// If next phoneme is not a pause, continue processing phonemes
            if ((flags[A] & 128) == 0) {pos++; continue;}
// If next phoneme is stressed, continue processing phonemes
// FIXME: How does a pause get stressed?
            if (stress[X] != 0) {pos++; continue;}
//pos41856:
// Set phonemes to DX
        if (debug) printf("RULE: Soften T or D following vowel or ER and preceding a pause -> DX\n");
        phonemeindex[pos] = 30;       // 'DX'
        } else
        {
            A = phonemeindex[X+1];
            if (A == 255) //prevent buffer overflow
                A = 65 & 128;
            else
// Is next phoneme a vowel or ER?
                A = flags[A] & 128;
            if (debug) if (A != 0) printf("RULE: Soften T or D following vowel or ER and preceding a pause -> DX\n");
            if (A != 0) phonemeindex[pos] = 30;  // 'DX'
        }

        pos++;

    } // while
}


// Applies various rules that adjust the lengths of phonemes
//
//         Lengthen <FRICATIVE> or <VOICED> between <VOWEL> and <PUNCTUATION> by 1.5
//         <VOWEL> <RX | LX> <CONSONANT> - decrease <VOWEL> length by 1
//         <VOWEL> <UNVOICED PLOSIVE> - decrease vowel by 1/8th
//         <VOWEL> <UNVOICED CONSONANT> - increase vowel by 1/2 + 1
//         <NASAL> <STOP CONSONANT> - set nasal = 5, consonant = 6
//         <VOICED STOP CONSONANT> {optional silence} <STOP CONSONANT> - shorten both to 1/2 + 1
//         <LIQUID CONSONANT> <DIPHTONG> - decrease by 2


//void Code48619()
static void AdjustLengths()
{

    // LENGTHEN VOWELS PRECEDING PUNCTUATION
    //
    // Search for punctuation. If found, back up to the first vowel, then
    // process all phonemes between there and up to (but not including) the punctuation.
    // If any phoneme is found that is a either a fricative or voiced, the duration is
    // increased by (length * 1.5) + 1

    // loop index
    X = 0;
    unsigned char index;

    // iterate through the phoneme list
    unsigned char loopIndex=0;
    while(1)
    {
        // get a phoneme
        index = phonemeindex[X];

        // exit loop if end on buffer token
        if (index == 255) break;

        // not punctuation?
        if((flags2[index] & 1) == 0)
        {
            // skip
            X++;
            continue;
        }

        // hold index
        loopIndex = X;

        // Loop backwards from this point
pos48644:

        // back up one phoneme
        X--;

        // stop once the beginning is reached
        if(X == 0) break;

        // get the preceding phoneme
        index = phonemeindex[X];

        if (index != 255) //inserted to prevent access overrun
        if((flags[index] & 128) == 0) goto pos48644; // if not a vowel, continue looping

        //pos48657:
        do
        {
            // test for vowel
            index = phonemeindex[X];

            if (index != 255)//inserted to prevent access overrun
            // test for fricative/unvoiced or not voiced
            if(((flags2[index] & 32) == 0) || ((flags[index] & 4) != 0))     //nochmal �berpr�fen
            {
                //A = flags[Y] & 4;
                //if(A == 0) goto pos48688;

                // get the phoneme length
                A = phonemeLength[X];

                // change phoneme length to (length * 1.5) + 1
                A = (A >> 1) + A + 1;
if (debug) printf("RULE: Lengthen <FRICATIVE> or <VOICED> between <VOWEL> and <PUNCTUATION> by 1.5\n");
if (debug) printf("PRE\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", X, signInputTable1[phonemeindex[X]], signInputTable2[phonemeindex[X]], phonemeLength[X]);

                phonemeLength[X] = A;

if (debug) printf("POST\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", X, signInputTable1[phonemeindex[X]], signInputTable2[phonemeindex[X]], phonemeLength[X]);

            }
            // keep moving forward
            X++;
        } while (X != loopIndex);
        //  if (X != loopIndex) goto pos48657;
        X++;
    }  // while

    // Similar to the above routine, but shorten vowels under some circumstances

    // Loop throught all phonemes
    loopIndex = 0;
    //pos48697

    while(1)
    {
        // get a phoneme
        X = loopIndex;
        index = phonemeindex[X];

        // exit routine at end token
        if (index == 255) return;

        // vowel?
        A = flags[index] & 128;
        if (A != 0)
        {
            // get next phoneme
            X++;
            index = phonemeindex[X];

            // get flags
            if (index == 255)
            mem56 = 65; // use if end marker
            else
            mem56 = flags[index];

            // not a consonant
            if ((flags[index] & 64) == 0)
            {
                // RX or LX?
                if ((index == 18) || (index == 19))  // 'RX' & 'LX'
                {
                    // get the next phoneme
                    X++;
                    index = phonemeindex[X];

                    // next phoneme a consonant?
                    if ((flags[index] & 64) != 0) {
                        // RULE: <VOWEL> RX | LX <CONSONANT>


if (debug) printf("RULE: <VOWEL> <RX | LX> <CONSONANT> - decrease length by 1\n");
if (debug) printf("PRE\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", loopIndex, signInputTable1[phonemeindex[loopIndex]], signInputTable2[phonemeindex[loopIndex]], phonemeLength[loopIndex]);

                        // decrease length of vowel by 1 frame
                        phonemeLength[loopIndex]--;

if (debug) printf("POST\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", loopIndex, signInputTable1[phonemeindex[loopIndex]], signInputTable2[phonemeindex[loopIndex]], phonemeLength[loopIndex]);

                    }
                    // move ahead
                    loopIndex++;
                    continue;
                }
                // move ahead
                loopIndex++;
                continue;
            }


            // Got here if not <VOWEL>

            // not voiced
            if ((mem56 & 4) == 0)
            {

                 // Unvoiced
                 // *, .*, ?*, ,*, -*, DX, S*, SH, F*, TH, /H, /X, CH, P*, T*, K*, KX

                // not an unvoiced plosive?
                if((mem56 & 1) == 0) {
                    // move ahead
                    loopIndex++;
                    continue;
                }

                // P*, T*, K*, KX


                // RULE: <VOWEL> <UNVOICED PLOSIVE>
                // <VOWEL> <P*, T*, K*, KX>

                // move back
                X--;

if (debug) printf("RULE: <VOWEL> <UNVOICED PLOSIVE> - decrease vowel by 1/8th\n");
if (debug) printf("PRE\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", X, signInputTable1[phonemeindex[X]], signInputTable2[phonemeindex[X]],  phonemeLength[X]);

                // decrease length by 1/8th
                mem56 = phonemeLength[X] >> 3;
                phonemeLength[X] -= mem56;

if (debug) printf("POST\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", X, signInputTable1[phonemeindex[X]], signInputTable2[phonemeindex[X]], phonemeLength[X]);

                // move ahead
                loopIndex++;
                continue;
            }

            // RULE: <VOWEL> <VOICED CONSONANT>
            // <VOWEL> <WH, R*, L*, W*, Y*, M*, N*, NX, DX, Q*, Z*, ZH, V*, DH, J*, B*, D*, G*, GX>

if (debug) printf("RULE: <VOWEL> <VOICED CONSONANT> - increase vowel by 1/2 + 1\n");
if (debug) printf("PRE\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", X-1, signInputTable1[phonemeindex[X-1]], signInputTable2[phonemeindex[X-1]],  phonemeLength[X-1]);

            // decrease length
            A = phonemeLength[X-1];
            phonemeLength[X-1] = (A >> 2) + A + 1;     // 5/4*A + 1

if (debug) printf("POST\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", X-1, signInputTable1[phonemeindex[X-1]], signInputTable2[phonemeindex[X-1]], phonemeLength[X-1]);

            // move ahead
            loopIndex++;
            continue;

        }


        // WH, R*, L*, W*, Y*, M*, N*, NX, Q*, Z*, ZH, V*, DH, J*, B*, D*, G*, GX

//pos48821:

        // RULE: <NASAL> <STOP CONSONANT>
        //       Set punctuation length to 6
        //       Set stop consonant length to 5

        // nasal?
        if((flags2[index] & 8) != 0)
        {

            // M*, N*, NX,

            // get the next phoneme
            X++;
            index = phonemeindex[X];

            // end of buffer?
            if (index == 255)
               A = 65&2;  //prevent buffer overflow
            else
                A = flags[index] & 2; // check for stop consonant


            // is next phoneme a stop consonant?
            if (A != 0)

               // B*, D*, G*, GX, P*, T*, K*, KX

            {
if (debug) printf("RULE: <NASAL> <STOP CONSONANT> - set nasal = 5, consonant = 6\n");
if (debug) printf("POST\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", X, signInputTable1[phonemeindex[X]], signInputTable2[phonemeindex[X]], phonemeLength[X]);
if (debug) printf("phoneme %d (%c%c) length %d\n", X-1, signInputTable1[phonemeindex[X-1]], signInputTable2[phonemeindex[X-1]], phonemeLength[X-1]);

                // set stop consonant length to 6
                phonemeLength[X] = 6;

                // set nasal length to 5
                phonemeLength[X-1] = 5;

if (debug) printf("POST\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", X, signInputTable1[phonemeindex[X]], signInputTable2[phonemeindex[X]], phonemeLength[X]);
if (debug) printf("phoneme %d (%c%c) length %d\n", X-1, signInputTable1[phonemeindex[X-1]], signInputTable2[phonemeindex[X-1]], phonemeLength[X-1]);

            }
            // move to next phoneme
            loopIndex++;
            continue;
        }


        // WH, R*, L*, W*, Y*, Q*, Z*, ZH, V*, DH, J*, B*, D*, G*, GX

        // RULE: <VOICED STOP CONSONANT> {optional silence} <STOP CONSONANT>
        //       Shorten both to (length/2 + 1)

        // (voiced) stop consonant?
        if((flags[index] & 2) != 0)
        {
            // B*, D*, G*, GX

            // move past silence
            do
            {
                // move ahead
                X++;
                index = phonemeindex[X];
            } while(index == 0);


            // check for end of buffer
            if (index == 255) //buffer overflow
            {
                // ignore, overflow code
                if ((65 & 2) == 0) {loopIndex++; continue;}
            } else if ((flags[index] & 2) == 0) {
                // if another stop consonant, move ahead
                loopIndex++;
                continue;
            }

            // RULE: <UNVOICED STOP CONSONANT> {optional silence} <STOP CONSONANT>
if (debug) printf("RULE: <UNVOICED STOP CONSONANT> {optional silence} <STOP CONSONANT> - shorten both to 1/2 + 1\n");
if (debug) printf("PRE\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", X, signInputTable1[phonemeindex[X]], signInputTable2[phonemeindex[X]], phonemeLength[X]);
if (debug) printf("phoneme %d (%c%c) length %d\n", X-1, signInputTable1[phonemeindex[X-1]], signInputTable2[phonemeindex[X-1]], phonemeLength[X-1]);
// X gets overwritten, so hold prior X value for debug statement
int debugX = X;
            // shorten the prior phoneme length to (length/2 + 1)
            phonemeLength[X] = (phonemeLength[X] >> 1) + 1;
            X = loopIndex;

            // also shorten this phoneme length to (length/2 +1)
            phonemeLength[loopIndex] = (phonemeLength[loopIndex] >> 1) + 1;

if (debug) printf("POST\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", debugX, signInputTable1[phonemeindex[debugX]], signInputTable2[phonemeindex[debugX]], phonemeLength[debugX]);
if (debug) printf("phoneme %d (%c%c) length %d\n", debugX-1, signInputTable1[phonemeindex[debugX-1]], signInputTable2[phonemeindex[debugX-1]], phonemeLength[debugX-1]);


            // move ahead
            loopIndex++;
            continue;
        }


        // WH, R*, L*, W*, Y*, Q*, Z*, ZH, V*, DH, J*, **,

        // RULE: <VOICED NON-VOWEL> <DIPHTONG>
        //       Decrease <DIPHTONG> by 2

        // liquic consonant?
        if ((flags2[index] & 16) != 0)
        {
            // R*, L*, W*, Y*

            // get the prior phoneme
            index = phonemeindex[X-1];

            // prior phoneme a stop consonant>
            if((flags[index] & 2) != 0) {
                             // Rule: <LIQUID CONSONANT> <DIPHTONG>

if (debug) printf("RULE: <LIQUID CONSONANT> <DIPHTONG> - decrease by 2\n");
if (debug) printf("PRE\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", X, signInputTable1[phonemeindex[X]], signInputTable2[phonemeindex[X]], phonemeLength[X]);

             // decrease the phoneme length by 2 frames (20 ms)
             phonemeLength[X] -= 2;

if (debug) printf("POST\n");
if (debug) printf("phoneme %d (%c%c) length %d\n", X, signInputTable1[phonemeindex[X]], signInputTable2[phonemeindex[X]], phonemeLength[X]);
        }
         }

         // move to next phoneme
         loopIndex++;
         continue;
    }
//            goto pos48701;
}

// -------------------------------------------------------------------------
// ML : Code47503 is division with remainder, and mem50 gets the sign
void Code47503(unsigned char mem52)
{

    Y = 0;
    if ((mem53 & 128) != 0)
    {
        mem53 = -mem53;
        Y = 128;
    }
    mem50 = Y;
    A = 0;
    for(X=8; X > 0; X--)
    {
        int temp = mem53;
        mem53 = mem53 << 1;
        A = A << 1;
        if (temp >= 128) A++;
        if (A >= mem52)
        {
            A = A - mem52;
            mem53++;
        }
    }

    mem51 = A;
    if ((mem50 & 128) != 0) mem53 = -mem53;

}


static unsigned char pitches[256]; // tab43008

static unsigned char frequency1[256];
static unsigned char frequency2[256];
static unsigned char frequency3[256];

static unsigned char amplitude1[256];
static unsigned char amplitude2[256];
static unsigned char amplitude3[256];

static unsigned char sampledConsonantFlag[256]; // tab44800


static void AddInflection(unsigned char mem48, unsigned char phase1);
static unsigned char trans(unsigned char mem39212, unsigned char mem39213);


// contains the final soundbuffer
extern int bufferpos;
extern char *buffer;



//timetable for more accurate c64 simulation
static int timetable[5][5] =
{
    {162, 167, 167, 127, 128},
    {226, 60, 60, 0, 0},
    {225, 60, 59, 0, 0},
    {200, 0, 0, 54, 55},
    {199, 0, 0, 54, 54}
};

/* SAM wrote its 8-bit output into a 220 KB malloc'd buffer indexed by
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


//written by me because of different table positions.
// mem[47] = ...
// 168=pitches
// 169=frequency1
// 170=frequency2
// 171=frequency3
// 172=amplitude1
// 173=amplitude2
// 174=amplitude3
unsigned char Read(unsigned char p, unsigned char Y)
{
    switch(p)
    {
    case 168: return pitches[Y];
    case 169: return frequency1[Y];
    case 170: return frequency2[Y];
    case 171: return frequency3[Y];
    case 172: return amplitude1[Y];
    case 173: return amplitude2[Y];
    case 174: return amplitude3[Y];
    }
    printf("Error reading to tables");
    return 0;
}

void Write(unsigned char p, unsigned char Y, unsigned char value)
{

    switch(p)
    {
    case 168: pitches[Y] = value; return;
    case 169: frequency1[Y] = value;  return;
    case 170: frequency2[Y] = value;  return;
    case 171: frequency3[Y] = value;  return;
    case 172: amplitude1[Y] = value;  return;
    case 173: amplitude2[Y] = value;  return;
    case 174: amplitude3[Y] = value;  return;
    }
    printf("Error writing to tables\n");
}



// -------------------------------------------------------------------------
//Code48227
// Render a sampled sound from the sampleTable.
//
//   Phoneme   Sample Start   Sample End
//   32: S*    15             255
//   33: SH    257            511
//   34: F*    559            767
//   35: TH    583            767
//   36: /H    903            1023
//   37: /X    1135           1279
//   38: Z*    84             119
//   39: ZH    340            375
//   40: V*    596            639
//   41: DH    596            631
//
//   42: CH
//   43: **    399            511
//
//   44: J*
//   45: **    257            276
//   46: **
//
//   66: P*
//   67: **    743            767
//   68: **
//
//   69: T*
//   70: **    231            255
//   71: **
//
// The SampledPhonemesTable[] holds flags indicating if a phoneme is
// voiced or not. If the upper 5 bits are zero, the sample is voiced.
//
// Samples in the sampleTable are compressed, with bits being converted to
// bytes from high bit to low, as follows:
//
//   unvoiced 0 bit   -> X
//   unvoiced 1 bit   -> 5
//
//   voiced 0 bit     -> 6
//   voiced 1 bit     -> 24
//
// Where X is a value from the table:
//
//   { 0x18, 0x1A, 0x17, 0x17, 0x17 };
//
// The index into this table is determined by masking off the lower
// 3 bits from the SampledPhonemesTable:
//
//        index = (SampledPhonemesTable[i] & 7) - 1;
//
// For voices samples, samples are interleaved between voiced output.


// Code48227()
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

    // mask low three bits and subtract 1 get value to
    // convert 0 bits on unvoiced samples.
    A = mem39&7;
    X = A-1;

    // store the result
    mem56 = X;

    // determine which offset to use from table { 0x18, 0x1A, 0x17, 0x17, 0x17 }
    // T, S, Z                0          0x18
    // CH, J, SH, ZH          1          0x1A
    // P, F*, V, TH, DH       2          0x17
    // /H                     3          0x17
    // /X                     4          0x17

    // get value from the table
    mem53 = tab48426[X];
    mem47 = X;      //46016+mem[56]*256

    // voiced sample?
    A = mem39 & 248;
    if(A == 0)
    {
        // voiced phoneme: Z*, ZH, V*, DH
        Y = mem49;
        A = pitches[mem49] >> 4;

        // jump to voiced portion
        goto pos48315;
    }

    Y = A ^ 255;
pos48274:

    // step through the 8 bits in the sample
    mem56 = 8;

    // get the next sample from the table
    // mem47*256 = offset to start of samples
    A = sampleTable[mem47*256+Y];
pos48280:
    if (sam_ring_full()) { rs_resume = 1; return 1; }

    // left shift to get the high bit
    tempA = A;
    A = A << 1;
    //48281: BCC 48290

    // bit not set?
    if ((tempA & 128) == 0)
    {
        // convert the bit to value from table
        X = mem53;
        //mem[54296] = X;
        // output the byte
        Output8Bit(1, (X&0x0f) * 16);
        // if X != 0, exit loop
        if(X != 0) goto pos48296;
    }

    // output a 5 for the on bit
    Output8Bit(2, 5 * 16);

    //48295: NOP
pos48296:

    X = 0;

    // decrement counter
    mem56--;

    // if not done, jump to top of loop
    if (mem56 != 0) goto pos48280;

    // increment position
    Y++;
    if (Y != 0) goto pos48274;

    // restore values and return
    mem44 = 1;
    gl_len = 1;
    Y = mem49;
    rs_resume = 0;
    return 0;


pos48315:
// handle voiced samples here

   // number of samples?
    rs_phase = A ^ 255;

    Y = *mem66;
    do
    {
        //pos48321:

        // shift through all 8 bits
        mem56 = 8;
        //A = Read(mem47, Y);

        // fetch value from table
        A = sampleTable[mem47*256+Y];

        // loop 8 times
        //pos48327:
pos48327:
        if (sam_ring_full()) { rs_resume = 2; return 1; }
        do
        {
            //48327: ASL A
            //48328: BCC 48337

            // left shift and check high bit
            tempA = A;
            A = A << 1;
            if ((tempA & 128) != 0)
            {
                // if bit set, output 26
                X = 26;
                Output8Bit(3, (X&0xf)*16);
            } else
            {
                //timetable 4
                // bit is not set, output a 6
                X=6;
                Output8Bit(4, (X&0xf)*16);
            }

            mem56--;
        } while(mem56 != 0);

        // move ahead in the table
        Y++;

        // continue until counter done
        rs_phase++;

    } while (rs_phase != 0);
    //  if (phase1 != 0) goto pos48321;

    // restore values and return
    A = 1;
    mem44 = 1;
    gl_len = 1;
    *mem66 = Y;
    Y = mem49;
    rs_resume = 0;
    return 0;
}



// RENDER THE PHONEMES IN THE LIST
//
// The phoneme list is converted into sound through the steps:
//
// 1. Copy each phoneme <length> number of times into the frames list,
//    where each frame represents 10 milliseconds of sound.
//
// 2. Determine the transitions lengths between phonemes, and linearly
//    interpolate the values across the frames.
//
// 3. Offset the pitches by the fundamental frequency.
//
// 4. Render the each frame.



//void Code47574()
/* Frames, transitions, pitch contour and amplitude rescale for one chunk.
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

/* ---- a chunk's tables, made in slices ------------------------------------ */
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
        //pos47810:

      // mem47 is used to index the tables:
      // 168  pitches[]
      // 169  frequency1
      // 170  frequency2
      // 171  frequency3
      // 172  amplitude1
      // 173  amplitude2
      // 174  amplitude3

        mem40 = mem38;

        if (mem47 == 168)     // pitch
        {

           // unlike the other values, the pitches[] interpolates from
           // the middle of the current phoneme to the middle of the
           // next phoneme

            unsigned char mem36, mem37;
            // half the width of the current phoneme
            mem36 = phonemeLengthOutput[mem44] >> 1;
            // half the width of the next phoneme
            mem37 = phonemeLengthOutput[mem44+1] >> 1;
            // sum the values
            mem40 = mem36 + mem37; // length of both halves
            mem37 += mem49; // center of next phoneme
            mem36 = mem49 - mem36; // center index of current phoneme
            A = Read(mem47, mem37); // value at center of next phoneme - end interpolation value
            //A = mem[address];

            Y = mem36; // start index of interpolation
            mem53 = A - Read(mem47, mem36); // value to center of current phoneme
        } else
        {
            // value to interpolate to
            A = Read(mem47, speedcounter);
            // position to start interpolation from
            Y = phase3;
            // value to interpolate from
            mem53 = A - Read(mem47, phase3);
        }

        //Code47503(mem40);
        // ML : Code47503 is division with remainder, and mem50 gets the sign

        // calculate change per frame
        signed char m53 = (signed char)mem53;
        mem50 = mem53 & 128;
        unsigned char m53abs = abs(m53);
        mem51 = m53abs % mem40; //abs((char)m53) % mem40;
        mem53 = (unsigned char)((signed char)(m53) / mem40);

        // interpolation range
        X = mem40; // number of frames to interpolate over
        Y = phase3; // starting frame


        // linearly interpolate values

        mem56 = 0;
        //47907: CLC
        //pos47908:
        while(1)     //while No. 3
        {
            A = Read(mem47, Y) + mem53; //carry alway cleared

            mem48 = A;
            Y++;
            X--;
            if(X == 0) break;

            mem56 += mem51;
            if (mem56 >= mem40)  //???
            {
                mem56 -= mem40; //carry? is set
                //if ((mem56 & 128)==0)
                if ((mem50 & 128)==0)
                {
                    //47935: BIT 50
                    //47937: BMI 47943
                    if(mem48 != 0) mem48++;
                } else mem48--;
            }
            //pos47945:
            Write(mem47, Y, mem48);
        } //while No. 3

        //pos47952:
        mem47++;
        //if (mem47 != 175) goto pos47810;
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
}

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
        // get the sampled information on the phoneme
        A = sampledConsonantFlag[Y];
        mem39 = A;

        // unvoiced sampled phoneme?
        A = A & 248;
        if(A != 0)
        {
            // render the sample for the phoneme
            rs_after = 1;
            if (RenderSample(&mem66)) return 1;
            rs_after = 0;

            // skip ahead two in the phoneme buffer
            Y += 2;
            mem48 -= 2;
        } else
        {
            // simulate the glottal pulse and formants
            unsigned char ary[5];
            unsigned int p1 = phase1 * 256; // Fixed point integers because we need to divide later on
            unsigned int p2 = phase2 * 256;
            unsigned int p3 = phase3 * 256;
            int k;
            for (k=0; k<5; k++) {
                signed char sp1 = (signed char)sinus[0xff & (p1>>8)];
                signed char sp2 = (signed char)sinus[0xff & (p2>>8)];
                signed char rp3 = (signed char)rectangle[0xff & (p3>>8)];
                signed int sin1 = sp1 * ((unsigned char)amplitude1[Y] & 0x0f);
                signed int sin2 = sp2 * ((unsigned char)amplitude2[Y] & 0x0f);
                signed int rect = rp3 * ((unsigned char)amplitude3[Y] & 0x0f);
                signed int mux = sin1 + sin2 + rect;
                mux /= 32;
                mux += 128; // Go from signed to unsigned amplitude
                ary[k] = mux;
                p1 += frequency1[Y] * 256 / 4; // Compromise, this becomes a shift and works well
                p2 += frequency2[Y] * 256 / 4;
                p3 += frequency3[Y] * 256 / 4;
            }
            // output the accumulated value
            Output8BitAry(0, ary);
            speedcounter--;
            if (speedcounter != 0) goto pos48155;
            Y++; //go to next amplitude

            // decrement the frame count
            mem48--;
        }

pos48152:
        // if the frame count is zero, exit the loop
        if(mem48 == 0)  return 0;
        speedcounter = speed;
pos48155:

        // decrement the remaining length of the glottal pulse
        gl_len--;

        // finished with a glottal pulse?
        if(gl_len == 0)
        {
pos48159:
            // fetch the next glottal pulse length
            A = pitches[Y];
            gl_len = sayit_glottal(A);
            gl_third = gl_len - (gl_len >> 2);

            // reset the formant wave generators to keep them in
            // sync with the glottal pulse
            phase1 = 0;
            phase2 = 0;
            phase3 = 0;
            continue;
        }

        // decrement the count
        gl_third--;

        // is the count non-zero and the sampled flag is zero?
        if((gl_third != 0) || (mem39 == 0))
        {
            // reset the phase of the formants to match the pulse
            phase1 += frequency1[Y];
            phase2 += frequency2[Y];
            phase3 += frequency3[Y];
            continue;
        }

        // voiced sampled phonemes interleave the sample with the
        // glottal pulse. The sample flag is non-zero, so render
        // the sample for the phoneme.
        rs_after = 2;
        if (RenderSample(&mem66)) return 1;
        rs_after = 0;
        goto pos48159;
    }
}

// Create a rising or falling inflection 30 frames prior to
// index X. A rising inflection is used for questions, and
// a falling inflection is used for statements.

static void AddInflection(unsigned char mem48, unsigned char phase1)
{
    //pos48372:
    //  mem48 = 255;
//pos48376:

    // store the location of the punctuation
    mem49 = X;
    A = X;
    int Atemp = A;

    // backup 30 frames
    A = A - 30;
    // if index is before buffer, point to start of buffer
    if (Atemp <= 30) A=0;
    X = A;

    // FIXME: Explain this fix better, it's not obvious
    // ML : A =, fixes a problem with invalid pitch with '.'
    while( (A=pitches[X]) == 127) X++;


pos48398:
    //48398: CLC
    //48399: ADC 48

    // add the inflection direction
    A += mem48;
    phase1 = A;

    // set the inflection
    pitches[X] = A;
pos48406:

    // increment the position
    X++;

    // exit if the punctuation has been reached
    if (X == mem49) return; //goto pos47615;
    if (pitches[X] == 255) goto pos48406;
    A = phase1;
    goto pos48398;
}

/*
    SAM's voice can be altered by changing the frequencies of the
    mouth formant (F1) and the throat formant (F2). Only the voiced
    phonemes (5-29 and 48-53) are altered.
*/
static void SetMouthThroat(unsigned char mouth, unsigned char throat)
{
    unsigned char initialFrequency;
    unsigned char newFrequency = 0;
    //unsigned char mouth; //mem38880
    //unsigned char throat; //mem38881

    // mouth formants (F1) 5..29
    unsigned char mouthFormants5_29[30] = {
        0, 0, 0, 0, 0, 10,
        14, 19, 24, 27, 23, 21, 16, 20, 14, 18, 14, 18, 18,
        16, 13, 15, 11, 18, 14, 11, 9, 6, 6, 6};

    // throat formants (F2) 5..29
    unsigned char throatFormants5_29[30] = {
    255, 255,
    255, 255, 255, 84, 73, 67, 63, 40, 44, 31, 37, 45, 73, 49,
    36, 30, 51, 37, 29, 69, 24, 50, 30, 24, 83, 46, 54, 86};

    // there must be no zeros in this 2 tables
    // formant 1 frequencies (mouth) 48..53
    unsigned char mouthFormants48_53[6] = {19, 27, 21, 27, 18, 13};

    // formant 2 frequencies (throat) 48..53
    unsigned char throatFormants48_53[6] = {72, 39, 31, 43, 30, 34};

    unsigned char pos = 5; //mem39216
//pos38942:
    // recalculate formant frequencies 5..29 for the mouth (F1) and throat (F2)
    while(pos != 30)
    {
        // recalculate mouth frequency
        initialFrequency = mouthFormants5_29[pos];
        if (initialFrequency != 0) newFrequency = trans(mouth, initialFrequency);
        freq1data[pos] = newFrequency;

        // recalculate throat frequency
        initialFrequency = throatFormants5_29[pos];
        if(initialFrequency != 0) newFrequency = trans(throat, initialFrequency);
        freq2data[pos] = newFrequency;
        pos++;
    }

//pos39059:
    // recalculate formant frequencies 48..53
    pos = 48;
    Y = 0;
    while(pos != 54)
    {
        // recalculate F1 (mouth formant)
        initialFrequency = mouthFormants48_53[Y];
        newFrequency = trans(mouth, initialFrequency);
        freq1data[pos] = newFrequency;

        // recalculate F2 (throat formant)
        initialFrequency = throatFormants48_53[Y];
        newFrequency = trans(throat, initialFrequency);
        freq2data[pos] = newFrequency;
        Y++;
        pos++;
    }
}


//return = (mem39212*mem39213) >> 1
static unsigned char trans(unsigned char mem39212, unsigned char mem39213)
{
    //pos39008:
    unsigned char carry;
    int temp;
    unsigned char mem39214, mem39215;
    A = 0;
    mem39215 = 0;
    mem39214 = 0;
    X = 8;
    do
    {
        carry = mem39212 & 1;
        mem39212 = mem39212 >> 1;
        if (carry != 0)
        {
            /*
                        39018: LSR 39212
                        39021: BCC 39033
                        */
            carry = 0;
            A = mem39215;
            temp = (int)A + (int)mem39213;
            A = A + mem39213;
            if (temp > 255) carry = 1;
            mem39215 = A;
        }
        temp = mem39215 & 1;
        mem39215 = (mem39215 >> 1) | (carry?128:0);
        carry = temp;
        //39033: ROR 39215
        X--;
    } while (X != 0);
    temp = mem39214 & 128;
    mem39214 = (mem39214 << 1) | (carry?1:0);
    carry = temp;
    temp = mem39215 & 128;
    mem39215 = (mem39215 << 1) | (carry?1:0);
    carry = temp;

    return mem39215;
}


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
