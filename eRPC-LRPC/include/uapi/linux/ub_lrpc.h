#ifndef _UAPI_LINUX_UB_LRPC_H
#define _UAPI_LINUX_UB_LRPC_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define UB_LRPC_ABI_VERSION 1
#define UB_LRPC_MAGIC 0x4350524c4255ULL /* "UBLRPC" */
#define UB_LRPC_MAX_PROCS 16

#define UB_LRPC_CACHE_CACHED    0
#define UB_LRPC_CACHE_NONCACHED 1

/* ivshmem BAR2 layout. All offsets are page aligned. */
#define UB_LRPC_META_OFFSET   0x00000000ULL
#define UB_LRPC_META_SIZE     0x00001000ULL
#define UB_LRPC_CODE_OFFSET   0x00001000ULL
#define UB_LRPC_CODE_SIZE     0x00100000ULL
#define UB_LRPC_ASTACK_OFFSET 0x00101000ULL
#define UB_LRPC_ASTACK_SIZE   0x00100000ULL
#define UB_LRPC_ASTACK_SLOT_SIZE (UB_LRPC_ASTACK_SIZE / UB_LRPC_MAX_PROCS)
#define UB_LRPC_DATA_OFFSET   0x00201000ULL

enum ub_lrpc_role {
	UB_LRPC_ROLE_NONE = 0,
	UB_LRPC_ROLE_PUBLISHER = 1,
	UB_LRPC_ROLE_CALLER = 2,
	UB_LRPC_ROLE_SHADOW = 3,
};

struct ub_lrpc_proc_desc {
	__u32 procedure_id;
	__u32 flags;
	__u64 code_offset;
	__u64 code_size;
	__u64 astack_size;
};

struct ub_lrpc_metadata {
	__u64 magic;
	__u32 abi_version;
	__u32 isa;
	__u64 code_epoch;
	__u64 image_size;
	__u32 num_procs;
	__u32 ready;
	struct ub_lrpc_proc_desc proc[UB_LRPC_MAX_PROCS];
};

struct ub_lrpc_set_role {
	__u32 role;
	__u32 reserved;
};

struct ub_lrpc_publish {
	__u64 code_epoch;
	__u64 image_size;
	__u32 num_procs;
	__u32 reserved;
	struct ub_lrpc_proc_desc proc[UB_LRPC_MAX_PROCS];
};

struct ub_lrpc_bind {
	/* One successful bind is allowed per open file. Reopen to rebind. */
	__u32 procedure_id;
	__u32 reserved;
	__u64 expected_epoch;
	__u64 entry_offset;
	__u64 astack_size;
	__u64 astack_offset;
};

struct ub_lrpc_info {
	__u64 bar_start;
	__u64 bar_size;
	__u64 code_epoch;
	__u32 ready;
	__u32 role;
	__u32 shadow_ready;
	__s32 shadow_cpu;
	__u32 shadow_pid;
	__u32 cache_mode;
	__u32 reserved;
};

struct ub_lrpc_handoff {
	__u32 procedure_id;
	__s32 result;
	__u32 caller_pid;
	__s32 caller_cpu;
	__u64 call_enter_ns;
	__u64 shadow_dispatch_ns;
	__u64 shadow_return_ns;
	__u64 caller_resume_ns;
};

#define UB_LRPC_IOC_MAGIC 'L'
#define UB_LRPC_IOC_SET_ROLE _IOW(UB_LRPC_IOC_MAGIC, 0x01, struct ub_lrpc_set_role)
#define UB_LRPC_IOC_PUBLISH  _IOW(UB_LRPC_IOC_MAGIC, 0x02, struct ub_lrpc_publish)
#define UB_LRPC_IOC_BIND     _IOWR(UB_LRPC_IOC_MAGIC, 0x03, struct ub_lrpc_bind)
#define UB_LRPC_IOC_INFO     _IOR(UB_LRPC_IOC_MAGIC, 0x04, struct ub_lrpc_info)
#define UB_LRPC_IOC_REGISTER_SHADOW _IO(UB_LRPC_IOC_MAGIC, 0x05)
#define UB_LRPC_IOC_WAIT_CALL _IOR(UB_LRPC_IOC_MAGIC, 0x06, struct ub_lrpc_handoff)
#define UB_LRPC_IOC_CALL _IOWR(UB_LRPC_IOC_MAGIC, 0x07, struct ub_lrpc_handoff)
#define UB_LRPC_IOC_RETURN _IOW(UB_LRPC_IOC_MAGIC, 0x08, struct ub_lrpc_handoff)

#endif
