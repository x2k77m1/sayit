/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The planning engine: SAM's text-to-phoneme half with its own state, for the
 * UI task to run ahead of a trigger (sam/sam_api.h, sam/sam_plan_build).
 *
 * It is a copy for the same reason the track engines are copies (sam_inst.h):
 * the port keeps SAM's state at file scope. It is a separate one because this
 * work must never touch a track's engine - a phrase is planned while that
 * track's word is speaking, and only the UI task ever runs here, so nothing
 * races with the audio callback.
 *
 * SAM_PLAN_BUILD compiles in sam_plan_build() alone; the render half of the
 * port is not reached from here. */
#define SAM_SUFFIX _p
#define SAM_PLAN_BUILD 1
#include "sam_inst.h"
#include "sam_port.c"
