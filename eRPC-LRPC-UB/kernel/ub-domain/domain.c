// SPDX-License-Identifier: GPL-2.0
/* Trusted, bounded bring-up gate. No general thread migration or untrusted B.
 * A's OBMM mapping must remain imported, B immutable, for the whole session. */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/capability.h>
#include <linux/ktime.h>
#include <linux/random.h>
#include <linux/version.h>
#include <linux/irqflags.h>
#include <linux/sched/task.h>
#include <asm/mmu_context.h>
#include <asm/cpufeature.h>
#include <asm/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/sysreg.h>
#include <asm/memory.h>
#include <lrpc/ub_domain_layout.h>
#include <lrpc/ub_domain_services.h>
#include <lrpc/ub_domain_pool.h>
#include <lrpc/ub_remote_code.h>
#include "pool_slots.h"

static DEFINE_MUTEX(ud_pool_lock);
static struct ud_pool_bank ud_pool_banks[UD_POOL_BANKS];

#if !defined(CONFIG_ARM64)
#error "UB domain requires ARM64"
#endif
/* The PFN/VMA adapter below is audited against this API baseline. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0) || LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
#error "Port and audit the OBMM VMA adapter for this kernel; supported API baseline is Linux 6.6"
#endif
struct ud_regs {
    u64 x[31], sp, pc, pstate, esr, far, vector;
};
#include "nested_gate.h"
extern void ud_enter(struct ud_regs *regs, u64 root, u64 tcr,
                     u64 empty_root, unsigned int save_middle);
extern void ud_enter_nested(struct ud_nested_gate *gate, u64 tcr, u64 empty_root);
#ifdef UD_VHE_GATE
extern void ud_enter_vhe(struct ud_regs *regs, u64 root, u64 tcr,
                         u64 empty_root, unsigned int save_middle);
#define ud_enter_selected ud_enter_vhe
extern void ud_enter_nested_vhe(struct ud_nested_gate *gate, u64 tcr, u64 empty_root);
#define ud_enter_nested_selected ud_enter_nested_vhe
#define UD_GATE_MODE "VHE-EL2"
#else
#define ud_enter_selected ud_enter
#define ud_enter_nested_selected ud_enter_nested
#define UD_GATE_MODE "EL1"
#endif
static bool experimental;
module_param(experimental, bool, 0400);
MODULE_PARM_DESC(experimental, "Opt into bounded IRQ-masked EL0 gate on a test machine");
static bool allow_remote_code;
module_param(allow_remote_code, bool, 0400);
MODULE_PARM_DESC(allow_remote_code, "Opt into verified, branch-free B-published direct/nested code");
#ifdef UD_VHE_GATE
static bool vhe_gate;
module_param(vhe_gate, bool, 0400);
MODULE_PARM_DESC(vhe_gate, "Separately opt into the unvalidated VHE EL2 gate");
#endif

struct ud_session {
    struct mutex lock;
    struct mm_struct *mm;
    struct task_struct *owner;
    /* Allocator-only tokens: never installed as a Linux task mm. The audited
     * 6.6 get/put API accesses context.id/pinned only; mm_alloc isn't exported. */
    struct mm_struct *asid_token[UD_DOMAINS];
    unsigned long asid[UD_DOMAINS];
    void *empty_root;
    u64 completion;
    u32 nested_path, profile, code_mode;
    u32 remote_words[UD_DOMAINS];
    u64 pool_generation;
    unsigned int pool_bank, pool_slot;
    bool multi, pool_reserved, pool_exposed, pool_reusable;
    struct ud_prepare p;
    void *code[UD_DOMAINS], *stack[UD_DOMAINS], *args[UD_DOMAINS];
    struct ud_initial initial[UD_DOMAINS];
    struct file *import_file[UD_DOMAINS * 4 + 2];
    bool prepared, committed;
};

/* Prefer Linux's conventional slot, but accept vendor MAIR slot placement. */
static int ud_mair_index(u64 mair, unsigned int encoding, unsigned int preferred)
{
    unsigned int i;
    if (((mair >> (8 * preferred)) & 255) == encoding) return preferred;
    for (i = 0; i < 8; ++i)
        if (((mair >> (8 * i)) & 255) == encoding) return i;
    return -EOPNOTSUPP;
}

static int ud_supported(void)
{
    u64 tcr = read_sysreg(tcr_el1), mair = read_sysreg(mair_el1);
#ifdef UD_VHE_GATE
    u64 hcr;
    if (read_sysreg(CurrentEL) != 8) {
        pr_warn_once("ub_domain_vhe: EL2 host kernel required\n");
        return -EOPNOTSUPP;
    }
    hcr = read_sysreg(hcr_el2);
    /* E2H=bit34, TGE=bit27: host EL0 must use the EL2&0 regime.
     * Linux's *_EL1 MMU accesses are redirected by VHE in this mode. */
    if ((hcr & (BIT_ULL(34) | BIT_ULL(27))) !=
        (BIT_ULL(34) | BIT_ULL(27))) {
        pr_warn_once("ub_domain_vhe: HCR_EL2 E2H=1 and TGE=1 required\n");
        return -EOPNOTSUPP;
    }
    if (!vhe_gate) {
        pr_warn_once("ub_domain_vhe: load with vhe_gate=1 after EL2 gate audit\n");
        return -EOPNOTSUPP;
    }
#else
    if (read_sysreg(CurrentEL) != 4) {
        pr_warn_once("ub_domain: EL1 kernel required; VHE EL2 gate is not supported\n");
        return -EOPNOTSUPP;
    }
#endif
    if (!experimental) {
        pr_warn_once("ub_domain: load with experimental=1 on the test host\n");
        return -EOPNOTSUPP;
    }
    /* CONFIG alone is capability, not activation. Use Linux's runtime CPU
     * feature decision, matching the local_irq_* PMR/DAIF selection. */
    if (system_uses_irq_prio_masking()) {
        pr_warn_once("ub_domain: runtime GIC priority masking is active; PMR/pseudo-NMI gate unsupported\n");
        return -EOPNOTSUPP;
    }
    if (IS_ENABLED(CONFIG_KASAN) || IS_ENABLED(CONFIG_KCSAN)) {
        pr_warn_once("ub_domain: instrumented KASAN/KCSAN kernel unsupported by this gate\n");
        return -EOPNOTSUPP;
    }
    /* Shadow uses four-level 4K tables independently of Linux PAGE_SIZE.
     * TTBR1 keeps the host granule. Require hardware support for TG0=4K. */
    if (((read_sysreg(id_aa64mmfr0_el1) >> 28) & 15) > 1) {
        pr_warn_once("ub_domain: hardware does not support the required 4K TG0 granule\n");
        return -EOPNOTSUPP;
    }
    if (((tcr >> 32) & 7) > 5 || (tcr & (1ULL << 59))) {
        pr_warn_once("ub_domain: TCR=%llx needs >48-bit/LPA2 descriptor support\n",
                     (unsigned long long)tcr);
        return -EOPNOTSUPP;
    }
    if (ud_mair_index(mair, 0xff, MT_NORMAL) < 0 ||
        ud_mair_index(mair, 0x44, MT_NORMAL_NC) < 0) {
        pr_warn_once("ub_domain: MAIR=%llx lacks Normal-WB(0xff)/Normal-NC(0x44) slots\n",
                     (unsigned long long)mair);
        return -EOPNOTSUPP;
    }
    return 0;
}

