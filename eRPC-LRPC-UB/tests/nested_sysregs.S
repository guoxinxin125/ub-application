/* Test-only system-register encoding tables; no hardware emulation. */
.text
.global ud_test_sysregs
ud_test_sysregs:
#ifdef UD_GATE_VHE
    mrs x0, ttbr0_el2
    mrs x0, tcr_el2
    mrs x0, vbar_el2
    mrs x0, sp_el0
    mrs x0, daif
    mrs x0, elr_el2
    mrs x0, spsr_el2
    mrs x0, esr_el2
    mrs x0, far_el2
#else
    mrs x0, ttbr0_el1
    mrs x0, tcr_el1
    mrs x0, vbar_el1
    mrs x0, sp_el0
    mrs x0, daif
    mrs x0, elr_el1
    mrs x0, spsr_el1
    mrs x0, esr_el1
    mrs x0, far_el1
#endif
