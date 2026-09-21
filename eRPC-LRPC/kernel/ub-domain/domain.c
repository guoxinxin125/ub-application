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
#include <asm/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/sysreg.h>
#include <asm/memory.h>
#include <lrpc/ub_domain_layout.h>
#include <lrpc/ub_domain_services.h>

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
extern void ud_enter(struct ud_regs *regs, u64 root, u64 tcr);
static bool experimental;
module_param(experimental, bool, 0400);
MODULE_PARM_DESC(experimental, "Opt into bounded IRQ-masked EL0 gate on a test machine");

struct ud_session {
    struct mutex lock;
    struct mm_struct *mm;
    struct ud_prepare p;
    void *code[UD_DOMAINS], *stack[UD_DOMAINS], *args[UD_DOMAINS];
    struct ud_initial initial[UD_DOMAINS];
    struct file *import_file[UD_DOMAINS * 4 + 1];
    bool prepared, committed;
};

static int ud_supported(void)
{
    u64 tcr = read_sysreg(tcr_el1), mair = read_sysreg(mair_el1);
    if (read_sysreg(CurrentEL) != 4) {
        pr_warn_once("ub_domain: EL1 kernel required; VHE EL2 gate is not supported\n");
        return -EOPNOTSUPP;
    }
    if (!experimental || IS_ENABLED(CONFIG_ARM64_PSEUDO_NMI) ||
        IS_ENABLED(CONFIG_KASAN) || IS_ENABLED(CONFIG_KCSAN)) {
        pr_warn_once("ub_domain: experimental opt-in required; pseudo-NMI/KASAN/KCSAN unsupported\n");
        return -EOPNOTSUPP;
    }
    /* Shadow uses four-level 4K tables independently of Linux PAGE_SIZE.
     * TTBR1 keeps the host granule. Require hardware support for TG0=4K. */
    if (((read_sysreg(id_aa64mmfr0_el1) >> 28) & 15) == 15 ||
        ((tcr >> 32) & 7) > 5 || (tcr & (1ULL << 59))) {
        pr_warn_once("ub_domain: requires 4K hardware granule and <=48-bit non-LPA2 output\n");
        return -EOPNOTSUPP;
    }
    if (((mair >> (8 * MT_NORMAL)) & 255) != 0xff ||
        ((mair >> (8 * MT_NORMAL_NC)) & 255) != 0x44) {
        pr_warn_once("ub_domain: unexpected MAIR Normal/Normal-NC encodings\n");
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
    rc = ((pte_val(pte) & PTE_ATTRINDX_MASK) == PTE_ATTRINDX(MT_NORMAL_NC) &&
          (pte_val(pte) & (PTE_VALID | PTE_USER)) == (PTE_VALID | PTE_USER)) ? 0 : -EOPNOTSUPP;
    pte_unmap_unlock(ptep, ptl);
    if (rc) {
        pr_warn_ratelimited("ub_domain: import PTE must be valid EL0 Normal-NC (offset=%lx)\n", offset);
        return rc;
    }
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
        rc = ud_import_pa(s, UD_TABLE(d, l), &pa, d*4+l, initialize);
        if (rc) return rc;
        if (initialize) s->p.contract.table_pa[d][l] = pa;
        else if (s->p.contract.table_pa[d][l] != pa) return -ESTALE;
    }
    rc = ud_import_pa(s, UD_HEAP, &pa, UD_DOMAINS*4, initialize);
    if (rc) return rc;
    if (initialize) s->p.contract.heap_pa = pa;
    else if (s->p.contract.heap_pa != pa) return -ESTALE;
    return 0;
}