/* Caller holds mmap_read_lock. Never accept a raw PA supplied by userspace.
 * The mapping must actually expose this PFN through the live OBMM VMA. */
static int ud_import_pa(struct ud_session *s, unsigned long offset, u64 *pa,
                        unsigned int slot, bool initialize)
{
    unsigned long va = s->p.import_va + offset;
    struct vm_area_struct *vma = find_vma(current->mm, va);
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *ptep, pte;
    spinlock_t *ptl;
    const char *name;
    int rc;
    if (!vma || va < vma->vm_start || vma->vm_end - va < UD_PAGE ||
        !(vma->vm_flags & (VM_IO | VM_PFNMAP)) || !vma->vm_file ||
        !(vma->vm_flags & VM_SHARED) || !(vma->vm_flags & VM_READ)) return -EINVAL;
    name = (const char *)vma->vm_file->f_path.dentry->d_name.name;
    if (strncmp(name, "obmm_shmdev", 11)) return -EXDEV;
    if (!initialize && vma->vm_file != s->import_file[slot]) return -ESTALE;
    /* follow_pte expects small PTE mappings. Refuse huge/block mappings
     * explicitly before its PMD assertion. Inspect real PTE attributes. */
    pgd = pgd_offset(current->mm, va);
    if (pgd_none(*pgd) || pgd_bad(*pgd)) return -EINVAL;
    p4d = p4d_offset(pgd, va);
    if (p4d_none(*p4d) || p4d_bad(*p4d)) return -EINVAL;
    pud = pud_offset(p4d, va);
    if (pud_none(*pud) || pud_bad(*pud) || pud_sect(*pud)) return -EOPNOTSUPP;
    pmd = pmd_offset(pud, va);
    if (pmd_none(*pmd) || pmd_bad(*pmd) || pmd_sect(*pmd)) return -EOPNOTSUPP;
    rc = follow_pte(current->mm, va, &ptep, &ptl);
    if (rc) return rc;
    pte = READ_ONCE(*ptep);
    *pa = ((u64)pte_pfn(pte) << PAGE_SHIFT) + offset_in_page(va);
    rc = (((read_sysreg(mair_el1) >>
            (8 * ((pte_val(pte) & PTE_ATTRINDX_MASK) >> 2))) & 255) == 0x44 &&
          (pte_val(pte) & (PTE_VALID | PTE_USER)) == (PTE_VALID | PTE_USER)) ? 0 : -EOPNOTSUPP;
    pte_unmap_unlock(ptep, ptl);
    if (rc) {
        pr_warn_ratelimited("ub_domain: import PTE must be valid EL0 Normal-NC (offset=%lx)\n", offset);
        return rc;
    }
    if ((s->profile == UD_PROFILE_SOCIAL_WRITE ||
         s->profile == UD_PROFILE_SOCIAL_SHARED) &&
        offset == (s->profile == UD_PROFILE_SOCIAL_SHARED ?
                   UD_SHARED_HEAP : UD_WRITE_HEAP(s->pool_slot)) &&
        (!(vma->vm_flags & VM_WRITE) || !pte_write(pte))) return -EACCES;
    if (!ud_pa_valid(*pa)) return -ERANGE;
    if (initialize) s->import_file[slot] = get_file(vma->vm_file);
    return 0;
}

static int ud_mapping(struct ud_session *s, bool initialize)
{
    unsigned int d, l;
    u64 pa;
    int rc;
    for (d = 0; d < UD_DOMAINS; ++d) for (l = 0; l < 4; ++l) {
        unsigned long offset = s->multi ? UD_POOL_TABLE(s->pool_slot,d,l) : UD_TABLE(d,l);
        rc = ud_import_pa(s, offset, &pa, d*4+l, initialize);
        if (rc) return rc;
        if (initialize) s->p.contract.table_pa[d][l] = pa;
        else if (s->p.contract.table_pa[d][l] != pa) return -ESTALE;
    }
    rc = ud_import_pa(s, UD_HEAP, &pa, UD_DOMAINS*4, initialize);
    if (rc) return rc;
    if (initialize) s->p.contract.heap_pa = pa;
    else if (s->p.contract.heap_pa != pa) return -ESTALE;
    if (s->profile == UD_PROFILE_SOCIAL_WRITE ||
        s->profile == UD_PROFILE_SOCIAL_SHARED) {
        rc = ud_import_pa(s, s->profile == UD_PROFILE_SOCIAL_SHARED ?
                          UD_SHARED_HEAP : UD_WRITE_HEAP(s->pool_slot), &pa,
                          UD_DOMAINS*4+1, initialize);
        if (rc) return rc;
        if (initialize) s->p.contract.write_pa = pa;
        else if (s->p.contract.write_pa != pa) return -ESTALE;
    }
    return 0;
}

