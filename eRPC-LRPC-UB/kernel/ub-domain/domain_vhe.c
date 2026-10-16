// SPDX-License-Identifier: GPL-2.0
/* Separate module translation unit. Sharing C validation and lifetime logic
 * avoids silently diverging the EL1 and VHE domain protocol. */
#define UD_VHE_GATE 1
#include "domain.c"
