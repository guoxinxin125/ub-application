cmake_minimum_required(VERSION 3.2)
project(tigon)

option(TIGON_ENABLE_UB "Build the UBS Memory backend" OFF)
option(TIGON_BUILD_UB_UNIT_TESTS "Build host-only UB ABI/lock tests" OFF)
set(UBSM_INCLUDE_DIR "/usr/local/ubs_mem/include" CACHE PATH "UBS Memory SDK include directory")
set(UBSM_LIBRARY "/usr/local/ubs_mem/lib/libubsm_sdk.so" CACHE FILEPATH "UBS Memory SDK library")
set(TIGON_CXLALLOC_LIBRARY
        "${CMAKE_SOURCE_DIR}/dependencies/cxlalloc/libcxlalloc_static.a"
        CACHE FILEPATH "Legacy CXL allocator library (not used by the UB backend)")

set(CMAKE_C_COMPILER /usr/bin/clang-15)
set(CMAKE_CXX_COMPILER /usr/bin/clang++-15)

set(CMAKE_C_STANDARD 14)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_CXX_FLAGS "-pthread -Wall -fno-omit-frame-pointer -pedantic -O3 -march=native -DNDEBUG -g3 \
                    -Wno-long-long -Wno-unused-variable -Wno-unused-but-set-variable \
                    -Wno-variadic-macros -Wno-unused-lambda-capture -Wno-c99-extensions \
                    -Wno-zero-length-array -Wno-deprecated-declarations"
)
set(CMAKE_EXE_LINKER_FLAGS "-O3 -march=native -no-pie -fuse-ld=lld-15")

# include path
include_directories(
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/dependencies/cxlalloc
)

find_library(jemalloc_lib jemalloc)

# all misc CPP files
file(GLOB_RECURSE MISC_CPP_FILES common/*.cpp protocol/Pasha/*.cpp protocol/SundialPasha/*.cpp protocol/TwoPLPasha/*.cpp core/*.cpp)
add_library(misc_cpp STATIC ${MISC_CPP_FILES})

if(TIGON_ENABLE_UB)
        if(NOT EXISTS "${UBSM_INCLUDE_DIR}/ubs_mem.h")
                message(FATAL_ERROR "ubs_mem.h was not found under UBSM_INCLUDE_DIR=${UBSM_INCLUDE_DIR}")
        endif()
        if(NOT EXISTS "${UBSM_LIBRARY}")
                message(FATAL_ERROR "UBS Memory SDK library was not found: ${UBSM_LIBRARY}")
        endif()
        target_compile_definitions(misc_cpp PUBLIC TIGON_ENABLE_UB=1)
        target_include_directories(misc_cpp PUBLIC ${UBSM_INCLUDE_DIR})
        target_link_libraries(misc_cpp PUBLIC ${UBSM_LIBRARY})

        message(STATUS
                "Tigon UB backend enabled for ${CMAKE_SYSTEM_PROCESSOR}; "
                "the legacy cxlalloc archive will not be linked")

        add_executable(ub_tigon_two_node_test tests/ub_two_node_correctness.cpp)
        target_link_libraries(ub_tigon_two_node_test misc_cpp ${jemalloc_lib} glog)
else()
        if(NOT EXISTS "${TIGON_CXLALLOC_LIBRARY}")
                message(FATAL_ERROR
                        "Legacy CXL allocator library was not found: "
                        "${TIGON_CXLALLOC_LIBRARY}")
        endif()
endif()

function(tigon_link_shared_memory_backend target)
        if(NOT TIGON_ENABLE_UB)
                target_link_libraries(${target} ${TIGON_CXLALLOC_LIBRARY})
        endif()
endfunction()

# TPCC benchmark
add_executable(bench_tpcc bench_tpcc.cpp)
target_link_libraries(bench_tpcc misc_cpp ${jemalloc_lib} glog gflags)
tigon_link_shared_memory_backend(bench_tpcc)

# YCSB benchmark
add_executable(bench_ycsb bench_ycsb.cpp)
target_link_libraries(bench_ycsb misc_cpp ${jemalloc_lib} glog gflags)
tigon_link_shared_memory_backend(bench_ycsb)

if(TIGON_BUILD_UB_UNIT_TESTS)
        enable_testing()
        add_executable(ub_primitives_test tests/ub_primitives_test.cpp)
        target_link_libraries(ub_primitives_test pthread)
        add_test(NAME ub_primitives_test COMMAND ub_primitives_test)

        add_executable(ub_btree_compile_test tests/ub_btree_compile_test.cpp)
        target_link_libraries(ub_btree_compile_test misc_cpp pthread)
        add_test(NAME ub_btree_compile_test COMMAND ub_btree_compile_test)

        add_executable(ub_btree_host_test tests/ub_btree_host_test.cpp)
        target_link_libraries(ub_btree_host_test pthread)
        add_test(NAME ub_btree_host_test COMMAND ub_btree_host_test)

        add_executable(ub_adapter_compile_test tests/ub_adapter_compile_test.cpp)
        target_link_libraries(ub_adapter_compile_test misc_cpp ${jemalloc_lib} glog)
        add_test(NAME ub_adapter_compile_test COMMAND ub_adapter_compile_test)

        add_executable(ub_tpcc_adapter_compile_test
                tests/ub_tpcc_adapter_compile_test.cpp)
        target_link_libraries(ub_tpcc_adapter_compile_test misc_cpp ${jemalloc_lib} glog)
        add_test(NAME ub_tpcc_adapter_compile_test
                COMMAND ub_tpcc_adapter_compile_test)

        add_executable(ub_twopl_compile_test tests/ub_twopl_compile_test.cpp)
        target_link_libraries(ub_twopl_compile_test misc_cpp ${jemalloc_lib} glog)
        add_test(NAME ub_twopl_compile_test COMMAND ub_twopl_compile_test)
endif()

# # SmallBank benchmark
# add_executable(bench_smallbank bench_smallbank.cpp)
# target_link_libraries(bench_smallbank misc_cpp ${CMAKE_SOURCE_DIR}/dependencies/cxlalloc/libcxlalloc_static.a ${jemalloc_lib} glog gflags)

# # TATP benchmark
# add_executable(bench_tatp bench_tatp.cpp)
# target_link_libraries(bench_tatp misc_cpp ${CMAKE_SOURCE_DIR}/dependencies/cxlalloc/libcxlalloc_static.a ${jemalloc_lib} glog gflags)