static void ud_free_pages(struct ud_session *s)
{
    unsigned int d;
    /* Remove translations on all CPUs before local backing/ASIDs can be reused.
     * This is connection teardown, never the per-call path. */
    for (d = 0; d < UD_DOMAINS; ++d) {
        if (s->asid[d]) {
            u64 operand = (u64)s->asid[d] << 48;
            asm volatile("dsb ishst; tlbi aside1is, %0; dsb ish; isb"
                         : : "r"(operand) : "memory");
            arm64_mm_context_put(s->asid_token[d]);
            s->asid[d] = 0;
        }
        kfree(s->asid_token[d]);
        s->asid_token[d] = NULL;
    }
    if (s->empty_root) free_page((unsigned long)s->empty_root);
    s->empty_root = NULL;
    for (d = 0; d < UD_DOMAINS; ++d) {
        if (s->code[d]) free_page((unsigned long)s->code[d]);
        if (s->stack[d]) free_page((unsigned long)s->stack[d]);
        if (s->args[d]) free_page((unsigned long)s->args[d]);
        s->code[d] = s->stack[d] = s->args[d] = NULL;
    }
    for (d = 0; d < ARRAY_SIZE(s->import_file); ++d) {
        if (s->import_file[d]) fput(s->import_file[d]);
        s->import_file[d] = NULL;
    }
    if (s->pool_reserved) {
        /* A pending publication can still be handled by B after local failure.
         * Never recycle that slot into an active connection. */
        mutex_lock(&ud_pool_lock);
        ud_slots_release(ud_pool_banks, s->pool_bank, s->pool_slot,
                         s->pool_exposed, s->pool_reusable);
        mutex_unlock(&ud_pool_lock);
        if (s->pool_exposed && !s->pool_reusable)
            pr_warn("ub_domain: quarantined pool slot=%u after incomplete prepare; restart fresh owner/module after all clients stop\n",
                    s->pool_slot);
        s->pool_reserved = false;
    }
}
static int ud_prepare_pages(struct ud_session *s, const struct ud_prepare *input)
{
    unsigned int d;
    int rc;
    if (s->prepared) return -EALREADY;
    if (!input->import_va || input->import_va & (UD_PAGE - 1) ||
        input->import_va > TASK_SIZE - UD_BYTES) return -EINVAL;
    memset(&s->p, 0, sizeof(s->p));
    s->p.import_va = input->import_va;
    s->p.contract.magic = UD_MAGIC;
    s->p.contract.abi = UD_ABI;
    s->p.contract.nonce = get_random_u64() | 1;
    rc = ud_supported();
    if (rc) return rc;
    s->p.contract.wb_index = ud_mair_index(read_sysreg(mair_el1), 0xff, MT_NORMAL);
    s->p.contract.nc_index = ud_mair_index(read_sysreg(mair_el1), 0x44, MT_NORMAL_NC);
    mmap_read_lock(current->mm);
    if (s->multi) {
        /* Discover the real A-view region identity before choosing offsets. */
        u64 heap;
        rc = ud_import_pa(s, UD_HEAP, &heap, UD_DOMAINS*4, true);
        if (!rc) {
            mutex_lock(&ud_pool_lock);
            rc = ud_slots_acquire(ud_pool_banks, heap, s->pool_generation,
                                  &s->pool_bank, &s->pool_slot);
            mutex_unlock(&ud_pool_lock);
            if (!rc) s->pool_reserved = true;
        }
        if (rc) { mmap_read_unlock(current->mm); goto fail; }
        /* Discovery's file reference is replaced by ud_mapping below. */
        if (s->import_file[UD_DOMAINS*4]) {
            fput(s->import_file[UD_DOMAINS*4]); s->import_file[UD_DOMAINS*4] = NULL;
        }
    }
    rc = ud_mapping(s, true);
    mmap_read_unlock(current->mm);
    if (rc) goto fail;
    s->empty_root = (void *)get_zeroed_page(GFP_KERNEL);
    if (!s->empty_root) { rc = -ENOMEM; goto fail; }
    for (d = 0; d < UD_DOMAINS; ++d) {
        u64 operand;
        s->asid_token[d] = kzalloc(sizeof(struct mm_struct), GFP_KERNEL);
        if (!s->asid_token[d]) { rc = -ENOMEM; goto fail; }
        init_new_context(NULL, s->asid_token[d]);
        s->asid[d] = arm64_mm_context_get(s->asid_token[d]);
        if (!s->asid[d]) { rc = -ENOSPC; goto fail; }
        /* get() reserves against Linux rollover; it does not flush old TLBs. */
        operand = (u64)s->asid[d] << 48;
        asm volatile("dsb ishst; tlbi aside1is, %0; dsb ish; isb"
                     : : "r"(operand) : "memory");
        s->code[d] = (void *)get_zeroed_page(GFP_KERNEL);
        s->stack[d] = (void *)get_zeroed_page(GFP_KERNEL);
        s->args[d] = (void *)get_zeroed_page(GFP_KERNEL);
        if (!s->code[d] || !s->stack[d] || !s->args[d]) {
            rc = -ENOMEM; goto fail;
        }
        s->p.contract.code_pa[d] = virt_to_phys(s->code[d]);
        s->p.contract.stack_pa[d] = virt_to_phys(s->stack[d]);
        /* Routing domains share one connection-private A-stack, so the route
         * handoff needs neither a payload copy nor a remote argument access. */
        s->p.contract.args_pa[d] = virt_to_phys(s->args[s->profile != UD_PROFILE_MICRO ? 0 : d]);
    }
    if (!ud_contract_valid(&s->p.contract)) { rc = -ERANGE; goto fail; }
    return 0;
fail:
    ud_free_pages(s);
    return rc;
}
static int ud_prepare_session(struct ud_session *s, void __user *arg)
{
    struct ud_prepare input;
    struct ud_remote_manifest remote = {0};
    u64 magic;
    int rc;
    if (s->prepared) return -EALREADY;
    if (s->profile != UD_PROFILE_MICRO) return -EPROTONOSUPPORT;
    if (copy_from_user(&input, arg, sizeof(input))) return -EFAULT;
    if (!input.import_va || input.import_va > TASK_SIZE - UD_BYTES) return -EINVAL;
    if (copy_from_user(&magic, (void __user *)input.import_va, sizeof(magic))) return -EFAULT;
    if (magic == UD_POOL_MAGIC) return -EPROTONOSUPPORT;
    if (copy_from_user(&remote, (void __user *)(input.import_va + UD_REMOTE_META_OFFSET),
                       sizeof(remote))) return -EFAULT;
    if (s->code_mode == UD_CODE_REMOTE_VERIFIED) {
        if (!allow_remote_code || remote.magic != UD_REMOTE_MAGIC ||
            remote.abi != UD_REMOTE_ABI || remote.mode != UD_CODE_REMOTE_VERIFIED ||
            remote.reserved) return -EPROTONOSUPPORT;
    } else if (remote.magic || remote.abi || remote.mode || remote.reserved) {
        return -EPROTONOSUPPORT;
    }
    s->multi = false;
    rc = ud_prepare_pages(s, &input);
    if (rc) return rc;
    if (copy_to_user(arg, &s->p, sizeof(s->p))) { ud_free_pages(s); return -EFAULT; }
    s->prepared = true;
    return 0;
}
static int ud_prepare_pool(struct ud_session *s, void __user *arg)
{
    struct ud_pool_prepare input;
    struct ud_pool_header header;
    struct ud_profile_header tag;
    int rc;
    if (s->prepared) return -EALREADY;
    if (copy_from_user(&input, arg, sizeof(input))) return -EFAULT;
    if (input.abi != UD_POOL_ABI || !input.generation || input.slot ||
        !input.domain.import_va || input.domain.import_va > TASK_SIZE-UD_BYTES) return -EINVAL;
    if (copy_from_user(&header, (void __user *)input.domain.import_va, sizeof(header))) return -EFAULT;
    if (!ud_pool_header_valid(&header) || header.generation != input.generation) return -ESTALE;
    if (copy_from_user(&tag, (void __user *)(input.domain.import_va+UD_PROFILE_OFFSET), sizeof(tag))) return -EFAULT;
    if (tag.magic == UD_PROFILE_MAGIC) {
        if (tag.abi != UD_POOL_ABI || tag.profile != s->profile) return -EPROTONOSUPPORT;
    } else if (s->profile) return -EPROTONOSUPPORT;
    s->multi = true; s->pool_generation = input.generation;
    s->pool_exposed = s->pool_reusable = false;
    rc = ud_prepare_pages(s, &input.domain);
    if (rc) return rc;
    input.domain = s->p; input.slot = s->pool_slot;
    if (copy_to_user(arg, &input, sizeof(input))) { ud_free_pages(s); return -EFAULT; }
    s->prepared = s->pool_exposed = true;
    return 0;
}

