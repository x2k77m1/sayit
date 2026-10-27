/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Which stock Digitone knob carries which SAY IT control, by DNM_KNOBS index:
 * SYN1 A-H are 0-7, SYN2 A-H are 8-15.
 *
 * This is the Digitone machine's half of the layout contract; the Digitakt
 * machine has its own (machines/digitakt/dt_knobs.h), because there the
 * controls are the machine's own SRC-page knobs.
 *
 * The Digitone's eight encoders are two rows of four, A-D above E-H, so a
 * page's cells must sit in the matching half of the screen and the column of
 * their encoder.
 *
 * All of SAY IT's controls are on the lower row (E-G on SYN1, E-F on SYN2):
 * the page then has one band of wide cells and the whole upper half of the
 * screen for the phrase.
 * SYN1 E HARM carries PIT, F DTUN carries SPD, G FDBK carries MODE; SYN2 E
 * and F carry THRT and MOUT. HARM arrives as the stock parameter's own
 * position (37..89 on the kit, 63 is the centre), not as semitones, so PIT
 * maps that position to +/-24 semitones.
 *
 * Keep these equal to engine.c's layout: a function must sit on the knob of
 * the cell that shows it. */
#ifndef SAYIT_DN_KNOBS_H
#define SAYIT_DN_KNOBS_H

#define K_PIT   4               /* SYN1 E, stock HARM, 37..89 */
#define K_SPD   5               /* SYN1 F, stock DTUN 0-127 */
#define K_MODE  6               /* SYN1 G, stock FDBK 0-127 */
#define K_THRT 12               /* SYN2 E, 0-127 */
#define K_MOUT 13               /* SYN2 F, 0-127 */

#endif /* SAYIT_DN_KNOBS_H */
