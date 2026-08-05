cmake_minimum_required(VERSION 3.16)
project(ubs_mem_micro_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(UBSM_INCLUDE_DIR "" CACHE PATH "Directory containing ubs_mem.h and ubs_mem_def.h")
set(UBSM_LIBRARY "" CACHE FILEPATH "Path to libubsm_sdk.so")

if(NOT UBSM_INCLUDE_DIR)
    find_path(
        UBSM_INCLUDE_DIR_AUTO
        NAMES ubs_mem.h ubs_mem_def.h
        HINTS
            /usr/local/ubs_mem/include
            ${CMAKE_CURRENT_LIST_DIR}/../../ubs-mem-master/src/app_lib/include)
    if(UBSM_INCLUDE_DIR_AUTO)
        set(UBSM_INCLUDE_DIR ${UBSM_INCLUDE_DIR_AUTO} CACHE PATH
            "Directory containing ubs_mem.h and ubs_mem_def.h" FORCE)
    endif()
endif()

if(NOT UBSM_LIBRARY)
    find_library(
        UBSM_LIBRARY_AUTO
        NAMES ubsm_sdk
        HINTS /usr/local/ubs_mem/lib /usr/local/lib64 /usr/local/lib /usr/lib64 /usr/lib)
    if(UBSM_LIBRARY_AUTO)
        set(UBSM_LIBRARY ${UBSM_LIBRARY_AUTO} CACHE FILEPATH "Path to libubsm_sdk.so" FORCE)
    endif()
endif()

set(UBSM_HEADERS_OK FALSE)
if(UBSM_INCLUDE_DIR AND
   EXISTS "${UBSM_INCLUDE_DIR}/ubs_mem.h" AND
   EXISTS "${UBSM_INCLUDE_DIR}/ubs_mem_def.h")
    set(UBSM_HEADERS_OK TRUE)
endif()

set(UBSM_LIBRARY_OK FALSE)
if(UBSM_LIBRARY AND EXISTS "${UBSM_LIBRARY}")
    set(UBSM_LIBRARY_OK TRUE)
endif()

message(STATUS "UBS Memory path diagnostics:")
message(STATUS "  UBSM_INCLUDE_DIR='${UBSM_INCLUDE_DIR}'")
if(EXISTS "${UBSM_INCLUDE_DIR}/ubs_mem.h")
    message(STATUS "    ubs_mem.h: FOUND")
else()
    message(STATUS "    ubs_mem.h: MISSING at '${UBSM_INCLUDE_DIR}/ubs_mem.h'")
endif()
if(EXISTS "${UBSM_INCLUDE_DIR}/ubs_mem_def.h")
    message(STATUS "    ubs_mem_def.h: FOUND")
else()
    message(STATUS "    ubs_mem_def.h: MISSING at '${UBSM_INCLUDE_DIR}/ubs_mem_def.h'")
endif()
message(STATUS "  UBSM_LIBRARY='${UBSM_LIBRARY}'")
if(UBSM_LIBRARY_OK)
    message(STATUS "    libubsm_sdk: FOUND")
else()
    message(STATUS "    libubsm_sdk: MISSING")
endif()

if(NOT UBSM_HEADERS_OK OR NOT UBSM_LIBRARY_OK)
    message(FATAL_ERROR
        "UBS Memory micro tests cannot be configured. Review the FOUND/MISSING "
        "diagnostics above. Set UBSM_INCLUDE_DIR and UBSM_LIBRARY to absolute paths.")
endif()

get_filename_component(UBSM_LIBRARY_DIR "${UBSM_LIBRARY}" DIRECTORY)
set(UBSM_TEST_TARGETS
    ubsm_shm_admin
    ubsm_bidirectional_test
    ubsm_local_one_sided_test
    ubsm_one_sided_owner
    ubsm_one_sided_remote)

set(UBSM_BENCHMARK_TARGETS
    ubsm_local_latency_test
    ubsm_remote_rw_latency_test
    ubsm_remote_atomic_test
    ubsm_owner_to_remote_cc_test
    ubsm_remote_to_owner_cc_test)

function(add_ubsm_executable UBSM_TARGET UBSM_SOURCE)
    add_executable(${UBSM_TARGET} ${UBSM_SOURCE})
    target_include_directories(${UBSM_TARGET} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/benchmarks
        ${UBSM_INCLUDE_DIR})
    target_link_libraries(${UBSM_TARGET} PRIVATE ${UBSM_LIBRARY})
    target_compile_options(${UBSM_TARGET} PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    set_target_properties(${UBSM_TARGET} PROPERTIES
        BUILD_RPATH "${UBSM_LIBRARY_DIR}")
endfunction()

foreach(UBSM_TEST_TARGET IN LISTS UBSM_TEST_TARGETS)
    add_ubsm_executable(${UBSM_TEST_TARGET} "${UBSM_TEST_TARGET}.cpp")
endforeach()

foreach(UBSM_BENCHMARK_TARGET IN LISTS UBSM_BENCHMARK_TARGETS)
    add_ubsm_executable(${UBSM_BENCHMARK_TARGET}
        "benchmarks/${UBSM_BENCHMARK_TARGET}.cpp")
endforeach()