static int ud_copy(struct ud_session *s, unsigned long off, void *to, size_t n)
{
    return copy_from_user(to, (void __user *)(s->p.import_va + off), n) ? -EFAULT : 0;
}
static int ud_commit(struct ud_session *s)
{
    struct ud_publication pub;
    struct ud_remote_manifest remote = {0};
    u64 *expected, *actual;
    unsigned int d, l;
    unsigned long size;
    int rc = -ENOMEM;
    if (!s->prepared || s->committed) return -EINVAL;
    expected = kmalloc(UD_PAGE, GFP_KERNEL);
    actual = kmalloc(UD_PAGE, GFP_KERNEL);
    if (!expected || !actual) goto out;
    if (s->multi) {
        struct ud_pool_header header;
        struct ud_pool_publication slot;
        rc = ud_copy(s, 0, &header, sizeof(header));
        if (rc) goto out;
        if (!ud_pool_header_valid(&header) || header.generation != s->pool_generation) {
            rc = -ESTALE; goto out;
        }
        rc = ud_copy(s, UD_POOL_SLOT(s->pool_slot), &slot, sizeof(slot));
        if (rc) goto out;
        if (slot.error || slot.reserved) { rc = -EBADMSG; goto out; }
        pub = slot.pub;
    } else rc = ud_copy(s, 0, &pub, sizeof(pub));
    if (rc) goto out;
    if (memcmp(&pub.contract, &s->p.contract, sizeof(pub.contract)) ||
        pub.request != s->p.contract.nonce || pub.complete != pub.request) {
        rc = -ESTALE; goto out;
    }
    if (s->code_mode == UD_CODE_REMOTE_VERIFIED) {
        rc = ud_copy(s, UD_REMOTE_META_OFFSET, &remote, sizeof(remote));
        if (rc) goto out;
        if (remote.magic != UD_REMOTE_MAGIC || remote.abi != UD_REMOTE_ABI ||
            remote.mode != UD_CODE_REMOTE_VERIFIED || remote.reserved) {
            rc = -EBADMSG; goto out;
        }
    }
    for (d = 0; d < UD_DOMAINS; ++d) {
        struct ud_initial initial = {UD_ARGS, UD_DATA, UD_STACK + UD_PAGE, UD_VA};
        const unsigned char *code = NULL;
        if (s->code_mode == UD_CODE_REMOTE_VERIFIED) {
            if (!remote.words[d] || remote.words[d] > UD_REMOTE_MAX_WORDS) {
                rc = -EINVAL; goto out;
            }
            size = remote.words[d] * sizeof(u32);
        } else code = ud_profile_service(s->profile, d, &size);
        if (s->profile != UD_PROFILE_MICRO) initial.heap += us_config_offset(d);
        for (l = 0; l < 4; ++l) {
            ud_build_table(&s->p.contract, d, l, expected);
            rc = ud_copy(s, s->multi ? UD_POOL_TABLE(s->pool_slot,d,l) : UD_TABLE(d,l), actual, UD_PAGE);
            if (rc) goto out;
            if (memcmp(expected, actual, UD_PAGE)) { rc = -EBADMSG; goto out; }
        }
        rc = ud_copy(s, UD_CONTEXT(d), &s->initial[d], sizeof(initial));
        if (rc) goto out;
        if (memcmp(&initial, &s->initial[d], sizeof(initial))) { rc = -EBADMSG; goto out; }
        if (!size || size > UD_PAGE) { rc = -EINVAL; goto out; }
        rc = ud_copy(s, UD_IMAGE(d), s->code[d], size);
        if (rc) goto out;
        if (s->code_mode == UD_CODE_REMOTE_VERIFIED) {
            if (!ud_remote_image_valid(d, s->code[d], remote.words[d])) {
                rc = -EBADMSG; goto out;
            }
            s->remote_words[d] = remote.words[d];
        } else if (memcmp(code, s->code[d], size)) { rc = -EBADMSG; goto out; }
        flush_icache_range((unsigned long)s->code[d], (unsigned long)s->code[d] + size);
    }
    rc = ud_copy(s, UD_HEAP, actual, UD_PAGE);
    if (rc) goto out;
    if (s->profile != UD_PROFILE_MICRO) {
        if (!us_heap_valid((void *)actual)) { rc = -EBADMSG; goto out; }
    } else {
        if (actual[0] != 100) { rc = -EBADMSG; goto out; }
        for (d = 0; d < 32; ++d)
            if (actual[8 + d*8] != d || actual[9 + d*8] != ((d+1) & 31) ||
                actual[10 + d*8] != 1000+d) { rc = -EBADMSG; goto out; }
    }
    s->committed = true;
    s->pool_reusable = true;
    rc = 0;
out:
    kfree(expected); kfree(actual);
    return rc;
}

static void ud_initial_regs(struct ud_session *s, unsigned int d, struct ud_regs *r)
{
    memset(r, 0, sizeof(*r));
    r->x[0] = s->initial[d].args; r->x[1] = s->initial[d].heap;
    r->sp = s->initial[d].sp; r->pc = s->initial[d].pc;
}
static bool ud_returned(struct ud_session *s, struct ud_regs *r,
                        unsigned int d, bool nested)
{
    unsigned long size;
    if (s->code_mode == UD_CODE_REMOTE_VERIFIED)
        size = s->remote_words[d] * sizeof(u32);
    else ud_service(d, &size);
    return r->vector == 8 && (r->esr >> 26) == 0x3c &&
        (r->esr & 0xffff) == (nested ? 0x4c1 : 0x4c0) &&
        r->pc == UD_VA + (nested ? UD_NESTED_TRAP_OFFSET : size-4);
}
/* All frame pointers and PAs are prepared by A's kernel. The assembly owns
 * both inter-domain handoffs; Linux C runs again only after A is restored. */
