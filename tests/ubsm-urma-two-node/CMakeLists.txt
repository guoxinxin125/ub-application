cmake_minimum_required(VERSION 3.16)
project(ubsm_urma_two_node_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(UBSM_INCLUDE_DIR "" CACHE PATH "Directory containing ubs_mem.h")
set(UBSM_LIBRARY "" CACHE FILEPATH "Path to libubsm_sdk.so")
set(URMA_INCLUDE_DIR "" CACHE PATH "Directory containing urma_api.h")
set(URMA_COMMON_INCLUDE_DIR "" CACHE PATH "Directory containing urma_types_str.h")
set(URMA_LIBRARY "" CACHE FILEPATH "Path to liburma.so")
set(URMA_COMMON_LIBRARY "" CACHE FILEPATH "Path to liburma_common.so")

if(NOT UBSM_INCLUDE_DIR)
  find_path(UBSM_INCLUDE_DIR NAMES ubs_mem.h
    HINTS /usr/local/ubs_mem/include
          ${CMAKE_CURRENT_LIST_DIR}/../../ubs-mem-master/src/app_lib/include)
endif()
if(NOT UBSM_LIBRARY)
  find_library(UBSM_LIBRARY NAMES ubsm_sdk
    HINTS /usr/local/ubs_mem/lib /usr/local/lib64 /usr/local/lib /usr/lib64 /usr/lib)
endif()
if(NOT URMA_INCLUDE_DIR)
  find_path(URMA_INCLUDE_DIR NAMES urma_api.h
    HINTS /usr/include/ub/umdk/urma
          ${CMAKE_CURRENT_LIST_DIR}/../../umdk-master/src/urma/lib/urma/core/include)
endif()
if(NOT URMA_COMMON_INCLUDE_DIR)
  find_path(URMA_COMMON_INCLUDE_DIR NAMES urma_types_str.h
    HINTS /usr/include/ub/umdk/urma
          ${CMAKE_CURRENT_LIST_DIR}/../../umdk-master/src/urma/common/include)
endif()
if(NOT URMA_LIBRARY)
  find_library(URMA_LIBRARY NAMES urma
    HINTS /usr/lib64 /usr/local/lib64 /usr/local/lib /usr/lib
          ${CMAKE_CURRENT_LIST_DIR}/../../umdk-master/src/build/urma/lib/urma/core
          ${CMAKE_CURRENT_LIST_DIR}/../../umdk-master/build/urma/lib/urma/core)
endif()
if(NOT URMA_COMMON_LIBRARY)
  find_library(URMA_COMMON_LIBRARY NAMES urma_common
    HINTS /usr/lib64 /usr/local/lib64 /usr/local/lib /usr/lib
          ${CMAKE_CURRENT_LIST_DIR}/../../umdk-master/src/build/urma/common
          ${CMAKE_CURRENT_LIST_DIR}/../../umdk-master/build/urma/common)
endif()

foreach(required UBSM_INCLUDE_DIR UBSM_LIBRARY URMA_INCLUDE_DIR
                 URMA_COMMON_INCLUDE_DIR URMA_LIBRARY URMA_COMMON_LIBRARY)
  if(NOT ${required})
    message(FATAL_ERROR "Missing ${required}; pass it as an absolute CMake path")
  endif()
endforeach()

get_filename_component(UBSM_LIBRARY_DIR "${UBSM_LIBRARY}" DIRECTORY)
get_filename_component(URMA_LIBRARY_DIR "${URMA_LIBRARY}" DIRECTORY)
get_filename_component(URMA_COMMON_LIBRARY_DIR "${URMA_COMMON_LIBRARY}" DIRECTORY)

function(add_ubsm_urma_test target source)
  add_executable(${target} ${source})
  target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../ubs-mem-two-node
    ${UBSM_INCLUDE_DIR}
    ${URMA_INCLUDE_DIR}
    ${URMA_COMMON_INCLUDE_DIR})
  target_link_libraries(${target} PRIVATE
    ${UBSM_LIBRARY} ${URMA_LIBRARY} ${URMA_COMMON_LIBRARY} pthread)
  target_compile_options(${target} PRIVATE -O2 -Wall -Wextra -Wpedantic)
  set_target_properties(${target} PROPERTIES BUILD_RPATH
    "${UBSM_LIBRARY_DIR};${URMA_LIBRARY_DIR};${URMA_COMMON_LIBRARY_DIR}")
endfunction()

add_ubsm_urma_test(urma_rw_latency_test urma_rw_latency_test.cpp)
add_ubsm_urma_test(ubsm_urma_compat_test ubsm_urma_compat_test.cpp)
