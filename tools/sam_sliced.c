/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The port compiled the way the machines compile it: SAM_SETUP_SLICED, so a
 * chunk's tables are made a slice a block (sam_port.c, "a chunk's tables, made
 * in slices") and sam_voice_next() is silence until they are ready.
 *
 * The machines reach this state by defining it in mod.json; a host build has
 * no mod.json, so tools/sayit_host_test.py compiles this second copy under the
 * _s names and drives it with the same block hook the machines use. That is
 * what proves the sliced path makes the same stream as the one-shot path, just
 * starting after the silence its setup took.
 *
 * Never a machine's source: nothing in machines/ lists it. */
#define SAM_SUFFIX _s
#define SAM_SETUP_SLICED 1
#include "../sam/sam_inst.h"
#include "../sam/sam_port.c"