static noinline int ud_run_fused(struct ud_session *s, struct ud_regs *result,
                                 u64 tcr, struct ud_call *call)
{
    struct ud_nested_gate gate;
    unsigned long middle_size, leaf_size;
    ud_initial_regs(s, UD_MIDDLE, &gate.middle);
    ud_initial_regs(s, UD_LEAF, &gate.leaf);
    if (s->code_mode == UD_CODE_REMOTE_VERIFIED) {
        middle_size = s->remote_words[UD_MIDDLE] * sizeof(u32);
        leaf_size = s->remote_words[UD_LEAF] * sizeof(u32);
    } else {
        ud_service(UD_MIDDLE, &middle_size);
        ud_service(UD_LEAF, &leaf_size);
    }
    gate.middle_root = s->p.contract.table_pa[UD_MIDDLE][0] |
                       ((u64)s->asid[UD_MIDDLE] << 48);
    gate.leaf_root = s->p.contract.table_pa[UD_LEAF][0] |
                     ((u64)s->asid[UD_LEAF] << 48);
    gate.middle_args = (u64)s->args[UD_MIDDLE];
    gate.leaf_args = (u64)s->args[UD_LEAF];
    gate.call_pc = UD_VA + UD_NESTED_TRAP_OFFSET;
    gate.leaf_pc = UD_VA + leaf_size - 4;
    gate.return_pc = UD_VA + middle_size - 4;
    gate.transitions = gate.complete = 0;
    gate.domain = UD_MIDDLE;
    gate.mode = UDN_MODE_MICRO;
    ud_enter_nested_selected(&gate, tcr, virt_to_phys(s->empty_root));
    call->transitions = gate.transitions;
    *result = gate.domain == UD_LEAF ? gate.leaf : gate.middle;
    return gate.complete == 1 && gate.transitions == 4 &&
           gate.domain == UD_MIDDLE && ud_returned(s, result, UD_MIDDLE, false) ? 0 : -EIO;
}
static int ud_run(struct ud_session *s, struct ud_call *c, bool mapped,
                  struct ud_trace_call *trace)
{
    struct ud_regs r, leaf;
    unsigned int d = c->procedure == 1 ? UD_DIRECT :
                     c->procedure == 2 ? UD_MIDDLE : UD_QUERY;
    unsigned long flags;
    u64 tcr, start;
#ifdef UD_TRACE_ENABLE
    u64 stamp = 0;
#else
    (void)trace;
#endif
    int rc;
    if (s->profile != UD_PROFILE_MICRO) return -EOPNOTSUPP;
    if (!s->committed || c->procedure < 1 || c->procedure > 3 || c->reserved ||
        (s->code_mode == UD_CODE_REMOTE_VERIFIED && c->procedure == 3) ||
        (c->procedure == 3 && (!c->a || c->a > 32))) return -EINVAL;
    /* Read lock spans entry/return; munmap/mremap cannot replace the VMA.
     * External UB teardown is forbidden by the trusted session contract. */
    mmap_read_lock(current->mm);
    rc = ud_mapping(s, false);
    if (rc) goto unlock;
    if (!mapped) {
        ((u64 *)s->args[d])[0] = c->a; ((u64 *)s->args[d])[1] = c->b;
    }
    ((u64 *)s->args[d])[2] = 0;
    if (d != UD_MIDDLE || s->nested_path != UD_NESTED_FUSED)
        ud_initial_regs(s, d, &r);
    preempt_disable();
    local_irq_save(flags);
    rc = ud_supported();
    if (rc) goto restore;
    c->cpu = raw_smp_processor_id();
    /* NC table walks, inner shareable; shadow ASID comes from TTBR0 (A1=0).
     * Every root owns a Linux-pinned ASID, so switching doesn't flush TLB. */
    tcr = read_sysreg(tcr_el1);
    tcr &= ~((15ULL << 8) | (1ULL << 7) | (3ULL << 12) |
             (3ULL << 39) | (3ULL << 14) | (1ULL << 22) | 63ULL);
    tcr |= (3ULL << 12) | 16ULL;
    start = ktime_get_ns();
#ifdef UD_TRACE_ENABLE
    if (trace) stamp = start;
#endif
    if (d == UD_MIDDLE && s->nested_path == UD_NESTED_FUSED) {
        rc = ud_run_fused(s, &r, tcr, c);
#ifdef UD_TRACE_ENABLE
        /* One complete fused chain. No fabricated per-domain timestamps. */
        if (trace) {
            trace->segment_ns[0] = ktime_get_ns() - stamp;
            trace->segments = 1;
        }
#endif
        if (rc) goto bad;
        goto returned;
    }
    ud_enter_selected(&r, s->p.contract.table_pa[d][0] | ((u64)s->asid[d] << 48),
             tcr, virt_to_phys(s->empty_root), d == UD_MIDDLE);
#ifdef UD_TRACE_ENABLE
    if (trace) {
        u64 end = ktime_get_ns();
        trace->segment_ns[0] = end - stamp;
        trace->segments = 1;
        stamp = end;
    }
#endif
    c->transitions = 2;
    if (d == UD_MIDDLE && ud_returned(s, &r, d, true)) {
        memset(s->args[UD_LEAF], 0, 3 * sizeof(u64));
        ((u64 *)s->args[UD_LEAF])[0] = r.x[2];
        ud_initial_regs(s, UD_LEAF, &leaf);
#ifdef UD_TRACE_ENABLE
        if (trace) {
            u64 begin = ktime_get_ns();
            trace->handoff_ns[0] = begin - stamp;
            stamp = begin;
        }
#endif
        ud_enter_selected(&leaf, s->p.contract.table_pa[UD_LEAF][0] |
                 ((u64)s->asid[UD_LEAF] << 48), tcr,
                 virt_to_phys(s->empty_root), 0);
#ifdef UD_TRACE_ENABLE
        if (trace) {
            u64 end = ktime_get_ns();
            trace->segment_ns[1] = end - stamp;
            trace->segments = 2;
            stamp = end;
        }
#endif
        c->transitions += 2;
        if (!ud_returned(s, &leaf, UD_LEAF, false)) { r = leaf; goto bad; }
        r.pc += 4;
        r.x[2] = ((u64 *)s->args[UD_LEAF])[2];
#ifdef UD_TRACE_ENABLE
        if (trace) {
            u64 begin = ktime_get_ns();
            trace->handoff_ns[1] = begin - stamp;
            stamp = begin;
        }
#endif
        ud_enter_selected(&r, s->p.contract.table_pa[d][0] | ((u64)s->asid[d] << 48),
                 tcr, virt_to_phys(s->empty_root), 0);
#ifdef UD_TRACE_ENABLE
        if (trace) {
            trace->segment_ns[2] = ktime_get_ns() - stamp;
            trace->segments = 3;
        }
#endif
        c->transitions += 2;
    }
returned:
    if (!ud_returned(s, &r, d, false)) goto bad;
    c->result = ((u64 *)s->args[d])[2];
    c->esr = c->far = 0;
    rc = 0;
    goto timed;
bad:
    c->esr = r.esr; c->far = r.far;
    s->committed = false;
    rc = -EIO;
timed:
    c->elapsed_ns = ktime_get_ns() - start;
restore:
    local_irq_restore(flags);
    preempt_enable();
unlock:
    mmap_read_unlock(current->mm);
    return rc;
}
/* Comparison helper: invalidate only this session's service ASIDs. The
 * caller invokes two identical calls; TLBI itself is outside both timings. */
