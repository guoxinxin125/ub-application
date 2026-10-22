#ifndef UB_DOMAIN_SERVICES_H
#define UB_DOMAIN_SERVICES_H
#include <lrpc/ub_social.h>
extern const unsigned char us_lb_forward_start[], us_lb_forward_end[];
extern const unsigned char us_nginx_forward_start[], us_nginx_forward_end[];
extern const unsigned char us_nginx_forward_write_start[], us_nginx_forward_write_end[];
extern const unsigned char us_nginx_return_start[], us_nginx_return_end[];
extern const unsigned char us_lb_return_start[], us_lb_return_end[];
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
static inline const unsigned char *ud_profile_service(unsigned int profile,
                                                       unsigned int d, unsigned long *size)
{
    const unsigned char *start, *end;
    if (profile == UD_PROFILE_MICRO) return ud_service(d, size);
    if (profile == UD_PROFILE_SOCIAL_WRITE && d == 1) {
        *size = (unsigned long)(us_nginx_forward_write_end - us_nginx_forward_write_start);
        return us_nginx_forward_write_start;
    }
    switch (d) {
    case 0: start=us_lb_forward_start; end=us_lb_forward_end; break;
    case 1: start=us_nginx_forward_start; end=us_nginx_forward_end; break;
    case 2: start=us_nginx_return_start; end=us_nginx_return_end; break;
    default: start=us_lb_return_start; end=us_lb_return_end; break;
    }
    *size = (unsigned long)end - (unsigned long)start;
    return start;
}
#endif
