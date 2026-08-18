# Install script for directory: /home/gxx/pccshm-sdk/eRPC/third_party/HdrHistogram_c/src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "RelWithDebInfo")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "0")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  foreach(file
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/libhdr_histogram.so.6.1.1"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/libhdr_histogram.so.6"
      )
    if(EXISTS "${file}" AND
       NOT IS_SYMLINK "${file}")
      file(RPATH_CHECK
           FILE "${file}"
           RPATH "")
    endif()
  endforeach()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64" TYPE SHARED_LIBRARY FILES
    "/home/gxx/pccshm-sdk/eRPC/build_comparison/cxl/third_party/HdrHistogram_c/src/libhdr_histogram.so.6.1.1"
    "/home/gxx/pccshm-sdk/eRPC/build_comparison/cxl/third_party/HdrHistogram_c/src/libhdr_histogram.so.6"
    )
  foreach(file
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/libhdr_histogram.so.6.1.1"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/libhdr_histogram.so.6"
      )
    if(EXISTS "${file}" AND
       NOT IS_SYMLINK "${file}")
      if(CMAKE_INSTALL_DO_STRIP)
        execute_process(COMMAND "/usr/bin/strip" "${file}")
      endif()
    endif()
  endforeach()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64" TYPE SHARED_LIBRARY FILES "/home/gxx/pccshm-sdk/eRPC/build_comparison/cxl/third_party/HdrHistogram_c/src/libhdr_histogram.so")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64" TYPE STATIC_LIBRARY FILES "/home/gxx/pccshm-sdk/eRPC/build_comparison/cxl/third_party/HdrHistogram_c/src/libhdr_histogram_static.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/hdr" TYPE FILE FILES
    "/home/gxx/pccshm-sdk/eRPC/third_party/HdrHistogram_c/src/hdr_histogram.h"
    "/home/gxx/pccshm-sdk/eRPC/third_party/HdrHistogram_c/src/hdr_histogram_log.h"
    "/home/gxx/pccshm-sdk/eRPC/third_party/HdrHistogram_c/src/hdr_interval_recorder.h"
    "/home/gxx/pccshm-sdk/eRPC/third_party/HdrHistogram_c/src/hdr_thread.h"
    "/home/gxx/pccshm-sdk/eRPC/third_party/HdrHistogram_c/src/hdr_time.h"
    "/home/gxx/pccshm-sdk/eRPC/third_party/HdrHistogram_c/src/hdr_writer_reader_phaser.h"
    )
endif()