static int ud_tlb_probe(struct ud_session *s, void __user *arg)
{
    struct ud_tlb_probe p;
    struct ud_call first = {0}, second = {0};
    unsigned int d, count;
    int rc;
    if (!s->committed) return -ENOTCONN;
    if (copy_from_user(&p, arg, sizeof(p))) return -EFAULT;
    if ((p.procedure != 1 && p.procedure != 2) || p.flush > 1)
        return -EINVAL;
    p.first_ns = p.second_ns = p.first_result = p.second_result = 0;
    p.first_esr = p.first_far = p.second_esr = p.second_far = 0;
    p.first_cpu = p.second_cpu = 0;
    if (p.flush) {
        count = p.procedure == 2 ? 2 : 1;
        for (d = 0; d < count; ++d) {
            unsigned int domain = d ? UD_LEAF :
                                  p.procedure == 2 ? UD_MIDDLE : UD_DIRECT;
            u64 operand = (u64)s->asid[domain] << 48;
            asm volatile("dsb ishst; tlbi aside1is, %0; dsb ish; isb"
                         : : "r"(operand) : "memory");
        }
    }
    first.a = second.a = p.a;
    first.b = second.b = p.b;
    first.procedure = second.procedure = p.procedure;
    rc = ud_run(s, &first, false, NULL);
    p.first_ns = first.elapsed_ns;
    p.first_result = first.result;
    p.first_esr = first.esr; p.first_far = first.far;
    p.first_cpu = first.cpu;
    if (!rc) {
        rc = ud_run(s, &second, false, NULL);
        p.second_ns = second.elapsed_ns;
        p.second_result = second.result;
        p.second_esr = second.esr; p.second_far = second.far;
        p.second_cpu = second.cpu;
        if (!rc && first.cpu != second.cpu) rc = -EXDEV;
    }
    if (copy_to_user(arg, &p, sizeof(p))) return -EFAULT;
    return rc;
}
/* Routing chain: legacy A->LB->A->nginx->A, or fused A->LB->nginx->A.
 * The reverse chain visits nginx before LB. Each call enters two domains.
 * B config is RO, code is A-local RX, arguments are A-local shared WB.
 * The opt-in write profiles map a private or shared B data page RW/NC.
 * No eRPC, allocator, syscall or data-plane B thread in a domain. */
