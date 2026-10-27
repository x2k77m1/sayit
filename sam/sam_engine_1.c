/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Engine 1: the SAM port compiled with its own state, so track 2 keeps its
 * own voice. See sam/sam_inst.h for why the port is copied rather than
 * rewritten, and sam/sam_multi.c for the dispatcher. */
#define SAM_SUFFIX _1
#include "sam_inst.h"
#include "sam_port.c"
