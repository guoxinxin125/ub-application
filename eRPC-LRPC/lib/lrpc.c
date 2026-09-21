#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sched.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <lrpc/lrpc.h>
#include <lrpc/lrpc_abi_offsets.h>
#include "remote_app_backend.h"
#ifdef LRPC_UB_DOMAIN
#include "ub_domain_client.h"
#endif

typedef char lrpc_request_size_offset_check[
	offsetof(struct lrpc_astack, request_size) ==
	LRPC_ASTACK_REQUEST_SIZE_OFFSET ? 1 : -1];
typedef char lrpc_response_size_offset_check[
	offsetof(struct lrpc_astack, response_size) ==
	LRPC_ASTACK_RESPONSE_SIZE_OFFSET ? 1 : -1];
typedef char lrpc_status_offset_check[
	offsetof(struct lrpc_astack, status) ==
	LRPC_ASTACK_STATUS_OFFSET ? 1 : -1];
typedef char lrpc_service_data_offset_check[
	offsetof(struct lrpc_astack, service_data) ==
	LRPC_ASTACK_SERVICE_DATA_OFFSET ? 1 : -1];
typedef char lrpc_payload_offset_check[
	offsetof(struct lrpc_astack, payload) ==
	LRPC_ASTACK_PAYLOAD_OFFSET ? 1 : -1];

static void *map_region(int fd, uint64_t off, size_t len, int prot)
{
	void *p = mmap(NULL, len, prot, MAP_SHARED, fd, (off_t)off);
	return p == MAP_FAILED ? NULL : p;
}

static uint64_t lrpc_code_hash(const void *data, size_t size)
{
	const uint8_t *bytes = data;
	uint64_t hash = UINT64_C(14695981039346656037);

	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static int lrpc_localize_code(int fd, const struct ub_lrpc_bind *bind,
			      void **local_code_out,
			      size_t *local_mapping_size_out)
{
	void *remote_code = NULL;
	void *remote_exec;
	void *local_code = MAP_FAILED;
	size_t mapping_size;
	long page_size;
	int saved_errno;

	if (!bind || !local_code_out || !local_mapping_size_out ||
	    !bind->code_size || bind->entry_offset > UB_LRPC_CODE_SIZE ||
	    bind->code_size > UB_LRPC_CODE_SIZE - bind->entry_offset) {
		errno = EPROTO;
		return -1;
	}
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0 ||
	    bind->code_size > SIZE_MAX - ((size_t)page_size - 1)) {
		errno = EOVERFLOW;
		return -1;
	}
	mapping_size = ((size_t)bind->code_size + (size_t)page_size - 1) &
		~((size_t)page_size - 1);

	/* Enforce that BAR2 is only a temporary read-only transport source. */
	remote_exec = mmap(NULL, UB_LRPC_CODE_SIZE, PROT_READ | PROT_EXEC,
			   MAP_SHARED, fd, (off_t)UB_LRPC_CODE_OFFSET);
	if (remote_exec != MAP_FAILED) {
		munmap(remote_exec, UB_LRPC_CODE_SIZE);
		errno = EPROTO;
		return -1;
	}
	if (errno != EPERM && errno != EACCES)
		return -1;
	remote_code = map_region(fd, UB_LRPC_CODE_OFFSET, UB_LRPC_CODE_SIZE,
				 PROT_READ);
	if (!remote_code)
		return -1;
	local_code = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (local_code == MAP_FAILED)
		goto fail;
	memcpy(local_code, (const uint8_t *)remote_code + bind->entry_offset,
	       (size_t)bind->code_size);
	if (lrpc_code_hash(local_code, (size_t)bind->code_size) !=
	    bind->code_hash) {
		errno = EBADMSG;
		goto fail;
	}
	if (munmap(remote_code, UB_LRPC_CODE_SIZE))
		goto fail;
	remote_code = NULL;
	__builtin___clear_cache((char *)local_code,
				(char *)local_code + bind->code_size);
	if (mprotect(local_code, mapping_size, PROT_READ | PROT_EXEC))
		goto fail;

	*local_code_out = local_code;
	*local_mapping_size_out = mapping_size;
	return 0;

fail:
	saved_errno = errno;
	if (local_code != MAP_FAILED)
		munmap(local_code, mapping_size);
	if (remote_code)
		munmap(remote_code, UB_LRPC_CODE_SIZE);
	errno = saved_errno;
	return -1;
}

