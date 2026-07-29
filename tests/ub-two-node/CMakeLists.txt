cmake_minimum_required(VERSION 3.16)
project(ub_two_node_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(MEMLINK_CLIENT_LIBRARY "" CACHE FILEPATH "Path to libmemlink_client.so or libmemlink_client.a")
if(NOT MEMLINK_CLIENT_LIBRARY)
    find_library(
        MEMLINK_CLIENT_LIBRARY
        NAMES memlink_client
        HINTS
            ${CMAKE_CURRENT_LIST_DIR}/../../shared_libs
            /usr/local/lib
            /usr/lib64
            /usr/lib)
endif()

if(NOT MEMLINK_CLIENT_LIBRARY)
    message(FATAL_ERROR "libmemlink_client was not found; set -DMEMLINK_CLIENT_LIBRARY=/path/to/libmemlink_client.so")
endif()

add_executable(ub_two_node_bench ub_two_node_bench.cpp)
target_include_directories(
    ub_two_node_bench
    PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../shm-lib/include)
target_link_libraries(
    ub_two_node_bench
    PRIVATE ${MEMLINK_CLIENT_LIBRARY} Threads::Threads)
target_compile_options(ub_two_node_bench PRIVATE -O3 -Wall -Wextra -Wpedantic)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_compile_options(ub_two_node_bench PRIVATE -mclflushopt -mclwb)
endif()