static void ud_free_pages(struct ud_session *s)
{
    unsigned int d;
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
}
static int ud_prepare_session(struct ud_session *s, void __user *arg)
{
    struct ud_prepare input;
    unsigned int d;
    int rc;
    if (s->prepared) return -EALREADY;
    if (copy_from_user(&input, arg, sizeof(input))) return -EFAULT;
    if (!input.import_va || input.import_va & (UD_PAGE - 1) ||
        input.import_va > TASK_SIZE - UD_BYTES) return -EINVAL;
    memset(&s->p, 0, sizeof(s->p));
    s->p.import_va = input.import_va;
    s->p.contract.magic = UD_MAGIC;
    s->p.contract.abi = UD_ABI;
    s->p.contract.nonce = get_random_u64() | 1;
    s->p.contract.wb_index = MT_NORMAL;
    s->p.contract.nc_index = MT_NORMAL_NC;
    mmap_read_lock(current->mm);
    rc = ud_mapping(s, true);
    mmap_read_unlock(current->mm);
    if (rc) goto fail;
    for (d = 0; d < UD_DOMAINS; ++d) {
        s->code[d] = (void *)get_zeroed_page(GFP_KERNEL);
        s->stack[d] = (void *)get_zeroed_page(GFP_KERNEL);
        s->args[d] = (void *)get_zeroed_page(GFP_KERNEL);
        if (!s->code[d] || !s->stack[d] || !s->args[d]) {
            rc = -ENOMEM; goto fail;
        }
        s->p.contract.code_pa[d] = virt_to_phys(s->code[d]);
        s->p.contract.stack_pa[d] = virt_to_phys(s->stack[d]);
        s->p.contract.args_pa[d] = virt_to_phys(s->args[d]);
    }
    if (!ud_contract_valid(&s->p.contract)) { rc = -ERANGE; goto fail; }
    if (copy_to_user(arg, &s->p, sizeof(s->p))) { rc = -EFAULT; goto fail; }
    s->prepared = true;
    return 0;
fail:
    ud_free_pages(s);
    return rc;
}

