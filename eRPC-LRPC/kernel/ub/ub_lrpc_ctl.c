// SPDX-License-Identifier: GPL-2.0
/* Local caller-to-shadow handoff driver for the real UBS Memory backend. */
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/ub_lrpc.h>

#define UB_LRPC_CTL_NAME "ub_lrpc_ctl"

struct ub_lrpc_ctl_file;

struct ub_lrpc_ctl_channel {
	struct mutex handoff_lock;
	struct ub_lrpc_ctl_file *shadow_owner;
	struct task_struct *shadow_task;
	struct task_struct *caller_task;
	struct mm_struct *shadow_mm;
	u64 epoch;
	int shadow_cpu;
	bool call_pending;
	struct ub_lrpc_handoff handoff;
};

struct ub_lrpc_ctl_dev {
	struct miscdevice misc;
	void *astacks;
	struct ub_lrpc_ctl_channel channel[UB_LRPC_MAX_PROCS];
};

struct ub_lrpc_ctl_file {
	struct ub_lrpc_ctl_dev *dev;
	enum ub_lrpc_role role;
	bool bound;
	u64 epoch;
	u32 procedure_id;
	u32 proc_index;
	struct ub_lrpc_ctl_channel *channel;
};

static struct ub_lrpc_ctl_dev ub_lrpc_ctl;

static int ub_lrpc_ctl_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct ub_lrpc_ctl_dev *dev =
		container_of(misc, struct ub_lrpc_ctl_dev, misc);
	struct ub_lrpc_ctl_file *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return -ENOMEM;
	ctx->dev = dev;
	file->private_data = ctx;
	return 0;
}