static int ud_run_social(struct ud_session *s)
{
    struct ud_mapped *m = s->args[0];
    struct us_call *call = (void *)((char *)s->args[0]+US_MAPPED_OFFSET);
    struct us_message input;
    struct ud_regs r;
    unsigned int first, i;
    unsigned long flags, size;
    u64 tcr, start;
    int rc;
    if (call->abi != US_ABI || call->direction > US_REVERSE ||
        !call->message.source || call->message.source > U32_MAX ||
        !call->message.value || call->message.value > U32_MAX || call->message.request > U32_MAX ||
        (call->message.type != (call->direction ? US_USER_RESP : US_USER_REQ) &&
         call->message.type != (call->direction ? US_HOME_RESP : US_HOME_REQ) &&
         !(s->profile == UD_PROFILE_SOCIAL_WRITE &&
           call->direction == US_FORWARD && call->message.type == US_WRITE_REQ) &&
         !(s->profile == UD_PROFILE_SOCIAL_SHARED && call->direction == US_FORWARD &&
           (call->message.type == US_SHARED_WRITE_REQ ||
            call->message.type == US_SHARED_READ_REQ ||
            call->message.type == US_SHARED_FETCH_ADD_REQ))) ||
        (s->profile == UD_PROFILE_SOCIAL_SHARED &&
         ((call->message.type == US_SHARED_WRITE_REQ &&
           call->message.request != US_SHARED_VALUE_INDEX) ||
          (call->message.type == US_SHARED_READ_REQ &&
           call->message.request != US_SHARED_VALUE_INDEX &&
           call->message.request != US_SHARED_COUNTER_INDEX) ||
          (call->message.type == US_SHARED_FETCH_ADD_REQ &&
           (call->message.request != US_SHARED_COUNTER_INDEX ||
            call->message.value != 1)))) ||
        call->message.status || call->message.action || call->message.target || call->message.ticket)
        return -EINVAL;
    input = call->message;
    first = call->direction == US_FORWARD ? 0 : 2;
    call->entries = call->transitions = call->fault_vector = 0;
    call->domain[0] = call->domain[1] = 0;
    m->esr = m->far = m->elapsed_ns = 0;
    mmap_read_lock(current->mm);
    rc = ud_mapping(s, false);
    if (rc) goto unlock;
    preempt_disable(); local_irq_save(flags);
    rc = ud_supported();
    if (rc) goto restore;
    tcr = read_sysreg(tcr_el1);
    tcr &= ~((15ULL << 8) | (1ULL << 7) | (3ULL << 12) |
             (3ULL << 39) | (3ULL << 14) | (1ULL << 22) | 63ULL);
    tcr |= (3ULL << 12) | 16ULL;
    call->cpu = raw_smp_processor_id();
    start = ktime_get_ns();
    if (s->nested_path == UD_NESTED_FUSED) {
        struct ud_nested_gate gate = {0};
        unsigned long second_size;
        ud_initial_regs(s, first, &gate.middle);
        ud_initial_regs(s, first+1, &gate.leaf);
        gate.middle.x[0] += US_MAPPED_OFFSET;
        gate.leaf.x[0] += US_MAPPED_OFFSET;
        ud_profile_service(s->profile, first, &size);
        ud_profile_service(s->profile, first+1, &second_size);
        gate.middle_root = s->p.contract.table_pa[first][0] |
                           ((u64)s->asid[first] << 48);
        gate.leaf_root = s->p.contract.table_pa[first+1][0] |
                         ((u64)s->asid[first+1] << 48);
        gate.call_pc = UD_VA + size - 4;
        gate.leaf_pc = UD_VA + second_size - 4;
        gate.mode = UDN_MODE_SOCIAL;
        gate.social_args = (u64)s->args[0];
        gate.social_target = first ? 1 : 2;
        ud_enter_nested_selected(&gate, tcr, virt_to_phys(s->empty_root));
        call->transitions = gate.transitions;
        r = gate.domain == 2 ? gate.leaf : gate.middle;
        if (gate.complete != 1 || gate.transitions != 3 || gate.domain != 2 ||
            r.vector != 8 || (r.esr >> 26) != 0x3c ||
            (r.esr & 0xffff) != 0x4c0 || r.pc != gate.leaf_pc) {
            rc = -EIO; goto bad;
        }
        call->domain[0] = first ? 2 : 1;
        call->domain[1] = first ? 1 : 2;
        call->entries = 2;
        if (call->message.action != (first ? US_ACTION_CLIENT : US_ACTION_RPC) ||
            call->message.target != (first ? 0 :
                (input.type == US_WRITE_REQ ? 104 :
                 (input.type == US_SHARED_WRITE_REQ ||
                  input.type == US_SHARED_READ_REQ ||
                  input.type == US_SHARED_FETCH_ADD_REQ) ? 105 :
                 input.type == US_HOME_REQ ? 103 : 102))) {
            rc = -EPROTO; goto bad;
        }
    } else for (i = 0; i < 2; ++i) {
        unsigned int d = first+i;
        ud_initial_regs(s, d, &r); r.x[0] += US_MAPPED_OFFSET;
        ud_profile_service(s->profile, d, &size);
        ud_enter_selected(&r, s->p.contract.table_pa[d][0] | ((u64)s->asid[d] << 48),
                          tcr, virt_to_phys(s->empty_root), 0);
        call->transitions += 2;
        if (r.vector != 8 || (r.esr >> 26) != 0x3c || (r.esr & 0xffff) != 0x4c0 ||
            r.pc != UD_VA+size-4) { rc = -EIO; goto bad; }
        call->domain[i] = first ? (i ? 1 : 2) : i+1;
        ++call->entries;
        if (call->message.action != (i ? (first ? US_ACTION_CLIENT : US_ACTION_RPC) : US_ACTION_DOMAIN) ||
            call->message.target != (i ? (first ? 0 :
                (input.type == US_WRITE_REQ ? 104 :
                 (input.type == US_SHARED_WRITE_REQ ||
                  input.type == US_SHARED_READ_REQ ||
                  input.type == US_SHARED_FETCH_ADD_REQ) ? 105 :
                 input.type == US_HOME_REQ ? 103 : 102)) : (first ? 1 : 2))) {
            rc = -EPROTO; goto bad;
        }
    }
    {
        struct us_message result = call->message;
        result.action = input.action; result.target = input.target;
        if (input.type == US_WRITE_REQ && result.ticket == input.value)
            result.ticket = input.ticket;
        if (input.type == US_SHARED_WRITE_REQ && result.ticket != input.value) {
            rc = -EPROTO; goto bad;
        }
        if (input.type == US_SHARED_WRITE_REQ ||
            input.type == US_SHARED_READ_REQ ||
            input.type == US_SHARED_FETCH_ADD_REQ)
            result.ticket = input.ticket;
        if (input.type == US_SHARED_FETCH_ADD_REQ && result.status == EAGAIN)
            result.status = input.status;
        if (memcmp(&result, &input, sizeof(input))) { rc = -EPROTO; goto bad; }
    }
    rc = call->message.status == EAGAIN ? -EAGAIN : 0; goto timed;
bad:
    m->esr = r.esr; m->far = r.far; call->fault_vector = r.vector;
    s->committed = false;
timed:
    m->elapsed_ns = ktime_get_ns()-start;
restore:
    local_irq_restore(flags); preempt_enable();
unlock:
    mmap_read_unlock(current->mm);
    return rc;
}
static long ud_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct ud_session *s = file->private_data;
    void __user *p = (void __user *)arg;
    struct ud_call call;
    int rc;
    if (!capable(CAP_SYS_RAWIO) || current != s->owner || current->mm != s->mm) return -EPERM;
    if (mutex_lock_interruptible(&s->lock)) return -ERESTARTSYS;
    switch (cmd) {
    case UD_SET_CODE_MODE: {
        u32 mode;
        if (s->prepared) { rc = -EBUSY; break; }
        if (copy_from_user(&mode, p, sizeof(mode))) { rc = -EFAULT; break; }
        if (mode != UD_CODE_LEGACY && mode != UD_CODE_REMOTE_VERIFIED) {
            rc = -EINVAL; break;
        }
        if (mode == UD_CODE_REMOTE_VERIFIED &&
            (!allow_remote_code || s->profile != UD_PROFILE_MICRO)) {
            rc = -EOPNOTSUPP; break;
        }
        s->code_mode = mode; rc = 0; break;
    }
    case UD_SET_PROFILE: {
        u32 profile;
        if (s->prepared) { rc = -EBUSY; break; }
        if (copy_from_user(&profile, p, sizeof(profile))) { rc = -EFAULT; break; }
        if (profile > UD_PROFILE_SOCIAL_SHARED) { rc = -EINVAL; break; }
        if (profile != UD_PROFILE_MICRO &&
            s->code_mode == UD_CODE_REMOTE_VERIFIED) { rc = -EINVAL; break; }
        s->profile = profile; rc = 0; break;
    }
    case UD_SOCIAL_CALL: {
        struct ud_mapped *m = s->args[0];
        if (!s->committed || s->profile == UD_PROFILE_MICRO) { rc = -ENOTCONN; break; }
        rc = ud_run_social(s);
        m->status = rc ? -rc : 0;
        smp_store_release(&m->completion, ++s->completion);
        break;
    }
    case UD_POOL_PREPARE: rc = ud_prepare_pool(s, p); break;
    case UD_CONNECTION_INFO: {
        struct ud_connection_info info = {
            .abi = UD_POOL_ABI, .multi = s->multi,
            .identity = s->p.contract.nonce, .generation = s->pool_generation,
            .slot = s->pool_slot, .capacity = s->multi ? UD_CLIENTS : 1,
        };
        rc = copy_to_user(p, &info, sizeof(info)) ? -EFAULT : 0;
        break;
    }
    case UD_SET_NESTED_PATH: {
        u32 path;
        if (s->prepared) { rc = -EBUSY; break; }
        if (copy_from_user(&path, p, sizeof(path))) { rc = -EFAULT; break; }
        if (path != UD_NESTED_LEGACY && path != UD_NESTED_FUSED) {
            rc = -EINVAL; break;
        }
        s->nested_path = path;
        rc = 0;
        break;
    }
    case UD_PREPARE: rc = ud_prepare_session(s, p); break;
    case UD_COMMIT: rc = ud_commit(s); break;
    case UD_TLB_PROBE: rc = ud_tlb_probe(s, p); break;
#ifdef UD_TRACE_ENABLE
    case UD_TRACE_CALL: {
        struct ud_trace_call trace;
        if (copy_from_user(&trace, p, sizeof(trace))) { rc = -EFAULT; break; }
        if (trace.procedure != 1 && trace.procedure != 2) { rc = -EINVAL; break; }
        memset(&call, 0, sizeof(call));
        call.a = trace.a; call.b = trace.b; call.procedure = trace.procedure;
        memset(&trace, 0, sizeof(trace));
        trace.a = call.a; trace.b = call.b; trace.procedure = call.procedure;
        rc = ud_run(s, &call, false, &trace);
        trace.result = call.result; trace.gate_total_ns = call.elapsed_ns;
        trace.esr = call.esr; trace.far = call.far;
        trace.transitions = call.transitions; trace.cpu = call.cpu;
        if (copy_to_user(p, &trace, sizeof(trace))) rc = -EFAULT;
        break;
    }
#endif
    case UD_CALL:
        if (copy_from_user(&call, p, sizeof(call))) { rc = -EFAULT; break; }
        call.result = call.elapsed_ns = call.esr = call.far = 0;
        call.transitions = call.cpu = 0;
        rc = ud_run(s, &call, false, NULL);
        if (copy_to_user(p, &call, sizeof(call))) rc = -EFAULT;
        break;
    case UD_MAPPED_CALL: {
        unsigned int d;
        struct ud_mapped *m;
        if (!s->committed || arg < 1 || arg > 3) { rc = -EINVAL; break; }
        d = arg == 1 ? UD_DIRECT : arg == 2 ? UD_MIDDLE : UD_QUERY;
        m = s->args[d];
        memset(&call, 0, sizeof(call));
        call.procedure = arg;
        call.a = READ_ONCE(m->a); call.b = READ_ONCE(m->b);
        rc = ud_run(s, &call, true, NULL);
        m->elapsed_ns = call.elapsed_ns; m->esr = call.esr; m->far = call.far;
        m->procedure = arg; m->transitions = call.transitions;
        m->cpu = call.cpu; m->status = rc ? -rc : 0;
        smp_store_release(&m->completion, ++s->completion);
        break;
    }
    default: rc = -ENOTTY;
    }
    mutex_unlock(&s->lock);
    return rc;
}
static int ud_open(struct inode *inode, struct file *file)
{
    struct ud_session *s;
    int rc;
    if (!capable(CAP_SYS_RAWIO)) return -EPERM;
    rc = ud_supported();
    if (rc) return rc;
    pr_info_once("ub_domain: platform accepted " UD_GATE_MODE " irq_prio_masking=off KASAN/KCSAN=off "
                 "linux_page_size=%lu shadow_page_size=4096 MAIR=%llx TCR=%llx\n",
                 PAGE_SIZE, (unsigned long long)read_sysreg(mair_el1),
                 (unsigned long long)read_sysreg(tcr_el1));
    s = kzalloc(sizeof(*s), GFP_KERNEL);
    if (!s) return -ENOMEM;
    mutex_init(&s->lock);
    s->mm = current->mm;
    s->owner = current;
    get_task_struct(s->owner);
    mmgrab(s->mm);
    file->private_data = s;
    return nonseekable_open(inode, file);
}
static int ud_release(struct inode *inode, struct file *file)
{
    struct ud_session *s = file->private_data;
    (void)inode;
    ud_free_pages(s);
    mmdrop(s->mm);
    put_task_struct(s->owner);
    kfree(s);
    return 0;
}
static int ud_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct ud_session *s = file->private_data;
    unsigned int d;
    int rc = 0;
    if (current != s->owner || current->mm != s->mm || !capable(CAP_SYS_RAWIO)) return -EPERM;
    if (vma->vm_pgoff || vma->vm_end - vma->vm_start != UD_DOMAINS * PAGE_SIZE ||
        !(vma->vm_flags & VM_SHARED) || !(vma->vm_flags & VM_WRITE) ||
        (vma->vm_flags & VM_EXEC)) return -EINVAL;
    mutex_lock(&s->lock);
    if (!s->committed) { rc = -ENOTCONN; goto out; }
    vm_flags_clear(vma, VM_MAYEXEC);
    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
    for (d = 0; d < UD_DOMAINS; ++d) {
        rc = vm_insert_page(vma, vma->vm_start + d * PAGE_SIZE,
                            virt_to_page(s->args[d]));
        if (rc) break;
    }
