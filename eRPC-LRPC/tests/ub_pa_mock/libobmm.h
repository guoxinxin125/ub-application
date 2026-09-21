/* Test double declarations only; production uses the installed libobmm.h. */
#include <stdint.h>
typedef uint64_t mem_id;
int obmm_query_pa_by_memid(mem_id, unsigned long, unsigned long *);
int obmm_query_memid_by_pa(unsigned long, mem_id *, unsigned long *);
