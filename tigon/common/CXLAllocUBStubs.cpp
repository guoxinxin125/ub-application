#include "cxlalloc.h"

#ifdef TIGON_ENABLE_UB

#include <cstdio>
#include <cstdlib>

namespace {

[[noreturn]] void reject_cxlalloc_call(const char *function_name)
{
        std::fprintf(stderr,
                "fatal: %s was called while Tigon is using the UB backend; "
                "the UB execution path must use UBMemory instead of cxlalloc\n",
                function_name);
        std::abort();
}

} // namespace

extern "C" {

void cxlalloc_init_backend(const char *)
{
        reject_cxlalloc_call(__func__);
}

void cxlalloc_set_log(const char *)
{
        reject_cxlalloc_call(__func__);
}

void cxlalloc_init(const char *, size_t, uint8_t, uint8_t, uint8_t, uint8_t)
{
        reject_cxlalloc_call(__func__);
}

bool cxlalloc_is_clean(void)
{
        reject_cxlalloc_call(__func__);
}

void cxlalloc_init_thread(size_t)
{
        reject_cxlalloc_call(__func__);
}

void *cxlalloc_malloc(size_t)
{
        reject_cxlalloc_call(__func__);
}

void cxlalloc_link(void *)
{
        reject_cxlalloc_call(__func__);
}

void cxlalloc_free(void *)
{
        reject_cxlalloc_call(__func__);
}

void cxlalloc_unlink(void *)
{
        reject_cxlalloc_call(__func__);
}

void *cxlalloc_realloc(void *, size_t)
{
        reject_cxlalloc_call(__func__);
}

void *cxlalloc_memalign(size_t, size_t)
{
        reject_cxlalloc_call(__func__);
}

void *cxlalloc_get_root(size_t)
{
        reject_cxlalloc_call(__func__);
}

void cxlalloc_set_root(size_t, void *)
{
        reject_cxlalloc_call(__func__);
}

void cxlalloc_close(void)
{
        reject_cxlalloc_call(__func__);
}

bool cxlalloc_pointer_to_offset(const void *, uint64_t *)
{
        reject_cxlalloc_call(__func__);
}

void *cxlalloc_offset_to_pointer(uint64_t)
{
        reject_cxlalloc_call(__func__);
}

} // extern "C"

#endif // TIGON_ENABLE_UB
