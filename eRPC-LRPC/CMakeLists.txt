cmake_minimum_required(VERSION 3.16)
project(erpc_lrpc LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
add_compile_options(-Wall -Wextra -Werror -O2)

set(LRPC_BACKEND "ivshmem" CACHE STRING "LRPC backend: ivshmem or ubsm")
set_property(CACHE LRPC_BACKEND PROPERTY STRINGS ivshmem ubsm)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(LRPC_STACK_SOURCE lib/switch_stack_aarch64.S)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(LRPC_STACK_SOURCE lib/switch_stack_x86_64.S)
else()
    message(FATAL_ERROR "Unsupported LRPC architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(LRPC_SOURCES lib/lrpc.c ${LRPC_STACK_SOURCE})

if(LRPC_BACKEND STREQUAL "ivshmem")
    add_compile_definitions(LRPC_BACKEND_IVSHMEM=1)
elseif(LRPC_BACKEND STREQUAL "ubsm")
    add_compile_definitions(LRPC_BACKEND_UBSM=1)
    list(APPEND LRPC_SOURCES lib/backend_ubsm.c)

    set(UBSM_INCLUDE_DIR "" CACHE PATH
        "Directory containing ubs_mem.h and ubs_mem_def.h")
    set(UBSM_LIBRARY "" CACHE FILEPATH "Path to libubsm_sdk.so")
    if(NOT UBSM_INCLUDE_DIR)
        find_path(UBSM_INCLUDE_DIR_AUTO NAMES ubs_mem.h ubs_mem_def.h
            HINTS /usr/local/ubs_mem/include
                  ${CMAKE_CURRENT_LIST_DIR}/../ubs-mem-master/src/app_lib/include)
        if(UBSM_INCLUDE_DIR_AUTO)
            set(UBSM_INCLUDE_DIR "${UBSM_INCLUDE_DIR_AUTO}" CACHE PATH
                "Directory containing ubs_mem.h and ubs_mem_def.h" FORCE)
        endif()
    endif()
    if(NOT UBSM_LIBRARY)
        find_library(UBSM_LIBRARY_AUTO NAMES ubsm_sdk
            HINTS /usr/local/ubs_mem/lib /usr/local/lib64 /usr/local/lib
                  /usr/lib64 /usr/lib)
        if(UBSM_LIBRARY_AUTO)
            set(UBSM_LIBRARY "${UBSM_LIBRARY_AUTO}" CACHE FILEPATH
                "Path to libubsm_sdk.so" FORCE)
        endif()
    endif()
    if(NOT EXISTS "${UBSM_INCLUDE_DIR}/ubs_mem.h" OR
       NOT EXISTS "${UBSM_INCLUDE_DIR}/ubs_mem_def.h" OR
       NOT EXISTS "${UBSM_LIBRARY}")
        message(FATAL_ERROR
            "UBSM backend requires UBSM_INCLUDE_DIR and UBSM_LIBRARY. "
            "Set them to the installed UBS Memory SDK headers and libubsm_sdk.so.")
    endif()
else()
    message(FATAL_ERROR "Unknown LRPC_BACKEND='${LRPC_BACKEND}'")
endif()

add_library(lrpc STATIC ${LRPC_SOURCES})
target_include_directories(lrpc PUBLIC include include/uapi)

if(LRPC_BACKEND STREQUAL "ivshmem")
    add_executable(lrpc-publisher demo/publisher.c)
    target_include_directories(lrpc-publisher PRIVATE demo)
    target_link_libraries(lrpc-publisher PRIVATE lrpc)

    add_executable(lrpc-shadow demo/shadow.c)
    target_link_libraries(lrpc-shadow PRIVATE lrpc)

    add_executable(lrpc-nested-shadow demo/nested_shadow.c)
    target_link_libraries(lrpc-nested-shadow PRIVATE lrpc)

    add_executable(lrpc-nested-client demo/nested_client.c)
    target_link_libraries(lrpc-nested-client PRIVATE lrpc)

    add_executable(erpc-lrpc-client demo/erpc_client.cc)
    target_include_directories(erpc-lrpc-client PRIVATE demo)
    target_link_libraries(erpc-lrpc-client PRIVATE lrpc)

    add_executable(lrpc-bar-latency demo/bar_latency.c)
    target_link_libraries(lrpc-bar-latency PRIVATE lrpc)

    add_executable(lrpc-bar-cache-probe demo/bar_cache_probe.c)
    target_link_libraries(lrpc-bar-cache-probe PRIVATE lrpc)
else()
    get_filename_component(UBSM_LIBRARY_DIR "${UBSM_LIBRARY}" DIRECTORY)
    target_include_directories(lrpc PUBLIC "${UBSM_INCLUDE_DIR}")
    target_link_libraries(lrpc PUBLIC "${UBSM_LIBRARY}")

    foreach(UB_TARGET IN ITEMS lrpc-ub-owner lrpc-ub-shadow lrpc-ub-client)
        string(REPLACE "lrpc-ub-" "" UB_SOURCE "${UB_TARGET}")
        add_executable(${UB_TARGET} "demo/ub/${UB_SOURCE}.c")
        target_link_libraries(${UB_TARGET} PRIVATE lrpc)
        set_target_properties(${UB_TARGET} PROPERTIES
            BUILD_RPATH "${UBSM_LIBRARY_DIR}")
    endforeach()
    target_sources(lrpc-ub-owner PRIVATE demo/ub/published_service_aarch64.S)

    add_executable(lrpc-ub-nested-shadow demo/ub/nested_shadow.c)
    target_link_libraries(lrpc-ub-nested-shadow PRIVATE lrpc)
    add_executable(lrpc-ub-nested-client demo/ub/nested_client.c)
    target_link_libraries(lrpc-ub-nested-client PRIVATE lrpc)
    set_target_properties(lrpc-ub-nested-shadow lrpc-ub-nested-client
        PROPERTIES BUILD_RPATH "${UBSM_LIBRARY_DIR}")

    add_executable(erpc-ub-lrpc-client demo/erpc_client.cc)
    target_include_directories(erpc-ub-lrpc-client PRIVATE demo)
    target_link_libraries(erpc-ub-lrpc-client PRIVATE lrpc)
    set_target_properties(erpc-ub-lrpc-client PROPERTIES
        BUILD_RPATH "${UBSM_LIBRARY_DIR}")
endif()

enable_testing()

# Explicit opt-in: the legacy scheduler path does not require OBMM query APIs.
option(LRPC_UB_PA_PROBE "Build the UB remote-domain PA preflight" OFF)
option(LRPC_UB_DOMAIN "Build the ARM64 B-owned UB domain userspace" OFF)
if(LRPC_UB_PA_PROBE OR LRPC_UB_DOMAIN)
    if(NOT LRPC_BACKEND STREQUAL "ubsm")
        message(FATAL_ERROR "LRPC_UB_PA_PROBE requires LRPC_BACKEND=ubsm")
    endif()
    find_path(OBMM_INCLUDE_DIR NAMES libobmm.h
        HINTS /usr/local/include /usr/local/ubs_mem/include
              ${CMAKE_CURRENT_LIST_DIR}/../ubs-mem-master/3rdparty/obmm_ub)
    find_library(OBMM_LIBRARY NAMES obmm
        HINTS /usr/local/ubs_mem/lib /usr/local/lib64 /usr/local/lib)
    if(NOT OBMM_INCLUDE_DIR OR NOT OBMM_LIBRARY)
        message(FATAL_ERROR "Set OBMM_INCLUDE_DIR and OBMM_LIBRARY to installed OBMM headers/library")
    endif()
    target_sources(lrpc PRIVATE lib/ubsm_pa.c)
    target_include_directories(lrpc PRIVATE "${OBMM_INCLUDE_DIR}")
    target_link_libraries(lrpc PUBLIC "${OBMM_LIBRARY}")
    add_executable(lrpc-ub-pa-probe demo/ub/pa_probe.c)
    target_link_libraries(lrpc-ub-pa-probe PRIVATE lrpc)
endif()
if(LRPC_UB_DOMAIN)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        message(FATAL_ERROR "LRPC_UB_DOMAIN requires AArch64")
    endif()
    target_compile_definitions(lrpc PRIVATE LRPC_UB_DOMAIN=1)
    target_sources(lrpc PRIVATE lib/ub_domain_client.c)
    add_executable(lrpc-ub-domain-owner demo/ub/domain_owner.c kernel/ub-domain/services.S)
    target_link_libraries(lrpc-ub-domain-owner PRIVATE lrpc)
    add_executable(lrpc-ub-domain-bench demo/ub/domain_bench.c)
    target_link_libraries(lrpc-ub-domain-bench PRIVATE lrpc)
    add_executable(test-ub-domain-layout tests/test_ub_domain_layout.c)
    target_include_directories(test-ub-domain-layout PRIVATE include include/uapi)
    target_compile_options(test-ub-domain-layout PRIVATE -UNDEBUG)
    add_test(NAME ub-domain-layout COMMAND test-ub-domain-layout)
endif()
