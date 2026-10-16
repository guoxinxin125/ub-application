#ifndef UB_DOMAIN_SERVICES_H
#define UB_DOMAIN_SERVICES_H
extern const unsigned char ud_direct_start[], ud_direct_end[];
extern const unsigned char ud_middle_start[], ud_middle_end[];
extern const unsigned char ud_leaf_start[], ud_leaf_end[];
extern const unsigned char ud_query_start[], ud_query_end[];
static inline const unsigned char *ud_service(unsigned int d, unsigned long *size)
{
    const unsigned char *start, *end;
    switch (d) {
    case 0: start = ud_direct_start; end = ud_direct_end; break;
    case 1: start = ud_middle_start; end = ud_middle_end; break;
    case 2: start = ud_leaf_start; end = ud_leaf_end; break;
    default: start = ud_query_start; end = ud_query_end; break;
    }
    *size = (unsigned long)end - (unsigned long)start;
    return start;
}
#endif
