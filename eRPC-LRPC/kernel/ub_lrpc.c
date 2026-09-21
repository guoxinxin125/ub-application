// SPDX-License-Identifier: GPL-2.0
/*
 * QEMU/ivshmem prototype of a published-code LRPC memory domain.
 *
 * The publisher may map the code window writable but never executable.  After
 * PUBLISH, a bound shadow may map that window read-only long enough to copy and
 * verify its procedure into private local RX memory; the remote mapping itself
 * is never executable. A-stacks are driver-allocated normal RAM pages shared
 * only by the bound caller and shadow, and therefore retain the architecture's
 * default WB page protection in both BAR cache modes. CALL sleeps the caller
 * and wakes a pinned shadow task with a distinct mm_struct, making Linux's
 * normal context switch install the shadow page tables. RETURN reverses the
 * handoff.
 */
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ub_lrpc.h>

#ifdef CONFIG_X86
#include <asm/mtrr.h>
#include <asm/pgtable_types.h>
#endif

#define UB_LRPC_PCI_VENDOR 0x1af4
#define UB_LRPC_PCI_DEVICE 0x1110
#define UB_LRPC_BAR 2

/*
 * SeaBIOS programs the QEMU 32-bit PCI MMIO hole as one UC variable MTRR.
 * BAR2 is allocated inside this range, and an overlapping WB MTRR cannot
 * override UC.  Cached mode is a QEMU-only experiment, so temporarily remove
 * this exact firmware range and restore it when the device is released.
 */
#define UB_LRPC_QEMU_PCI_HOLE_BASE 0xc0000000UL
#define UB_LRPC_QEMU_PCI_HOLE_SIZE 0x40000000UL

static unsigned int cache_mode = UB_LRPC_CACHE_NONCACHED;
module_param(cache_mode, uint, 0444);
MODULE_PARM_DESC(cache_mode,
	"BAR2 userspace mapping mode: 0=cached, 1=noncached (default)");

struct ub_lrpc_channel {
	struct mutex handoff_lock;
	struct page **astack_pages;
	struct ub_lrpc_file *shadow_owner;
	struct task_struct *shadow_task;
	struct task_struct *caller_task;
	struct mm_struct *shadow_mm; // shadow 的地址空间
	int shadow_cpu;
	bool call_pending;
	struct ub_lrpc_handoff handoff;
};

struct ub_lrpc_dev {
	struct pci_dev *pdev;
	resource_size_t bar_start; // BAR2，共享内存窗口 的起始地址
	resource_size_t bar_size;
	void __iomem *meta;
	struct miscdevice misc;
	struct ub_lrpc_channel channel[UB_LRPC_MAX_PROCS];
	unsigned int astack_npages;
#ifdef CONFIG_X86
	int mtrr_reg;
	bool qemu_pci_uc_removed;
#endif
};

/* Per-open file context. */
struct ub_lrpc_file {
	struct ub_lrpc_dev *dev;
	enum ub_lrpc_role role;
	bool bound;
	u64 epoch;
	u32 proc_index;
	struct ub_lrpc_proc_desc proc;
	struct ub_lrpc_channel *channel;
};

static inline bool ub_lrpc_range_ok(struct ub_lrpc_dev *d, u64 off, u64 len)
{
	return len && off <= d->bar_size && len <= d->bar_size - off;
}

static inline bool ub_lrpc_subrange_ok(u64 off, u64 len, u64 start, u64 size)
{
	return len && off >= start && off - start <= size &&
		len <= size - (off - start);
}

static void ub_lrpc_free_astacks(void *data)
{
	struct ub_lrpc_dev *d = data;
	unsigned int i, page_index;

	for (i = 0; i < UB_LRPC_MAX_PROCS; i++) {
		if (!d->channel[i].astack_pages)
			continue;
		for (page_index = 0; page_index < d->astack_npages;
		     page_index++)
			if (d->channel[i].astack_pages[page_index])
				__free_page(d->channel[i].astack_pages[page_index]);
	}
}