static int ud_copy(struct ud_session *s, unsigned long off, void *to, size_t n)
{
    return copy_from_user(to, (void __user *)(s->p.import_va + off), n) ? -EFAULT : 0;
}
static int ud_commit(struct ud_session *s)
{
    struct ud_publication pub;
    u64 *expected, *actual;
    unsigned int d, l;
    unsigned long size;
    int rc = -ENOMEM;
    if (!s->prepared || s->committed) return -EINVAL;
    expected = kmalloc(UD_PAGE, GFP_KERNEL);
    actual = kmalloc(UD_PAGE, GFP_KERNEL);
    if (!expected || !actual) goto out;
    rc = ud_copy(s, 0, &pub, sizeof(pub));
    if (rc) goto out;
    if (memcmp(&pub.contract, &s->p.contract, sizeof(pub.contract)) ||
        pub.request != s->p.contract.nonce || pub.complete != pub.request) {
        rc = -ESTALE; goto out;
    }
    for (d = 0; d < UD_DOMAINS; ++d) {
        struct ud_initial initial = {UD_ARGS, UD_DATA, UD_STACK + UD_PAGE, UD_VA};
        const unsigned char *code = ud_service(d, &size);
        for (l = 0; l < 4; ++l) {
            ud_build_table(&s->p.contract, d, l, expected);
            rc = ud_copy(s, UD_TABLE(d, l), actual, UD_PAGE);
            if (rc) goto out;
            if (memcmp(expected, actual, UD_PAGE)) { rc = -EBADMSG; goto out; }
        }
        rc = ud_copy(s, UD_CONTEXT(d), &s->initial[d], sizeof(initial));
        if (rc) goto out;
        if (memcmp(&initial, &s->initial[d], sizeof(initial))) { rc = -EBADMSG; goto out; }
        if (!size || size > UD_PAGE) { rc = -EINVAL; goto out; }
        rc = ud_copy(s, UD_IMAGE(d), s->code[d], size);
        if (rc) goto out;
        if (memcmp(code, s->code[d], size)) { rc = -EBADMSG; goto out; }
        flush_icache_range((unsigned long)s->code[d], (unsigned long)s->code[d] + size);
    }
    rc = ud_copy(s, UD_HEAP, actual, UD_PAGE);
    if (rc) goto out;
    if (actual[0] != 100) { rc = -EBADMSG; goto out; }
    for (d = 0; d < 32; ++d)
        if (actual[8 + d*8] != d || actual[9 + d*8] != ((d+1) & 31) ||
            actual[10 + d*8] != 1000+d) { rc = -EBADMSG; goto out; }
    s->committed = true;
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
static bool ud_returned(struct ud_regs *r, unsigned int d, bool nested)
{
    unsigned long size;
    ud_service(d, &size);
    return r->vector == 8 && (r->esr >> 26) == 0x3c &&
        (r->esr & 0xffff) == (nested ? 0x4c1 : 0x4c0) &&
        r->pc == UD_VA + (nested ? UD_NESTED_TRAP_OFFSET : size-4);
}
static int ud_run(struct ud_session *s, struct ud_call *c)
{
    struct ud_regs r, leaf;
    unsigned int d = c->procedure == 1 ? UD_DIRECT :
                     c->procedure == 2 ? UD_MIDDLE : UD_QUERY;
    unsigned long flags;
    u64 tcr, start;
    int rc;
    if (!s->committed || c->procedure < 1 || c->procedure > 3 || c->reserved ||
        (c->procedure == 3 && (!c->a || c->a > 32))) return -EINVAL;
    /* Read lock spans entry/return; munmap/mremap cannot replace the VMA.
     * External UB teardown is forbidden by the trusted session contract. */
    mmap_read_lock(current->mm);
    rc = ud_mapping(s, false);
    if (rc) goto unlock;
    memset(s->args[d], 0, 3 * sizeof(u64));
    ((u64 *)s->args[d])[0] = c->a; ((u64 *)s->args[d])[1] = c->b;
    ud_initial_regs(s, d, &r);
    preempt_disable();
    local_irq_save(flags);
    rc = ud_supported();
    if (rc) goto restore;
    c->cpu = raw_smp_processor_id();
    /* NC table walks, inner shareable. ASID/TLB retention intentionally off. */
    tcr = read_sysreg(tcr_el1);
    tcr &= ~((15ULL << 8) | (1ULL << 7) | (3ULL << 12) |
             (3ULL << 39) | (3ULL << 14) | 63ULL);
    tcr |= (3ULL << 12) | 16ULL;
    start = ktime_get_ns();
    ud_enter(&r, s->p.contract.table_pa[d][0], tcr);
    c->transitions = 2;
    if (d == UD_MIDDLE && ud_returned(&r, d, true)) {
        memset(s->args[UD_LEAF], 0, 3 * sizeof(u64));
        ((u64 *)s->args[UD_LEAF])[0] = r.x[2];
        ud_initial_regs(s, UD_LEAF, &leaf);
        ud_enter(&leaf, s->p.contract.table_pa[UD_LEAF][0], tcr);
        c->transitions += 2;
        if (!ud_returned(&leaf, UD_LEAF, false)) { r = leaf; goto bad; }
        r.pc += 4;
        r.x[2] = ((u64 *)s->args[UD_LEAF])[2];
        ud_enter(&r, s->p.contract.table_pa[d][0], tcr);
        c->transitions += 2;
    }
    if (!ud_returned(&r, d, false)) goto bad;
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
static long ud_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct ud_session *s = file->private_data;
    void __user *p = (void __user *)arg;
    struct ud_call call;
    int rc;
    if (!capable(CAP_SYS_RAWIO) || current->mm != s->mm) return -EPERM;
    if (mutex_lock_interruptible(&s->lock)) return -ERESTARTSYS;
    switch (cmd) {
    case UD_PREPARE: rc = ud_prepare_session(s, p); break;
    case UD_COMMIT: rc = ud_commit(s); break;
    case UD_CALL:
        if (copy_from_user(&call, p, sizeof(call))) { rc = -EFAULT; break; }
        call.result = call.elapsed_ns = call.esr = call.far = 0;
        call.transitions = call.cpu = 0;
        rc = ud_run(s, &call);
        if (copy_to_user(p, &call, sizeof(call))) rc = -EFAULT;
        break;
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
    s = kzalloc(sizeof(*s), GFP_KERNEL);
    if (!s) return -ENOMEM;
    mutex_init(&s->lock);
    s->mm = current->mm;
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
    kfree(s);
    return 0;
}
static const struct file_operations ud_fops = {
    .owner = THIS_MODULE, .open = ud_open, .release = ud_release,
    .unlocked_ioctl = ud_ioctl, .llseek = no_llseek,
};
static struct miscdevice ud_device = {
    .minor = MISC_DYNAMIC_MINOR, .name = "ub_lrpc_domain",
    .fops = &ud_fops, .mode = 0600,
};
static int __init ud_init(void)
{
    BUILD_BUG_ON(offsetof(struct ud_regs, vector) != 288);
    if (!experimental) return -EOPNOTSUPP;
    return misc_register(&ud_device);
}
static void __exit ud_exit(void) { misc_deregister(&ud_device); }
module_init(ud_init);
module_exit(ud_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental B-owned UB ARM64 EL0 domain gate");
