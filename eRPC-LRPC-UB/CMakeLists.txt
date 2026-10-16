cmake_minimum_required(VERSION 3.16)
project(erpc_lrpc_ub LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror -O2)
set(UBLRPC_ROLE "caller" CACHE STRING "Build caller or owner")
set_property(CACHE UBLRPC_ROLE PROPERTY STRINGS caller owner)

set(UBSM_INCLUDE_DIR "/usr/local/ubs_mem/include" CACHE PATH "UBSM headers")
set(UBSM_LIBRARY "/usr/local/ubs_mem/lib/libubsm_sdk.so" CACHE FILEPATH "UBSM SDK library")
if(NOT EXISTS "${UBSM_INCLUDE_DIR}/ubs_mem.h" OR
   NOT EXISTS "${UBSM_INCLUDE_DIR}/ubs_mem_def.h" OR
   NOT EXISTS "${UBSM_LIBRARY}")
    message(FATAL_ERROR "Set UBSM_INCLUDE_DIR and UBSM_LIBRARY to the installed UBSM SDK")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    message(FATAL_ERROR "This package builds on AArch64 UB hosts only")
endif()

if(UBLRPC_ROLE STREQUAL "owner")
    add_executable(lrpc-ub-domain-owner
        demo/ub/domain_owner.c kernel/ub-domain/services.S lib/backend_ubsm.c)
    add_executable(lrpc-ub-pa-owner
        demo/ub/pa_owner.c lib/backend_ubsm.c)
    target_include_directories(lrpc-ub-domain-owner PRIVATE include include/uapi
        "${UBSM_INCLUDE_DIR}")
    target_include_directories(lrpc-ub-pa-owner PRIVATE include include/uapi
        "${UBSM_INCLUDE_DIR}")
    target_link_libraries(lrpc-ub-domain-owner PRIVATE "${UBSM_LIBRARY}")
    target_link_libraries(lrpc-ub-pa-owner PRIVATE "${UBSM_LIBRARY}")
    get_filename_component(UBSM_LIBRARY_DIR "${UBSM_LIBRARY}" DIRECTORY)
    set_target_properties(lrpc-ub-domain-owner lrpc-ub-pa-owner PROPERTIES
        BUILD_RPATH "${UBSM_LIBRARY_DIR}")
elseif(UBLRPC_ROLE STREQUAL "caller")
    find_path(OBMM_INCLUDE_DIR NAMES libobmm.h
        HINTS /usr/local/include /usr/local/ubs_mem/include)
    find_library(OBMM_LIBRARY NAMES obmm
        HINTS /usr/local/ubs_mem/lib /usr/local/lib64 /usr/local/lib)
    if(NOT OBMM_INCLUDE_DIR OR NOT OBMM_LIBRARY)
        message(FATAL_ERROR "Set OBMM_INCLUDE_DIR and OBMM_LIBRARY to installed OBMM headers/library")
    endif()
    add_library(lrpc STATIC lib/ub_domain_api.c lib/ub_domain_client.c
        lib/backend_ubsm.c lib/ubsm_pa.c)
    target_include_directories(lrpc PUBLIC include include/uapi "${UBSM_INCLUDE_DIR}")
    target_include_directories(lrpc PRIVATE "${OBMM_INCLUDE_DIR}")
    target_link_libraries(lrpc PUBLIC "${UBSM_LIBRARY}" "${OBMM_LIBRARY}")
    add_executable(lrpc-ub-domain-bench demo/ub/domain_bench.c)
    target_link_libraries(lrpc-ub-domain-bench PRIVATE lrpc)
    add_executable(lrpc-ub-domain-tlb-probe demo/ub/domain_tlb_probe.c)
    target_link_libraries(lrpc-ub-domain-tlb-probe PRIVATE lrpc)
    add_executable(lrpc-ub-pa-probe demo/ub/pa_probe.c)
    target_link_libraries(lrpc-ub-pa-probe PRIVATE lrpc)
    get_filename_component(UBSM_LIBRARY_DIR "${UBSM_LIBRARY}" DIRECTORY)
    get_filename_component(OBMM_LIBRARY_DIR "${OBMM_LIBRARY}" DIRECTORY)
    set_target_properties(lrpc-ub-domain-bench lrpc-ub-domain-tlb-probe
        lrpc-ub-pa-probe PROPERTIES
        BUILD_RPATH "${UBSM_LIBRARY_DIR};${OBMM_LIBRARY_DIR}")
    enable_testing()
    add_executable(test-ub-domain-layout tests/test_ub_domain_layout.c)
    target_include_directories(test-ub-domain-layout PRIVATE include include/uapi)
    target_compile_options(test-ub-domain-layout PRIVATE -UNDEBUG)
    add_test(NAME ub-domain-layout COMMAND test-ub-domain-layout)
else()
    message(FATAL_ERROR "UBLRPC_ROLE must be caller or owner")
endif()
