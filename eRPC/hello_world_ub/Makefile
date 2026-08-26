CMAKE ?= cmake
BUILD_TYPE ?= Release
PERF ?= ON
JOBS ?= $(shell nproc 2>/dev/null || echo 1)

MAKEFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ERPC_ROOT ?= $(abspath $(MAKEFILE_DIR)/..)
BUILD_DIR ?= $(ERPC_ROOT)/build-ub
UBSM_INCLUDE_DIR ?= /usr/local/ubs_mem/include
UBSM_LIBRARY ?= /usr/local/ubs_mem/lib/libubsm_sdk.so

UB_TARGETS := erpc_ub_manager hello_ub_server hello_ub_client \
	hello_ub_multi_server hello_ub_multi_client

.PHONY: all configure server client multi-server multi-client manager clean help

all: configure
	$(CMAKE) --build "$(BUILD_DIR)" -j "$(JOBS)" --target $(UB_TARGETS)

configure:
	$(CMAKE) -S "$(ERPC_ROOT)" -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DPERF="$(PERF)" \
		-DTRANSPORT=ub \
		-DUBSM_INCLUDE_DIR="$(UBSM_INCLUDE_DIR)" \
		-DUBSM_LIBRARY="$(UBSM_LIBRARY)"

server: configure
	$(CMAKE) --build "$(BUILD_DIR)" -j "$(JOBS)" --target hello_ub_server

client: configure
	$(CMAKE) --build "$(BUILD_DIR)" -j "$(JOBS)" --target hello_ub_client

multi-server: configure
	$(CMAKE) --build "$(BUILD_DIR)" -j "$(JOBS)" --target hello_ub_multi_server

multi-client: configure
	$(CMAKE) --build "$(BUILD_DIR)" -j "$(JOBS)" --target hello_ub_multi_client

manager: configure
	$(CMAKE) --build "$(BUILD_DIR)" -j "$(JOBS)" --target erpc_ub_manager

clean:
	@if [ -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		$(CMAKE) --build "$(BUILD_DIR)" --target clean; \
	else \
		echo "Nothing to clean: $(BUILD_DIR) is not configured"; \
	fi

help:
	@echo "UB hello_world build targets:"
	@echo "  make             Configure and build single, multi, and manager targets"
	@echo "  make server      Build hello_ub_server"
	@echo "  make client      Build hello_ub_client"
	@echo "  make multi-server Build hello_ub_multi_server"
	@echo "  make multi-client Build hello_ub_multi_client"
	@echo "  make manager     Build erpc_ub_manager"
	@echo "  make clean       Clean the existing CMake build tree"
	@echo ""
	@echo "Override paths when needed:"
	@echo "  make UBSM_INCLUDE_DIR=/path/to/include UBSM_LIBRARY=/path/to/libubsm_sdk.so"
	@echo "  make BUILD_DIR=/path/to/build JOBS=32 BUILD_TYPE=Debug PERF=OFF"
