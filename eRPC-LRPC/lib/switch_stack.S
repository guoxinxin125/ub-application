#if defined(__aarch64__)
#include "switch_stack_aarch64.S"
#elif defined(__x86_64__)
#include "switch_stack_x86_64.S"
#else
#error "Unsupported LRPC architecture"
#endif
