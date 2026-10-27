/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Engine 7: the SAM port compiled with its own state, so track 8 keeps its
 * own voice. See sam/sam_inst.h for why the port is copied rather than
 * rewritten, and sam/sam_multi.c for the dispatcher. */
#define SAM_SUFFIX _7
#include "sam_inst.h"
#include "sam_port.c"