static int ub_lrpc_ctl_release(struct inode *inode, struct file *file)
{
	struct ub_lrpc_ctl_file *ctx = file->private_data;
	struct ub_lrpc_ctl_channel *ch = ctx->channel;
	struct task_struct *shadow = NULL;
	struct task_struct *caller = NULL;
	struct mm_struct *mm = NULL;

	if (ch) {
		mutex_lock(&ch->handoff_lock);
		if (ch->shadow_owner == ctx) {
			shadow = ch->shadow_task;
			caller = ch->caller_task;
			mm = ch->shadow_mm;
			ch->shadow_owner = NULL;
			ch->shadow_task = NULL;
			ch->caller_task = NULL;
			ch->shadow_mm = NULL;
			ch->shadow_cpu = -1;
			ch->epoch = 0;
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
	if (shadow)
		put_task_struct(shadow);
	if (caller)
		put_task_struct(caller);
	kfree(ctx);
	return 0;
}

static int ub_lrpc_ctl_bind(struct ub_lrpc_ctl_file *ctx,
			    struct ub_lrpc_bind *bind)
{
	struct ub_lrpc_ctl_channel *ch;

	if (ctx->role != UB_LRPC_ROLE_CALLER &&
	    ctx->role != UB_LRPC_ROLE_SHADOW)
		return -EPERM;
	if (ctx->bound)
		return -EBUSY;
	if (!bind->expected_epoch || !bind->procedure_id ||
	    bind->procedure_id > UB_LRPC_MAX_PROCS)
		return -EINVAL;
	ctx->procedure_id = bind->procedure_id;
	ctx->proc_index = bind->procedure_id - 1;
	ctx->epoch = bind->expected_epoch;
	ctx->channel = &ctx->dev->channel[ctx->proc_index];
	ch = ctx->channel;
	mutex_lock(&ch->handoff_lock);
	if (ctx->role == UB_LRPC_ROLE_CALLER && ch->shadow_owner &&
	    ch->epoch != ctx->epoch) {
		mutex_unlock(&ch->handoff_lock);
		ctx->channel = NULL;
		return -ESTALE;
	}
	mutex_unlock(&ch->handoff_lock);
	ctx->bound = true;
	bind->entry_offset = 0;
	bind->astack_size = UB_LRPC_ASTACK_SLOT_SIZE;
	bind->astack_offset = (u64)ctx->proc_index * UB_LRPC_ASTACK_SLOT_SIZE;
	return 0;
}

static long ub_lrpc_ctl_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct ub_lrpc_ctl_file *ctx = file->private_data;

	switch (cmd) {
	case UB_LRPC_IOC_SET_ROLE: {
		struct ub_lrpc_set_role role;
		if (copy_from_user(&role, (void __user *)arg, sizeof(role)))
			return -EFAULT;
		if (role.role != UB_LRPC_ROLE_CALLER &&
		    role.role != UB_LRPC_ROLE_SHADOW)
			return -EINVAL;
		if (ctx->role != UB_LRPC_ROLE_NONE)
			return -EBUSY;
		ctx->role = role.role;
		return 0;
	}
	case UB_LRPC_IOC_BIND: {
		struct ub_lrpc_bind bind;
		int ret;
		if (copy_from_user(&bind, (void __user *)arg, sizeof(bind)))
			return -EFAULT;
		ret = ub_lrpc_ctl_bind(ctx, &bind);
		if (ret)
			return ret;
		return copy_to_user((void __user *)arg, &bind, sizeof(bind)) ?
			-EFAULT : 0;
	}
	case UB_LRPC_IOC_INFO: {
		struct ub_lrpc_info info = { .role = ctx->role,
			.shadow_cpu = -1, .cache_mode = UB_LRPC_CACHE_CACHED };
		if (ctx->channel) {
			struct ub_lrpc_ctl_channel *ch = ctx->channel;
			mutex_lock(&ch->handoff_lock);
			info.code_epoch = ch->epoch;
			info.ready = ch->shadow_owner != NULL;
			info.shadow_ready = ch->shadow_owner != NULL;
			info.shadow_cpu = ch->shadow_cpu;
			info.shadow_pid = ch->shadow_task ?
				task_pid_nr(ch->shadow_task) : 0;
			mutex_unlock(&ch->handoff_lock);
		}
		return copy_to_user((void __user *)arg, &info, sizeof(info)) ?
			-EFAULT : 0;
	}
	case UB_LRPC_IOC_REGISTER_SHADOW: {
		struct ub_lrpc_ctl_channel *ch = ctx->channel;
		if (ctx->role != UB_LRPC_ROLE_SHADOW || !ctx->bound ||
		    !current->mm || current->nr_cpus_allowed != 1)
			return -EPERM;
		mutex_lock(&ch->handoff_lock);
		if (ch->shadow_owner) {
			mutex_unlock(&ch->handoff_lock);
			return -EBUSY;
		}
		get_task_struct(current);
		mmgrab(current->mm);
		ch->shadow_owner = ctx;
		ch->shadow_task = current;
		ch->shadow_mm = current->mm;
		ch->shadow_cpu = task_cpu(current);
		ch->epoch = ctx->epoch;
		mutex_unlock(&ch->handoff_lock);
		return 0;
	}
	case UB_LRPC_IOC_WAIT_CALL: {
		struct ub_lrpc_handoff handoff;
		struct ub_lrpc_ctl_channel *ch = ctx->channel;
		if (ctx->role != UB_LRPC_ROLE_SHADOW || !ctx->bound)
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
		struct task_struct *old_caller;
		struct ub_lrpc_ctl_channel *ch = ctx->channel;
		if (ctx->role != UB_LRPC_ROLE_CALLER || !ctx->bound)
			return -EPERM;
		if (copy_from_user(&handoff, (void __user *)arg, sizeof(handoff)))
			return -EFAULT;
		mutex_lock(&ch->handoff_lock);
		if (!ch->shadow_owner) {
			mutex_unlock(&ch->handoff_lock);
			return -ENOTCONN;
		}
		if (ch->epoch != ctx->epoch) {
			mutex_unlock(&ch->handoff_lock);
			return -ESTALE;
		}
		if (current->mm == ch->shadow_mm ||
		    current->nr_cpus_allowed != 1 ||
		    task_cpu(current) != ch->shadow_cpu) {
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
		handoff.procedure_id = ctx->procedure_id;
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
		struct ub_lrpc_ctl_channel *ch = ctx->channel;
		if (ctx->role != UB_LRPC_ROLE_SHADOW || !ctx->bound)
			return -EPERM;
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
		schedule();
		__set_current_state(TASK_RUNNING);
		if (signal_pending(current))
			return -ERESTARTSYS;
		return 0;
	}
	case UB_LRPC_IOC_PUBLISH:
		return -EOPNOTSUPP;
	default:
		return -ENOTTY;
	}
}

static int ub_lrpc_ctl_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ub_lrpc_ctl_file *ctx = file->private_data;
	u64 off = (u64)vma->vm_pgoff << PAGE_SHIFT;
	u64 len = vma->vm_end - vma->vm_start;
	u64 expected;

	if (!ctx->bound || (ctx->role != UB_LRPC_ROLE_CALLER &&
	    ctx->role != UB_LRPC_ROLE_SHADOW) || (vma->vm_flags & VM_EXEC))
		return -EPERM;
	expected = (u64)ctx->proc_index * UB_LRPC_ASTACK_SLOT_SIZE;
	if (off != expected || len != UB_LRPC_ASTACK_SLOT_SIZE)
		return -EINVAL;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	return remap_vmalloc_range(vma, ctx->dev->astacks,
				  off >> PAGE_SHIFT);
}

static const struct file_operations ub_lrpc_ctl_fops = {
	.owner = THIS_MODULE,
	.open = ub_lrpc_ctl_open,
	.release = ub_lrpc_ctl_release,
	.unlocked_ioctl = ub_lrpc_ctl_ioctl,
	.mmap = ub_lrpc_ctl_mmap,
	.llseek = no_llseek,
};

static int __init ub_lrpc_ctl_init(void)
{
	int i;
	int ret;

	if (UB_LRPC_ASTACK_SLOT_SIZE % PAGE_SIZE)
		return -EINVAL;
	ub_lrpc_ctl.astacks = vmalloc_user(UB_LRPC_ASTACK_SIZE);
	if (!ub_lrpc_ctl.astacks)
		return -ENOMEM;
	for (i = 0; i < UB_LRPC_MAX_PROCS; i++) {
		mutex_init(&ub_lrpc_ctl.channel[i].handoff_lock);
		ub_lrpc_ctl.channel[i].shadow_cpu = -1;
	}
	ub_lrpc_ctl.misc.minor = MISC_DYNAMIC_MINOR;
	ub_lrpc_ctl.misc.name = UB_LRPC_CTL_NAME;
	ub_lrpc_ctl.misc.fops = &ub_lrpc_ctl_fops;
	ret = misc_register(&ub_lrpc_ctl.misc);
	if (ret) {
		vfree(ub_lrpc_ctl.astacks);
		ub_lrpc_ctl.astacks = NULL;
		return ret;
	}
	pr_info("ub_lrpc_ctl: local A-stacks registered at /dev/%s\n",
		UB_LRPC_CTL_NAME);
	return 0;
}

static void __exit ub_lrpc_ctl_exit(void)
{
	misc_deregister(&ub_lrpc_ctl.misc);
	vfree(ub_lrpc_ctl.astacks);
}

module_init(ub_lrpc_ctl_init);
module_exit(ub_lrpc_ctl_exit);

MODULE_DESCRIPTION("Local handoff control for UBS Memory LRPC");
MODULE_AUTHOR("eRPC-LRPC prototype");
MODULE_LICENSE("GPL");