static int ub_lrpc_alloc_astacks(struct ub_lrpc_dev *d)
{
	unsigned int i, page_index;

	if (UB_LRPC_ASTACK_SLOT_SIZE % PAGE_SIZE)
		return -EINVAL;
	d->astack_npages = UB_LRPC_ASTACK_SLOT_SIZE / PAGE_SIZE;
	for (i = 0; i < UB_LRPC_MAX_PROCS; i++) {
		d->channel[i].astack_pages = devm_kcalloc(
			&d->pdev->dev, d->astack_npages,
			sizeof(*d->channel[i].astack_pages), GFP_KERNEL);
		if (!d->channel[i].astack_pages)
			goto no_memory;
		for (page_index = 0; page_index < d->astack_npages;
		     page_index++) {
			d->channel[i].astack_pages[page_index] =
				alloc_page(GFP_KERNEL | __GFP_ZERO);
			if (!d->channel[i].astack_pages[page_index])
				goto no_memory;
		}
	}
	return devm_add_action_or_reset(&d->pdev->dev,
					ub_lrpc_free_astacks, d);

no_memory:
	ub_lrpc_free_astacks(d);
	return -ENOMEM;
}

#ifdef CONFIG_X86
/*
 * x86 encodes WB as the zero cache-mode index: PWT=0, PCD=0, PAT=0.
 * Merely leaving vm_page_prot unchanged is not an explicit WB request for an
 * I/O PFN, so clear all PAT cache-selection bits in cached test mode.
 *
 * remap_pfn_range() still performs PAT memtype tracking and can reject or
 * restrict this request if the same physical range already has a conflicting
 * UC mapping.  The permanently mapped metadata page is a disjoint subrange.
 */
static pgprot_t ub_lrpc_pgprot_writeback(pgprot_t prot)
{
	return __pgprot(pgprot_val(prot) & ~_PAGE_CACHE_MASK);
}

static void ub_lrpc_release_mtrr(void *data)
{
	struct ub_lrpc_dev *d = data;
	int ret;

	if (d->mtrr_reg >= 0) {
		mtrr_del(d->mtrr_reg, (unsigned long)d->bar_start,
			 (unsigned long)d->bar_size);
		d->mtrr_reg = -1;
	}

	if (d->qemu_pci_uc_removed) {
		ret = mtrr_add(UB_LRPC_QEMU_PCI_HOLE_BASE,
			       UB_LRPC_QEMU_PCI_HOLE_SIZE,
			       MTRR_TYPE_UNCACHABLE, true);
		if (ret < 0)
			dev_warn(&d->pdev->dev,
				 "failed to restore QEMU PCI-hole UC MTRR: %d\n",
				 ret);
		else
			dev_info(&d->pdev->dev,
				 "restored QEMU PCI-hole UC MTRR (reg=%d)\n",
				 ret);
		d->qemu_pci_uc_removed = false;
	}
}
#endif

static int ub_lrpc_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct ub_lrpc_dev *d = container_of(misc, struct ub_lrpc_dev, misc); // 已知 ub_lrpc_dev 中成员 misc 的地址，反推出完整 ub_lrpc_dev 的地址
	struct ub_lrpc_file *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return -ENOMEM;
	ctx->dev = d;
	file->private_data = ctx;
	return 0;
}

static int ub_lrpc_release(struct inode *inode, struct file *file)
{
	struct ub_lrpc_file *ctx = file->private_data;
	struct ub_lrpc_channel *ch = ctx->channel;
	struct task_struct *task = NULL;
	struct task_struct *caller = NULL;
	struct mm_struct *mm = NULL;

	if (ch) {
		mutex_lock(&ch->handoff_lock);
		if (ch->shadow_owner == ctx) {
			task = ch->shadow_task;
			mm = ch->shadow_mm;
			ch->shadow_owner = NULL;
			ch->shadow_task = NULL;
			ch->shadow_mm = NULL;
			ch->shadow_cpu = -1;
			caller = ch->caller_task;
			ch->caller_task = NULL;
			if (ch->call_pending) {
				ch->handoff.result = -EPIPE;
				ch->call_pending = false;
				if (caller)
					wake_up_process(caller);
			}
		}
		mutex_unlock(&ch->handoff_lock);
	}
	if (mm)
		mmdrop(mm);
	if (task)
		put_task_struct(task);
	if (caller)
		put_task_struct(caller);
	kfree(ctx);
	return 0;
}