out:
    mutex_unlock(&s->lock);
    return rc;
}
static const struct file_operations ud_fops = {
    .owner = THIS_MODULE, .open = ud_open, .release = ud_release,
    .unlocked_ioctl = ud_ioctl, .mmap = ud_mmap, .llseek = no_llseek,
};
static struct miscdevice ud_device = {
    .minor = MISC_DYNAMIC_MINOR, .name = "ub_lrpc_domain",
    .fops = &ud_fops, .mode = 0600,
};
static int __init ud_init(void)
{
    BUILD_BUG_ON(offsetof(struct ud_regs, vector) != 288);
    BUILD_BUG_ON(sizeof(struct ud_regs) != UDN_LEAF);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, middle_root) != UDN_MIDDLE_ROOT);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, leaf_root) != UDN_LEAF_ROOT);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, middle_args) != UDN_MIDDLE_ARGS);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, leaf_args) != UDN_LEAF_ARGS);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, call_pc) != UDN_CALL_PC);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, leaf_pc) != UDN_LEAF_PC);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, return_pc) != UDN_RETURN_PC);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, transitions) != UDN_TRANSITIONS);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, complete) != UDN_COMPLETE);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, domain) != UDN_DOMAIN);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, mode) != UDN_MODE);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, social_args) != UDN_SOCIAL_ARGS);
    BUILD_BUG_ON(offsetof(struct ud_nested_gate, social_target) != UDN_SOCIAL_TARGET);
    BUILD_BUG_ON(US_MAPPED_OFFSET + offsetof(struct us_message, action) != UDN_SOCIAL_ACTION_OFFSET);
    BUILD_BUG_ON(US_MAPPED_OFFSET + offsetof(struct us_message, target) != UDN_SOCIAL_TARGET_OFFSET);
    BUILD_BUG_ON(sizeof(struct ud_nested_gate) != UDN_SIZE);
    BUILD_BUG_ON(offsetof(struct ud_mapped, result) != 16);
    BUILD_BUG_ON(sizeof(struct ud_mapped) > UD_PAGE);
    if (!experimental) return -EOPNOTSUPP;
    return misc_register(&ud_device);
}
static void __exit ud_exit(void) { misc_deregister(&ud_device); }
module_init(ud_init);
module_exit(ud_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental B-owned UB ARM64 EL0 domain gate (" UD_GATE_MODE ")");
MODULE_INFO(ud_nested, "legacy,fused");
MODULE_INFO(ud_multi, "pool-v2-8-slots-single-importer-kernel");
MODULE_INFO(ud_social, "user-home-read-opt-in-private-write-and-shared-lse-atomic-probe-abi2-legacy,fused");
MODULE_INFO(ud_remote_code, "opt-in-branch-free-direct-nested-abi1");
#ifdef UD_TRACE_ENABLE
MODULE_INFO(ud_trace, "enabled");
#else
MODULE_INFO(ud_trace, "disabled");
#endif