int lrpc_publish(int fd, const void *code, size_t code_size,
		 uint32_t procedure_id, uint64_t epoch)
{
	return lrpc_publish_services(fd, code, code_size, &procedure_id, 1, epoch);
}

int lrpc_publish_services(int fd, const void *code, size_t code_size,
			  const uint32_t *procedure_ids, size_t count,
			  uint64_t epoch)
{
	struct ub_lrpc_set_role role = { .role = UB_LRPC_ROLE_PUBLISHER };
	struct ub_lrpc_publish pub;
	void *dst;

	if (!code || !code_size || code_size > UB_LRPC_CODE_SIZE ||
	    !procedure_ids || !count || count > UB_LRPC_MAX_PROCS) {
		errno = EINVAL;
		return -1;
	}
	if (ioctl(fd, UB_LRPC_IOC_SET_ROLE, &role))
		return -1;
	dst = map_region(fd, UB_LRPC_CODE_OFFSET, UB_LRPC_CODE_SIZE,
			 PROT_READ | PROT_WRITE);
	if (!dst)
		return -1;
	memset(dst, 0xcc, UB_LRPC_CODE_SIZE);
	memcpy(dst, code, code_size);
	__sync_synchronize();
	if (munmap(dst, UB_LRPC_CODE_SIZE))
		return -1;

	memset(&pub, 0, sizeof(pub));
	pub.code_epoch = epoch;
	pub.image_size = code_size;
	pub.num_procs = (uint32_t)count;
	for (size_t i = 0; i < count; i++) {
		pub.proc[i].procedure_id = procedure_ids[i];
		pub.proc[i].flags = UB_LRPC_PROC_PUBLISHED_CODE;
		pub.proc[i].code_offset = 0;
		pub.proc[i].code_size = code_size;
		pub.proc[i].code_hash = lrpc_code_hash(code, code_size);
		pub.proc[i].astack_size = sizeof(struct lrpc_astack);
	}
	return ioctl(fd, UB_LRPC_IOC_PUBLISH, &pub);
}

