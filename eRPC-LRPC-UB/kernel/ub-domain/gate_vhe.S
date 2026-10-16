/* SPDX-License-Identifier: GPL-2.0 */
/* Deliberately separate object: VHE runs Linux at EL2 and takes EL0 traps
 * through EL2 state, while the original gate remains EL1-only. */
#define UD_GATE_VHE 1
#include "gate.S"
