#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <lrpc/ub_social_client.h>

int main(void)
{
    struct lrpc_handle handle;
    struct ud_connection_info info;
    struct us_call call = {0};
    int rc;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (us_client_open_write(&handle)) {
        perror("UB_DOMAIN_WRITE_FAIL open");
        return 1;
    }
    if (us_client_info(&handle, &info)) {
        perror("UB_DOMAIN_WRITE_FAIL info");
        us_client_close(&handle);
        return 1;
    }
    call.abi = US_ABI;
    call.direction = US_FORWARD;
    call.message.source = 1;
    call.message.request = 1;
    call.message.value = 42;
    call.message.type = US_WRITE_REQ;
    rc = us_client_call(&handle, &call);
    if (rc || call.message.ticket != 42 || call.message.action != US_ACTION_RPC ||
        call.message.target != 104 || call.entries != 2) {
        fprintf(stderr, "UB_DOMAIN_WRITE_FAIL call=%d errno=%d ticket=%llu target=%u\n",
                rc, errno, (unsigned long long)call.message.ticket, call.message.target);
        us_client_close(&handle);
        return 1;
    }
    printf("UB_DOMAIN_WRITE_A_PASS slot=%u index=1 value=%llu ticket=%llu "
           "transitions=%u b_observation=check_owner_log\n",
           info.slot, (unsigned long long)call.message.value,
           (unsigned long long)call.message.ticket, call.transitions);
    /* The B owner samples the B-local page asynchronously; this is test-only. */
    sleep(2);
    if (us_client_close(&handle)) {
        perror("UB_DOMAIN_WRITE_FAIL close");
        return 1;
    }
    return 0;
}
