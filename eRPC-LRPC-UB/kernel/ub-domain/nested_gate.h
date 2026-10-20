/* SPDX-License-Identifier: GPL-2.0 */
/* Fixed kernel-owned frame shared with gate_nested.S. No userspace pointers. */
#ifndef UD_NESTED_GATE_H
#define UD_NESTED_GATE_H
#define UDN_MIDDLE 0
#define UDN_LEAF 296
#define UDN_MIDDLE_ROOT 592
#define UDN_LEAF_ROOT 600
#define UDN_MIDDLE_ARGS 608
#define UDN_LEAF_ARGS 616
#define UDN_CALL_PC 624
#define UDN_LEAF_PC 632
#define UDN_RETURN_PC 640
#define UDN_TRANSITIONS 648
#define UDN_COMPLETE 656
#define UDN_DOMAIN 664
#define UDN_MODE 672
#define UDN_SOCIAL_ARGS 680
#define UDN_SOCIAL_TARGET 688
#define UDN_SIZE 696
#define UDN_MODE_MICRO 0
#define UDN_MODE_SOCIAL 1
#define UDN_SOCIAL_ACTION_OFFSET 196
#define UDN_SOCIAL_TARGET_OFFSET 200
#ifndef __ASSEMBLER__
struct ud_nested_gate {
    struct ud_regs middle, leaf;
    u64 middle_root, leaf_root;
    u64 middle_args, leaf_args;
    u64 call_pc, leaf_pc, return_pc;
    u64 transitions, complete, domain;
    u64 mode, social_args, social_target;
};
#endif
#endif
