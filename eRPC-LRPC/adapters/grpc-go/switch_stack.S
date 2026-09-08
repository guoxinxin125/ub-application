#if defined(__aarch64__)
#include "../../lib/switch_stack_aarch64.S"
#else
#include "../../lib/switch_stack_x86_64.S"
#endif
