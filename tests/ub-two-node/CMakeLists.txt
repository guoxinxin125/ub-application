cmake_minimum_required(VERSION 3.16)
project(obmm_micro_tests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(OBMM_LIBRARY "" CACHE FILEPATH "Path to libobmm.so or libobmm.a")
set(OBMM_SOURCE_DIR "" CACHE PATH "Directory containing libobmm.c and vendor_adaptor.c")
set(OBMM_INCLUDE_DIR "" CACHE PATH "Directory containing libobmm.h")
set(OBMM_UAPI_INCLUDE_DIR "" CACHE PATH "Include root containing ub/obmm.h")

if(NOT OBMM_LIBRARY)
    find_library(
        OBMM_LIBRARY
        NAMES obmm
        HINTS /usr/local/lib64 /usr/local/lib /usr/lib64 /usr/lib)
endif()

if(NOT OBMM_SOURCE_DIR)
    find_path(
        OBMM_SOURCE_DIR
        NAMES libobmm.c vendor_adaptor.c
        HINTS ${CMAKE_CURRENT_LIST_DIR}/../../obmm-master/src/libobmm)
endif()

if(NOT OBMM_INCLUDE_DIR)
    find_path(
        OBMM_INCLUDE_DIR
        NAMES libobmm.h
        HINTS ${OBMM_SOURCE_DIR} /usr/local/include /usr/include
        PATH_SUFFIXES include src/libobmm)
endif()

if(NOT OBMM_UAPI_INCLUDE_DIR)
    find_path(
        OBMM_UAPI_INCLUDE_DIR
        NAMES ub/obmm.h
        HINTS /usr/local/include /usr/include
        PATH_SUFFIXES include include/uapi usr/include)
endif()

set(OBMM_LINK_TARGET "")
if(OBMM_LIBRARY)
    set(OBMM_LINK_TARGET ${OBMM_LIBRARY})
elseif(OBMM_SOURCE_DIR AND OBMM_INCLUDE_DIR AND OBMM_UAPI_INCLUDE_DIR AND
       EXISTS "${OBMM_SOURCE_DIR}/libobmm.c" AND
       EXISTS "${OBMM_SOURCE_DIR}/vendor_adaptor.c")
    add_library(
        obmm_embedded STATIC
        ${OBMM_SOURCE_DIR}/libobmm.c
        ${OBMM_SOURCE_DIR}/vendor_adaptor.c)
    target_include_directories(
        obmm_embedded
        PUBLIC ${OBMM_SOURCE_DIR} ${OBMM_INCLUDE_DIR} ${OBMM_UAPI_INCLUDE_DIR})
    target_link_libraries(obmm_embedded PUBLIC Threads::Threads)
    target_compile_options(obmm_embedded PRIVATE -Wall -Wextra)
    set(OBMM_LINK_TARGET obmm_embedded)
endif()

if(OBMM_LINK_TARGET AND OBMM_INCLUDE_DIR AND OBMM_UAPI_INCLUDE_DIR)
    set(
        OBMM_TEST_TARGETS
        obmm_local_nc_test
        obmm_nc_exporter
        obmm_nc_importer)
    foreach(OBMM_TEST_TARGET IN LISTS OBMM_TEST_TARGETS)
        add_executable(${OBMM_TEST_TARGET} ${OBMM_TEST_TARGET}.cpp)
        target_include_directories(
            ${OBMM_TEST_TARGET}
            PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${OBMM_INCLUDE_DIR} ${OBMM_UAPI_INCLUDE_DIR})
        target_link_libraries(
            ${OBMM_TEST_TARGET} PRIVATE ${OBMM_LINK_TARGET} Threads::Threads)
        target_compile_options(
            ${OBMM_TEST_TARGET} PRIVATE -O2 -Wall -Wextra -Wpedantic)
    endforeach()
else()
    message(STATUS
        "OBMM headers and either its library or C sources were not found; "
        "OBMM micro tests will not be built. Set OBMM_SOURCE_DIR (or OBMM_LIBRARY), "
        "OBMM_INCLUDE_DIR, and OBMM_UAPI_INCLUDE_DIR on an OBMM host.")
endif()