int lrpc_bind(struct lrpc_handle *h, const char *device,
	      uint32_t procedure_id, uint64_t expected_epoch)
{
	struct ub_lrpc_set_role role = { .role = UB_LRPC_ROLE_CALLER };
	struct ub_lrpc_bind bind = { .procedure_id = procedure_id,
		.expected_epoch = expected_epoch };
	struct ub_lrpc_info info;
	cpu_set_t set;

	if (getenv("LRPC_EXECUTION_BACKEND") &&
	    !strcmp(getenv("LRPC_EXECUTION_BACKEND"), "ub-remote-domain")) {
#ifdef LRPC_UB_DOMAIN
		return ud_lrpc_bind(h, procedure_id, expected_epoch);
#else
		errno = ENOTSUP;
		return -1;
#endif
	}
	if (h && ra_backend_requested()) {
		if (expected_epoch != 1) { errno = ESTALE; return -1; }
		return ra_backend_bind(h, procedure_id);
	}

	if (!h || !device) {
		errno = EINVAL;
		return -1;
	}
	memset(h, 0, sizeof(*h));
	h->fd = open(device, O_RDWR | O_CLOEXEC);
	if (h->fd < 0)
		return -1;
	if (ioctl(h->fd, UB_LRPC_IOC_SET_ROLE, &role) ||
	    ioctl(h->fd, UB_LRPC_IOC_BIND, &bind))
		goto fail;
	memset(&info, 0, sizeof(info));
	if (ioctl(h->fd, UB_LRPC_IOC_INFO, &info) || !info.shadow_ready ||
	    info.shadow_cpu < 0) {
		errno = ENOTCONN;
		goto fail;
	}
	CPU_ZERO(&set);
	CPU_SET(info.shadow_cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		goto fail;
	h->astack_offset = bind.astack_offset;
	h->shadow_cpu = info.shadow_cpu;
	h->astack_map = map_region(h->fd, h->astack_offset,
				  UB_LRPC_ASTACK_SLOT_SIZE,
				  PROT_READ | PROT_WRITE);
	if (!h->astack_map)
		goto fail;
	h->epoch = bind.expected_epoch;
	return 0;
fail:
	lrpc_close(h);
	return -1;
}

int lrpc_pin(struct lrpc_handle *h)
{
	cpu_set_t set;

	if (!h || h->fd < 0 || h->shadow_cpu < 0) {
		errno = EINVAL;
		return -1;
	}
	CPU_ZERO(&set);
	CPU_SET(h->shadow_cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

int lrpc_invoke(struct lrpc_handle *h, struct lrpc_astack *call)
{
	struct lrpc_astack *shared = NULL;
	struct ub_lrpc_handoff handoff;
#ifdef LRPC_UB_DOMAIN
	if (h && h->fd >= 0 && h->astack_offset == UD_HANDLE_MARKER)
		return ud_lrpc_invoke(h, call);
#endif
	if (h && h->fd >= 0 && h->astack_offset == UINT64_MAX)
		return ra_backend_invoke(h, call);

	if (!h || !call || !h->astack_map) {
		errno = EINVAL;
		return -1;
	}
	if (call->request_size > LRPC_PAYLOAD_MAX) {
		errno = EMSGSIZE;
		return -1;
	}
	shared = h->astack_map;
	memcpy(shared, call, offsetof(struct lrpc_astack, payload));
	memcpy(shared->payload, call->payload, call->request_size);
	shared->caller_cpu_before = (uint64_t)sched_getcpu();
	shared->caller_cpu_after = UINT64_MAX;
	shared->caller_pid = (uint64_t)getpid();
	__sync_synchronize();
	memset(&handoff, 0, sizeof(handoff));
	if (ioctl(h->fd, UB_LRPC_IOC_CALL, &handoff))
		return -1;
	__sync_synchronize();
	shared->caller_cpu_after = (uint64_t)sched_getcpu();
	shared->call_enter_ns = handoff.call_enter_ns;
	shared->shadow_dispatch_ns = handoff.shadow_dispatch_ns;
	shared->shadow_return_ns = handoff.shadow_return_ns;
	shared->caller_resume_ns = handoff.caller_resume_ns;
	if (shared->response_size > LRPC_PAYLOAD_MAX) {
		errno = EPROTO;
		return -1;
	}
	memcpy(call, shared, offsetof(struct lrpc_astack, payload));
	memcpy(call->payload, shared->payload, shared->response_size);
	call->call_enter_ns = shared->call_enter_ns;
	call->shadow_dispatch_ns = shared->shadow_dispatch_ns;
	call->shadow_return_ns = shared->shadow_return_ns;
	call->caller_resume_ns = shared->caller_resume_ns;
	return handoff.result;
}

int lrpc_invoke_bytes(struct lrpc_handle *h, uint32_t procedure_id,
		      const void *request, size_t request_size,
		      void *response, size_t *response_size)
{
	struct lrpc_astack *call;
	int ret;

	if (!h || (!request && request_size) || !response_size ||
	    request_size > LRPC_PAYLOAD_MAX ||
	    (*response_size && !response)) {
		errno = EINVAL;
		return -1;
	}
	call = calloc(1, sizeof(*call));
	if (!call)
		return -1;
	call->abi = LRPC_ASTACK_ABI;
	call->procedure_id = procedure_id;
	call->request_size = request_size;
	if (request_size)
		memcpy(call->payload, request, request_size);
	ret = lrpc_invoke(h, call);
	if (!ret && !call->status) {
		if (*response_size < call->response_size) {
			*response_size = call->response_size;
			errno = ENOBUFS;
			ret = -1;
		} else {
			memcpy(response, call->payload, call->response_size);
			*response_size = call->response_size;
		}
	} else if (!ret) {
		ret = -(int)call->status;
	}
	free(call);
	return ret;
}

int lrpc_shadow_run(const char *device, uint32_t procedure_id,
		    uint64_t expected_epoch)
{
	struct ub_lrpc_set_role role = { .role = UB_LRPC_ROLE_SHADOW };
	struct ub_lrpc_bind bind = { .procedure_id = procedure_id,
		.expected_epoch = expected_epoch };
	struct ub_lrpc_info info;
	struct ub_lrpc_handoff handoff;
	struct lrpc_astack *shared = NULL;
	void *local_code = NULL, *data = NULL, *estack = MAP_FAILED;
	size_t local_code_mapping_size = 0;
	size_t estack_size = 256 * 1024;
	cpu_set_t set;
	const char *cpu_env;
	int cpu = 1, fd = -1, ret = -1;

	if (!device) {
		errno = EINVAL;
		return -1;
	}
	cpu_env = getenv("LRPC_SHADOW_CPU");
	if (cpu_env)
		cpu = atoi(cpu_env);
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		return -1;
	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0 || ioctl(fd, UB_LRPC_IOC_SET_ROLE, &role) ||
	    ioctl(fd, UB_LRPC_IOC_BIND, &bind))
		goto out;
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, UB_LRPC_IOC_INFO, &info))
		goto out;
	if (lrpc_localize_code(fd, &bind, &local_code,
			       &local_code_mapping_size))
		goto out;
	shared = map_region(fd, bind.astack_offset, UB_LRPC_ASTACK_SLOT_SIZE,
			    PROT_READ | PROT_WRITE);
	data = map_region(fd, UB_LRPC_DATA_OFFSET, 4096,
			  PROT_READ | PROT_WRITE);
	estack = mmap(NULL, estack_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (!shared || !data || estack == MAP_FAILED)
		goto out;
	printf("LRPC_CODE_LOCALIZED proc=%u remote_offset=%llu bytes=%llu "
	       "hash=0x%llx local_entry=%p permissions=rx remote_exec=denied "
	       "remote_unmapped=1\n",
	       procedure_id, (unsigned long long)bind.entry_offset,
	       (unsigned long long)bind.code_size,
	       (unsigned long long)bind.code_hash, local_code);
	fflush(stdout);
	if (ioctl(fd, UB_LRPC_IOC_REGISTER_SHADOW))
		goto out;
	printf("LRPC_SHADOW_REGISTERED pid=%d cpu=%d mm=separate\n",
	       getpid(), sched_getcpu());
	fflush(stdout);
	for (;;) {
		memset(&handoff, 0, sizeof(handoff));
		if (ioctl(fd, UB_LRPC_IOC_WAIT_CALL, &handoff))
			goto out;
		shared->service_data = (uint64_t)(uintptr_t)data;
		shared->shadow_pid = (uint64_t)getpid();
		__sync_synchronize();
		handoff.result = (int)lrpc_call_on_stack(
			local_code, shared,
			(uint8_t *)estack + estack_size);
		__sync_synchronize();
		if (ioctl(fd, UB_LRPC_IOC_RETURN, &handoff))
			goto out;
	}
out:
	if (estack != MAP_FAILED)
		munmap(estack, estack_size);
	if (data)
		munmap(data, 4096);
	if (shared)
		munmap(shared, UB_LRPC_ASTACK_SLOT_SIZE);
	if (local_code)
		munmap(local_code, local_code_mapping_size);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int lrpc_shadow_serve_impl(const char *device, uint32_t procedure_id,
				  uint64_t expected_epoch,
				  lrpc_shadow_handler_fn handler,
				  int switch_stack, void *service_data)
{
	struct ub_lrpc_set_role role = { .role = UB_LRPC_ROLE_SHADOW };
	struct ub_lrpc_bind bind = { .procedure_id = procedure_id,
		.expected_epoch = expected_epoch };
	struct ub_lrpc_handoff handoff;
	struct lrpc_astack *shared = NULL;
	void *estack = MAP_FAILED;
	const size_t estack_size = 256 * 1024;
	cpu_set_t set;
	const char *cpu_env;
	int cpu = 1, fd = -1, ret = -1;

	if (!device || !handler) {
		errno = EINVAL;
		return -1;
	}
	cpu_env = getenv("LRPC_SHADOW_CPU");
	if (cpu_env)
		cpu = atoi(cpu_env);
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		return -1;
	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0 || ioctl(fd, UB_LRPC_IOC_SET_ROLE, &role) ||
	    ioctl(fd, UB_LRPC_IOC_BIND, &bind))
		goto out;
	shared = map_region(fd, bind.astack_offset, UB_LRPC_ASTACK_SLOT_SIZE,
			    PROT_READ | PROT_WRITE);
	if (switch_stack)
		estack = mmap(NULL, estack_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (!shared || (switch_stack && estack == MAP_FAILED) ||
	    ioctl(fd, UB_LRPC_IOC_REGISTER_SHADOW))
		goto out;
	printf("LRPC_CALLBACK_SHADOW_REGISTERED proc=%u pid=%d cpu=%d\n",
	       procedure_id, getpid(), sched_getcpu());
	fflush(stdout);
	for (;;) {
		memset(&handoff, 0, sizeof(handoff));
		if (ioctl(fd, UB_LRPC_IOC_WAIT_CALL, &handoff))
			goto out;
		shared->shadow_pid = (uint64_t)getpid();
		shared->caller_cpu_in_service = (uint64_t)sched_getcpu();
		if (service_data)
			shared->service_data = (uint64_t)(uintptr_t)service_data;
		__sync_synchronize();
		if (switch_stack)
			handoff.result = (int)lrpc_call_on_stack(
				(void *)handler, shared,
				(uint8_t *)estack + estack_size);
		else
			handoff.result = (int)handler(shared);
		__sync_synchronize();
		if (ioctl(fd, UB_LRPC_IOC_RETURN, &handoff))
			goto out;
	}
out:
	if (estack != MAP_FAILED)
		munmap(estack, estack_size);
	if (shared)
		munmap(shared, UB_LRPC_ASTACK_SLOT_SIZE);
	if (fd >= 0)
		close(fd);
	return ret;
}

int lrpc_shadow_serve(const char *device, uint32_t procedure_id,
		      uint64_t expected_epoch, lrpc_shadow_handler_fn handler)
{
	return lrpc_shadow_serve_impl(device, procedure_id, expected_epoch,
				      handler, 1, NULL);
}

int lrpc_shadow_serve_with_data(const char *device, uint32_t procedure_id,
				uint64_t expected_epoch,
				lrpc_shadow_handler_fn handler,
				void *service_data)
{
	if (!service_data) {
		errno = EINVAL;
		return -1;
	}
	return lrpc_shadow_serve_impl(device, procedure_id, expected_epoch,
				      handler, 1, service_data);
}

int lrpc_shadow_serve_current_stack(const char *device, uint32_t procedure_id,
				    uint64_t expected_epoch,
				    lrpc_shadow_handler_fn handler)
{
	return lrpc_shadow_serve_impl(device, procedure_id, expected_epoch,
				      handler, 0, NULL);
}

void lrpc_close(struct lrpc_handle *h)
{
	if (!h)
		return;
#ifdef LRPC_UB_DOMAIN
	if (h->astack_offset == UD_HANDLE_MARKER) {
		ud_lrpc_close(h);
		return;
	}
#endif
	if (h->astack_map)
		munmap(h->astack_map, UB_LRPC_ASTACK_SLOT_SIZE);
	if (h->fd >= 0)
		close(h->fd);
	memset(h, 0, sizeof(*h));
	h->fd = -1;
}