static long ub_lrpc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ub_lrpc_file *ctx = file->private_data;
	struct ub_lrpc_dev *d = ctx->dev;
	struct ub_lrpc_metadata meta;

	switch (cmd) {
	case UB_LRPC_IOC_SET_ROLE: {
		struct ub_lrpc_set_role role;
		if (copy_from_user(&role, (void __user *)arg, sizeof(role)))
			return -EFAULT;
		if (role.role != UB_LRPC_ROLE_PUBLISHER &&
		    role.role != UB_LRPC_ROLE_CALLER &&
		    role.role != UB_LRPC_ROLE_SHADOW)
			return -EINVAL;
		if (ctx->role != UB_LRPC_ROLE_NONE)
			return -EBUSY;
		ctx->role = role.role;
		return 0;
	}
	case UB_LRPC_IOC_PUBLISH: {
		struct ub_lrpc_publish pub;
		u32 i;

		if (ctx->role != UB_LRPC_ROLE_PUBLISHER)
			return -EPERM;
		if (copy_from_user(&pub, (void __user *)arg, sizeof(pub)))
			return -EFAULT;
		if (!pub.code_epoch || !pub.num_procs ||
		    pub.num_procs > UB_LRPC_MAX_PROCS ||
		    pub.image_size > UB_LRPC_CODE_SIZE)
			return -EINVAL;
		for (i = 0; i < pub.num_procs; i++) {
			if (pub.proc[i].flags != UB_LRPC_PROC_PUBLISHED_CODE ||
			    !pub.proc[i].code_size ||
			    pub.proc[i].code_offset > pub.image_size ||
			    pub.proc[i].code_size >
				pub.image_size - pub.proc[i].code_offset ||
			    !pub.proc[i].astack_size ||
			    pub.proc[i].astack_size > UB_LRPC_ASTACK_SLOT_SIZE)
				return -EINVAL;
		}
		memset(&meta, 0, sizeof(meta));
		meta.magic = UB_LRPC_MAGIC;
		meta.abi_version = UB_LRPC_ABI_VERSION;
		meta.isa = 0x3e; /* ELF EM_X86_64 */
		meta.code_epoch = pub.code_epoch;
		meta.image_size = pub.image_size;
		meta.num_procs = pub.num_procs;
		memcpy(meta.proc, pub.proc,
		       sizeof(meta.proc[0]) * pub.num_procs);
		meta.ready = 0;
		memcpy_toio(d->meta, &meta, sizeof(meta));
		wmb();
		iowrite32(1, d->meta + offsetof(struct ub_lrpc_metadata, ready));
		return 0;
	}
	case UB_LRPC_IOC_BIND: {
		struct ub_lrpc_bind bind;
		u32 i;

		if (ctx->role != UB_LRPC_ROLE_CALLER &&
		    ctx->role != UB_LRPC_ROLE_SHADOW)
			return -EPERM;
		/* A binding determines this file's channel and mmap permissions.
		 * Rebinding could leave mappings or a registered shadow attached to
		 * the old channel, so bindings are immutable for the file lifetime.
		 */
		if (ctx->bound)
			return -EBUSY;
		if (copy_from_user(&bind, (void __user *)arg, sizeof(bind)))
			return -EFAULT;
		memcpy_fromio(&meta, d->meta, sizeof(meta));
		if (meta.magic != UB_LRPC_MAGIC ||
		    meta.abi_version != UB_LRPC_ABI_VERSION || !meta.ready)
			return -EAGAIN;
		if (bind.expected_epoch && bind.expected_epoch != meta.code_epoch)
			return -ESTALE;
		for (i = 0; i < meta.num_procs; i++)
			if (meta.proc[i].procedure_id == bind.procedure_id)
				break;
		if (i == meta.num_procs)
			return -ENOENT;
		ctx->bound = true;
		ctx->epoch = meta.code_epoch;
		ctx->proc_index = i;
		ctx->proc = meta.proc[i];
		ctx->channel = &d->channel[i];
		bind.expected_epoch = meta.code_epoch;
		bind.entry_offset = meta.proc[i].code_offset;
		bind.astack_size = meta.proc[i].astack_size;
		bind.astack_offset = UB_LRPC_ASTACK_OFFSET +
			(u64)i * UB_LRPC_ASTACK_SLOT_SIZE;
		bind.code_size = meta.proc[i].code_size;
		bind.code_hash = meta.proc[i].code_hash;
		return copy_to_user((void __user *)arg, &bind, sizeof(bind)) ?
			-EFAULT : 0;
	}
	case UB_LRPC_IOC_INFO: {
		struct ub_lrpc_info info = { .bar_start = d->bar_start,
			.bar_size = d->bar_size, .role = ctx->role,
			.cache_mode = cache_mode };
		memcpy_fromio(&meta, d->meta, sizeof(meta));
		info.code_epoch = meta.code_epoch;
		info.ready = meta.ready;
		info.shadow_cpu = -1;
		if (ctx->channel) {
			struct ub_lrpc_channel *ch = ctx->channel;
			mutex_lock(&ch->handoff_lock);
			info.shadow_ready = ch->shadow_owner != NULL;
			info.shadow_cpu = ch->shadow_cpu;
			info.shadow_pid = ch->shadow_task ?
				task_pid_nr(ch->shadow_task) : 0;
			mutex_unlock(&ch->handoff_lock);
		}
		return copy_to_user((void __user *)arg, &info, sizeof(info)) ?
			-EFAULT : 0;
	}
	case UB_LRPC_IOC_REGISTER_SHADOW:
		if (ctx->role != UB_LRPC_ROLE_SHADOW || !ctx->bound || !current->mm ||
		    current->nr_cpus_allowed != 1)
			return -EPERM;
		mutex_lock(&ctx->channel->handoff_lock);
		if (ctx->channel->shadow_owner) {
			mutex_unlock(&ctx->channel->handoff_lock);
			return -EBUSY;
		}
		get_task_struct(current);
		mmgrab(current->mm);
		ctx->channel->shadow_owner = ctx;
		ctx->channel->shadow_task = current;
		ctx->channel->shadow_mm = current->mm;
		ctx->channel->shadow_cpu = task_cpu(current);
		mutex_unlock(&ctx->channel->handoff_lock);
		return 0;
	case UB_LRPC_IOC_WAIT_CALL: {
		struct ub_lrpc_handoff handoff;
		struct ub_lrpc_channel *ch = ctx->channel;
		if (ctx->role != UB_LRPC_ROLE_SHADOW)
			return -EPERM;
		for (;;) {
			mutex_lock(&ch->handoff_lock);
			if (ch->shadow_owner != ctx) {
				mutex_unlock(&ch->handoff_lock);
				return -EPIPE;
			}
			if (ch->call_pending)
				break;
			set_current_state(TASK_INTERRUPTIBLE);
			mutex_unlock(&ch->handoff_lock);
			schedule();
			__set_current_state(TASK_RUNNING);
			if (signal_pending(current))
				return -ERESTARTSYS;
		}
		ch->handoff.shadow_dispatch_ns = ktime_get_ns();
		handoff = ch->handoff;
		mutex_unlock(&ch->handoff_lock);
		return copy_to_user((void __user *)arg, &handoff,
				    sizeof(handoff)) ? -EFAULT : 0;
	}
	case UB_LRPC_IOC_CALL: {
		struct ub_lrpc_handoff handoff;
		struct task_struct *old_caller = NULL;
		struct ub_lrpc_channel *ch = ctx->channel;
		if (ctx->role != UB_LRPC_ROLE_CALLER || !ctx->bound)
			return -EPERM;
		if (copy_from_user(&handoff, (void __user *)arg, sizeof(handoff)))
			return -EFAULT;
		mutex_lock(&ch->handoff_lock);
		if (!ch->shadow_owner) {
			mutex_unlock(&ch->handoff_lock);
			return -ENOTCONN;
		}
		if (current->mm == ch->shadow_mm) {
			mutex_unlock(&ch->handoff_lock);
			return -EXDEV;
		}
		if (current->nr_cpus_allowed != 1) {
			mutex_unlock(&ch->handoff_lock);
			return -EXDEV;
		}
		if (task_cpu(current) != ch->shadow_cpu) {
			mutex_unlock(&ch->handoff_lock);
			return -EXDEV;
		}
		if (ch->call_pending) {
			mutex_unlock(&ch->handoff_lock);
			return -EBUSY;
		}
		old_caller = ch->caller_task;
		get_task_struct(current);
		ch->caller_task = current;
		handoff.procedure_id = ctx->proc.procedure_id;
		handoff.caller_pid = task_pid_nr(current);
		handoff.caller_cpu = task_cpu(current);
		handoff.result = 0;
		handoff.call_enter_ns = ktime_get_ns();
		handoff.shadow_dispatch_ns = 0;
		handoff.shadow_return_ns = 0;
		handoff.caller_resume_ns = 0;
		ch->handoff = handoff;
		ch->call_pending = true;
		set_current_state(TASK_UNINTERRUPTIBLE);
		wake_up_process(ch->shadow_task);
		mutex_unlock(&ch->handoff_lock);
		if (old_caller)
			put_task_struct(old_caller);
		for (;;) {
			schedule();
			__set_current_state(TASK_RUNNING);
			mutex_lock(&ch->handoff_lock);
			if (!ch->call_pending)
				break;
			set_current_state(TASK_UNINTERRUPTIBLE);
			mutex_unlock(&ch->handoff_lock);
		}
		ch->handoff.caller_resume_ns = ktime_get_ns();
		handoff = ch->handoff;
		mutex_unlock(&ch->handoff_lock);
		return copy_to_user((void __user *)arg, &handoff,
				    sizeof(handoff)) ? -EFAULT : 0;
	}
	case UB_LRPC_IOC_RETURN: {
		struct ub_lrpc_handoff handoff;
		struct ub_lrpc_channel *ch = ctx->channel;
		if (copy_from_user(&handoff, (void __user *)arg, sizeof(handoff)))
			return -EFAULT;
		mutex_lock(&ch->handoff_lock);
		if (ch->shadow_owner != ctx || !ch->call_pending) {
			mutex_unlock(&ch->handoff_lock);
			return -EPERM;
		}
		ch->handoff.result = handoff.result;
		ch->handoff.shadow_return_ns = ktime_get_ns();
		ch->call_pending = false;
		set_current_state(TASK_INTERRUPTIBLE);
		wake_up_process(ch->caller_task);
		mutex_unlock(&ch->handoff_lock);
		/* Like ChCore sys_ipc_return(): do not continue the shadow. The
		 * next CALL wakes this kernel continuation, which returns to the
		 * shadow loop and consumes that already-pending request. */
		schedule();
		__set_current_state(TASK_RUNNING);
		if (signal_pending(current))
			return -ERESTARTSYS;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static int ub_lrpc_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ub_lrpc_file *ctx = file->private_data;
	struct ub_lrpc_dev *d = ctx->dev;
	u64 off = (u64)vma->vm_pgoff << PAGE_SHIFT;
	u64 len = vma->vm_end - vma->vm_start;
	bool write = vma->vm_flags & VM_WRITE;
	bool exec = vma->vm_flags & VM_EXEC;
	u64 expected_astack;
	unsigned int page_index;
	int ret;

	if (!PAGE_ALIGNED(off) || !PAGE_ALIGNED(len) || !len)
		return -EINVAL;

	/* A-stack mappings are normal A-local RAM.  Do not set VM_IO/VM_PFNMAP
	 * and do not apply BAR2's UC/WB page-protection policy here. */
	expected_astack = UB_LRPC_ASTACK_OFFSET +
		(u64)ctx->proc_index * UB_LRPC_ASTACK_SLOT_SIZE;
	if (ub_lrpc_subrange_ok(off, len, UB_LRPC_ASTACK_OFFSET,
				UB_LRPC_ASTACK_SIZE)) {
		if ((ctx->role != UB_LRPC_ROLE_CALLER &&
		     ctx->role != UB_LRPC_ROLE_SHADOW) || !ctx->bound || exec ||
		    off != expected_astack || len > UB_LRPC_ASTACK_SLOT_SIZE)
			return -EPERM;
		vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
		for (page_index = 0; page_index < len / PAGE_SIZE;
		     page_index++) {
			ret = vm_insert_page(vma,
				vma->vm_start + (unsigned long)page_index * PAGE_SIZE,
				ctx->channel->astack_pages[page_index]);
			if (ret)
				return ret;
		}
		dev_info_ratelimited(&d->pdev->dev,
			"A-stack local mmap proc=%u len=%#llx cache_mode=writeback\n",
			ctx->proc.procedure_id, (unsigned long long)len);
		return 0;
	}

	if (!ub_lrpc_range_ok(d, off, len))
		return -EINVAL;

	if (ub_lrpc_subrange_ok(off, len, UB_LRPC_CODE_OFFSET,
				UB_LRPC_CODE_SIZE)) {
		if (ctx->role == UB_LRPC_ROLE_PUBLISHER) {
			if (!write || exec)
				return -EPERM;
		} else if (ctx->role == UB_LRPC_ROLE_SHADOW && ctx->bound) {
			if (write || exec)
				return -EPERM;
		} else {
			return -EPERM;
		}
	} else if (ub_lrpc_subrange_ok(off, len,
				       UB_LRPC_REMOTE_BENCH_OFFSET,
				       UB_LRPC_REMOTE_BENCH_SIZE)) {
		if (ctx->role != UB_LRPC_ROLE_CALLER || !ctx->bound || exec ||
		    off != UB_LRPC_REMOTE_BENCH_OFFSET +
			   (u64)ctx->proc_index *
				UB_LRPC_REMOTE_BENCH_SLOT_SIZE ||
		    len > UB_LRPC_REMOTE_BENCH_SLOT_SIZE)
			return -EPERM;
	} else if (off >= UB_LRPC_DATA_OFFSET) {
		if ((ctx->role != UB_LRPC_ROLE_PUBLISHER &&
		     ctx->role != UB_LRPC_ROLE_SHADOW) || exec)
			return -EPERM;
	} else {
		return -EPERM; /* metadata is ioctl-only */
	}

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	if (cache_mode == UB_LRPC_CACHE_NONCACHED) {
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	} else {
#ifdef CONFIG_X86
		vma->vm_page_prot =
			ub_lrpc_pgprot_writeback(vma->vm_page_prot);
#else
		dev_warn_once(&d->pdev->dev,
			      "cached mode uses the architecture default page protection\n");
#endif
	}

#ifdef CONFIG_X86
	dev_info_ratelimited(&d->pdev->dev,
		"mmap off=%#llx len=%#llx cache_mode=%s pgprot=%#lx cache_bits=%#lx\n",
		(unsigned long long)off, (unsigned long long)len,
		cache_mode == UB_LRPC_CACHE_NONCACHED ?
			"noncached" : "writeback",
		(unsigned long)pgprot_val(vma->vm_page_prot),
		(unsigned long)(pgprot_val(vma->vm_page_prot) &
				_PAGE_CACHE_MASK));
#endif

	return remap_pfn_range(vma, vma->vm_start,
			       (d->bar_start + off) >> PAGE_SHIFT, len,
			       vma->vm_page_prot);
}

static const struct file_operations ub_lrpc_fops = {
	.owner = THIS_MODULE,
	.open = ub_lrpc_open,
	.release = ub_lrpc_release,
	.unlocked_ioctl = ub_lrpc_ioctl,
	.mmap = ub_lrpc_mmap,
	.llseek = no_llseek,
};

static int ub_lrpc_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct ub_lrpc_dev *d;
	int ret, i;

	if (cache_mode != UB_LRPC_CACHE_CACHED &&
	    cache_mode != UB_LRPC_CACHE_NONCACHED)
		return -EINVAL;
	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->pdev = pdev;
#ifdef CONFIG_X86
	d->mtrr_reg = -1;
#endif
	d->bar_start = pci_resource_start(pdev, UB_LRPC_BAR);
	d->bar_size = pci_resource_len(pdev, UB_LRPC_BAR);
	if (d->bar_size <= UB_LRPC_DATA_OFFSET)
		return -ENOSPC;
	if (!(pci_resource_flags(pdev, UB_LRPC_BAR) & IORESOURCE_MEM))
		return -ENODEV;
	if (!devm_request_mem_region(&pdev->dev, d->bar_start, d->bar_size,
				     "ub_lrpc"))
		return -EBUSY;
#ifdef CONFIG_X86
	/*
	 * SeaBIOS covers the QEMU PCI hole with one UC variable MTRR.  UC wins
	 * over an overlapping WB range, so cached mode first removes that exact
	 * firmware entry.  Other MMIO mappings still request UC through PAT; this
	 * relaxation is intentionally limited to the disposable QEMU guest.
	 */
	if (cache_mode == UB_LRPC_CACHE_CACHED) {
		if (d->bar_start < UB_LRPC_QEMU_PCI_HOLE_BASE ||
		    d->bar_start + d->bar_size >
			UB_LRPC_QEMU_PCI_HOLE_BASE +
			UB_LRPC_QEMU_PCI_HOLE_SIZE) {
			dev_err(&pdev->dev,
				"BAR2 is outside the expected QEMU PCI hole\n");
			return -ERANGE;
		}

		ret = mtrr_del(-1, UB_LRPC_QEMU_PCI_HOLE_BASE,
			       UB_LRPC_QEMU_PCI_HOLE_SIZE);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"failed to remove QEMU PCI-hole UC MTRR: %d\n",
				ret);
			return ret;
		}
		d->qemu_pci_uc_removed = true;
		dev_info(&pdev->dev,
			 "temporarily removed QEMU PCI-hole UC MTRR (reg=%d)\n",
			 ret);

		ret = devm_add_action_or_reset(&pdev->dev,
					       ub_lrpc_release_mtrr, d);
		if (ret)
			return ret;

		d->mtrr_reg = mtrr_add((unsigned long)d->bar_start,
				       (unsigned long)d->bar_size,
				       MTRR_TYPE_WRBACK, true);
		if (d->mtrr_reg < 0) {
			dev_err(&pdev->dev,
				"failed to add WB MTRR for BAR2: %d\n",
				d->mtrr_reg);
			return d->mtrr_reg;
		}

		dev_info(&pdev->dev,
			 "BAR2 guest MTRR set to write-back in cached test mode (reg=%d)\n",
			 d->mtrr_reg);
	}
