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
