//go:build ubsm

package main

/*
#cgo CFLAGS: -I${SRCDIR}/../../include -I/usr/local/ubs_mem/include
#cgo LDFLAGS: -L${SRCDIR}/../../build-ub -L/usr/local/ubs_mem/lib -Wl,-rpath,/usr/local/ubs_mem/lib -llrpc -lubsm_sdk
#include <stdint.h>
#include <stdlib.h>
#include <lrpc/lrpc_ubsm.h>

static struct lrpc_ubsm_region geo_region;
static volatile uint64_t *geo_service_value;

static int geo_ubsm_open(const char *name) {
	struct lrpc_ubsm_proc_desc proc;
	int ret = lrpc_ubsm_remote_open(&geo_region, name,
					LRPC_UBSM_REGION_SIZE);
	if (ret != 0)
		return ret;
	ret = lrpc_ubsm_lookup_procedure(&geo_region, 4, 1, &proc);
	if (ret != 0) {
		(void)lrpc_ubsm_close(&geo_region, 0);
		return ret;
	}
	geo_service_value = lrpc_ubsm_at(&geo_region, proc.data_offset,
					 sizeof(*geo_service_value));
	if (geo_service_value == NULL) {
		(void)lrpc_ubsm_close(&geo_region, 0);
		return -1;
	}
	return 0;
}

static uint64_t geo_ubsm_load(void) {
	return __atomic_load_n(geo_service_value, __ATOMIC_ACQUIRE);
}

static void geo_ubsm_close(void) {
	geo_service_value = NULL;
	(void)lrpc_ubsm_close(&geo_region, 0);
}
*/
import "C"

import (
	"fmt"
	"os"
	"unsafe"
)

func openServiceState() error {
	name := os.Getenv("LRPC_UBSM_NAME")
	if name == "" {
		name = "lrpc_ub_demo"
	}
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	if rc := int(C.geo_ubsm_open(cname)); rc != 0 {
		return fmt.Errorf("DeathStar Geo UBS Memory import failed: %d", rc)
	}
	return nil
}

func closeServiceState() { C.geo_ubsm_close() }

func validateServiceState() error {
	if value := uint64(C.geo_ubsm_load()); value != 100 {
		return fmt.Errorf("unexpected remote Geo service value: %d", value)
	}
	return nil
}