#endif
	/* Keep only the metadata page permanently mapped by the kernel. Mapping
	 * the complete BAR here would reserve one UC PAT type for all 4 MiB and
	 * prevent the userspace latency experiment from selecting WB vs UC for
	 * the code, remote benchmark, and data subranges. */
	d->meta = devm_ioremap(&pdev->dev, d->bar_start, UB_LRPC_META_SIZE);
	if (!d->meta)
		return -ENOMEM;
	for (i = 0; i < UB_LRPC_MAX_PROCS; i++) {
		mutex_init(&d->channel[i].handoff_lock);
		d->channel[i].shadow_cpu = -1;
	}
	ret = ub_lrpc_alloc_astacks(d);
	if (ret)
		return ret;
	d->misc.minor = MISC_DYNAMIC_MINOR;
	d->misc.name = "ub_lrpc0";
	d->misc.fops = &ub_lrpc_fops;
	d->misc.parent = &pdev->dev;
	ret = misc_register(&d->misc);
	if (ret)
		return ret;
	pci_set_drvdata(pdev, d);
	dev_info(&pdev->dev,
		 "UB-LRPC BAR2 start=%pa size=%pa cache_mode=%s exposed as /dev/%s\n",
		 &d->bar_start, &d->bar_size,
		 cache_mode == UB_LRPC_CACHE_NONCACHED ? "noncached" : "cached",
		 d->misc.name);
	dev_info(&pdev->dev,
		 "A-stacks use %u driver-allocated local WB pages per channel\n",
		 d->astack_npages);
	return 0;
}

static void ub_lrpc_remove(struct pci_dev *pdev)
{
	struct ub_lrpc_dev *d = pci_get_drvdata(pdev);
	misc_deregister(&d->misc);
}

static const struct pci_device_id ub_lrpc_ids[] = {
	{ PCI_DEVICE(UB_LRPC_PCI_VENDOR, UB_LRPC_PCI_DEVICE) },
	{ }
};
MODULE_DEVICE_TABLE(pci, ub_lrpc_ids);

static struct pci_driver ub_lrpc_driver = {
	.name = "ub_lrpc",
	.id_table = ub_lrpc_ids,
	.probe = ub_lrpc_probe,
	.remove = ub_lrpc_remove,
};
module_pci_driver(ub_lrpc_driver);

MODULE_DESCRIPTION("Published-code ivshmem-backed UB LRPC prototype");
MODULE_AUTHOR("eRPC-LRPC prototype");
MODULE_LICENSE("GPL");
