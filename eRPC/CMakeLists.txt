cmake_minimum_required(VERSION 2.8)
project(eRPC)

include(CMakeDependentOption)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIRS})

# 添加编译选项：禁用未使用函数的警告
add_compile_options(-Wno-unused-function)
add_compile_options(-Wno-unused-parameter)
add_compile_options(-Wno-unused-variable)

# Parse the build type
if(PERF)
  message(STATUS "Compilation optimized for performance.")
  message(STATUS "Debugging is disabled.")
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
  set(TESTING OFF)
else(PERF)
  message(STATUS "Compilation not optimized for performance.")
  message(STATUS "Debugging is enabled. Perf will be low.")
  set(CMAKE_BUILD_TYPE Debug)
  SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0")

  # For now, disable unit tests on Windows
  if (NOT WIN32)
    set(TESTING ON)
  endif()
endif(PERF)

# Common sub-projects: HdrHistogram
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "Minimize HDR histogram build")
set(HDR_LOG_REQUIRED OFF CACHE BOOL "Disable HDR histogram's log to avoid zlib dependency")
add_subdirectory(third_party/HdrHistogram_c)

# Common sub-projects: gtest
set(INSTALL_GTEST OFF)
add_subdirectory(third_party/googletest)
enable_testing()
include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})

# Common sub-projects: gflags
add_subdirectory(third_party/gflags)

# Common sub-projects: asio
include_directories(SYSTEM third_party/asio/include)

# Add additional compilation flags only after adding subprojects
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -march=native -Wall -Wextra -Werror -pedantic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wsign-conversion -Wold-style-cast -Wno-unused-function")

# 只在 Clang 上添加这些选项
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-nested-anon-types -Wno-keyword-macro")
endif()

# GCC 特有的选项
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-declarations")
endif()

include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(SYSTEM ${CMAKE_SOURCE_DIR}/third_party)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)

# DPDK may be needed for different reasons (e.g., the transport is DPDK, or
# if the transport is not DPDK but the application needs DPDK libraries)
set(DPDK_NEEDED "false")

# Options exposed to the user
set(TRANSPORT "dpdk" CACHE STRING "Datapath transport (infiniband/raw/dpdk/fake/cxl/ub)")
option(ROCE "Use RoCE if TRANSPORT is infiniband" OFF)
option(AZURE "Configure DPDK for Azure if TRANSPORT is dpdk" OFF)
option(PERF "Compile for performance" ON)
option(USE_SPSC_QUEUE "Use per-source SPSC queues for CXL pointer passing" OFF)
option(SPSC_POLL_STATS "Print CXL SPSC queue polling statistics" OFF)
set(PGO "none" CACHE STRING "Profile-guided optimization (generate/use/none)")
set(LOG_LEVEL "warn" CACHE STRING "Logging level (none/error/warn/info/reorder/trace/cc)") 

if(USE_SPSC_QUEUE)
  message(STATUS "USE_SPSC_QUEUE is enabled")
  add_compile_definitions(USE_SPSC_QUEUE)
endif()

if(SPSC_POLL_STATS)
  message(STATUS "SPSC_POLL_STATS is enabled")
  add_compile_definitions(SPSC_POLL_STATS)
endif()

# Profile-guided optimization
if(PGO STREQUAL "generate")
  message(STATUS "Profile-guided optimization (generate mode) is enabled. Performance will be low.")
  add_definitions(-fprofile-generate)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-generate")
elseif(PGO STREQUAL "use")
  message(STATUS "Profile-guided optimization (use mode) is enabled.")
  add_definitions(-fprofile-use -fprofile-correction)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-use -fprofile-correction")
elseif(PGO STREQUAL "none")
  message(STATUS "Profile-guided optimization is disabled.")
endif()

# Logging level
if(LOG_LEVEL STREQUAL "none")
  message(STATUS "Logging level = none.")
  add_definitions(-DERPC_LOG_LEVEL=0)
elseif(LOG_LEVEL STREQUAL "error")
  message(STATUS "Logging level = error.")
  add_definitions(-DERPC_LOG_LEVEL=1)
elseif(LOG_LEVEL STREQUAL "warn")
  message(STATUS "Logging level = warn.")
  add_definitions(-DERPC_LOG_LEVEL=2)
elseif(LOG_LEVEL STREQUAL "info")
  message(STATUS "Logging level = info.")
  add_definitions(-DERPC_LOG_LEVEL=3)
elseif(LOG_LEVEL STREQUAL "reorder")
  message(STATUS "Logging level = reorder. Warning: Performance will be low.")
  add_definitions(-DERPC_LOG_LEVEL=4)
elseif(LOG_LEVEL STREQUAL "trace")
  message(STATUS "Logging level = trace. Trace, on! Warning: Performance will be low.")
  add_definitions(-DERPC_LOG_LEVEL=5)
elseif(LOG_LEVEL STREQUAL "cc")
  message(STATUS "Logging level = cc. Warning: Performance will be low.")
  add_definitions(-DERPC_LOG_LEVEL=6)
else()
  message(STATUS "No logging level specified. Using warning level.")
  add_definitions(-DERPC_LOG_LEVEL=2)
endif()

# Testing for packet loss, machine failure, etc
if(TESTING)
  message(STATUS "Testing is enabled. Performance will be low.")
  add_definitions(-DERPC_TESTING=true)
else(TESTING)
  message(STATUS "Testing is disabled, so tests may fail.")
  add_definitions(-DERPC_TESTING=false)
endif(TESTING)

set(SOURCES
  src/nexus_impl/nexus.cc
  src/nexus_impl/nexus_bg_thread.cc
  src/nexus_impl/nexus_sm_thread.cc
  src/rpc_impl/rpc.cc
  src/rpc_impl/rpc_queues.cc
  src/rpc_impl/rpc_rfr.cc
  src/rpc_impl/rpc_cr.cc
  src/rpc_impl/rpc_kick.cc
  src/rpc_impl/rpc_req.cc
  src/rpc_impl/rpc_resp.cc
  src/rpc_impl/rpc_ev_loop.cc
  src/rpc_impl/rpc_fault_inject.cc
  src/rpc_impl/rpc_pkt_loss.cc
  src/rpc_impl/rpc_rx.cc
  src/rpc_impl/rpc_connect_handlers.cc
  src/rpc_impl/rpc_disconnect_handlers.cc
  src/rpc_impl/rpc_reset_handlers.cc
  src/rpc_impl/rpc_sm_api.cc
  src/rpc_impl/rpc_sm_helpers.cc
  src/transport_impl/transport.cc
  src/transport_impl/dpdk/dpdk_transport.cc
  src/transport_impl/dpdk/dpdk_init.cc
  src/transport_impl/dpdk/dpdk_externs.cc
  src/transport_impl/dpdk/dpdk_transport_datapath.cc
  src/transport_impl/infiniband/ib_transport.cc
  src/transport_impl/infiniband/ib_transport_datapath.cc
  src/transport_impl/raw/raw_transport.cc
  src/transport_impl/raw/raw_transport_datapath.cc
  src/transport_impl/fake/fake_transport.cc
  src/transport_impl/cxl/cxl_shared_allocator.cc
  src/transport_impl/cxl/cxl_transport.cc
  src/transport_impl/ub/ub_machine.cc
  src/transport_impl/ub/ub_shared_allocator.cc
  src/transport_impl/ub/ub_transport.cc
  src/util/huge_alloc.cc
  src/util/numautils.cc
  src/util/tls_registry.cc)

# Transport-specific. Mellanox OFED drivers are the best choice for raw and
# infiniband, but they do not play well with DPDK. So we compile only one
# transport. Other transports are exluded using preprocessor macros.
string(TOUPPER ${TRANSPORT} DEFINE_TRANSPORT)
add_definitions(-DERPC_${DEFINE_TRANSPORT}=true)
message(STATUS "Selected transport = ${TRANSPORT}.")
set(CONFIG_IS_ROCE false)

if(TRANSPORT STREQUAL "dpdk")
  set(CONFIG_TRANSPORT "DpdkTransport")
  set(CONFIG_HEADROOM 40)
  set(DPDK_NEEDED "true") # We'll resolve DPDK later

  if(AZURE)
    set(CONFIG_IS_AZURE true)
    message(STATUS "Configuring DPDK for Azure")
  else()
    set(CONFIG_IS_AZURE false)
    message(STATUS "Configuring DPDK for bare-metal cluster (i.e., not Azure)")
  endif()
elseif(TRANSPORT STREQUAL "fake")
  set(CONFIG_IS_AZURE false)
  set(CONFIG_TRANSPORT "FakeTransport")
  set(CONFIG_HEADROOM 40)
elseif(TRANSPORT STREQUAL "cxl")
  set(CONFIG_TRANSPORT "CXLTransport")
  set(CONFIG_HEADROOM 0)
  set(CONFIG_IS_AZURE false)
  set(CONFIG_IS_ROCE false)
  message(STATUS "Configuring CXL shared memory transport")

  # 启用 NO-CC 模式
  option(USE_NO_CC_QUEUE "Enable use no cc mode" ON)
  if(USE_NO_CC_QUEUE)
    add_compile_definitions(USE_NO_CC_QUEUE)
  endif()
  
  # 用一端有 CC 模式
  option(USE_ONE_SIDE_READ "Enable one-sided read no_cc auto decision" ON)
  if(USE_ONE_SIDE_READ)
   add_compile_definitions(USE_ONE_SIDE_READ)
  endif()

  # 用 uncacheable 内存
  option(ENABLE_UNCACHE_MEM "Enable use uncacheable memory" OFF)
  if(ENABLE_UNCACHE_MEM)
    add_compile_definitions(ENABLE_UNCACHE_MEM)
  endif()

  # 查找 memkind 库
  find_library(MEMKIND_LIB memkind)
  if(MEMKIND_LIB)
    message(STATUS "Found memkind library: ${MEMKIND_LIB}")
  else()
    message(FATAL_ERROR "memkind library not found. Please install libmemkind-dev")
  endif()
  
  # 查找 numactl 库
  find_library(NUMACTL_LIB numa)
  if(NUMACTL_LIB)
    message(STATUS "Found numa library: ${NUMACTL_LIB}")
    set(LIBRARIES ${LIBRARIES} ${NUMACTL_LIB})
  endif()
  
  # pccshm-sdk 配置
  set(PCCSHM_SDK_PATH "${CMAKE_SOURCE_DIR}/../shm-lib")
  if(EXISTS ${PCCSHM_SDK_PATH})
    include_directories(${PCCSHM_SDK_PATH}/include)
    # 增加 libssh 的 include 路径
    include_directories(/home/gxx/local/libssh/include)
    include_directories(/home/gxx/local/libssh-build/include)
    message(STATUS "Using pccshm-sdk from: ${PCCSHM_SDK_PATH}")
    
    link_directories(${PCCSHM_SDK_PATH}/build)

    # shm-lib 依赖 memkind，memkind 必须在 shm-lib 之前
    set(LIBRARIES ${LIBRARIES} ${MEMKIND_LIB})
    set(LIBRARIES ${LIBRARIES} shm-lib)
    # 链接 libssh
    set(LIBRARIES ${LIBRARIES} /home/gxx/local/libssh-build/lib64/libssh.so)
    
    # 忽略 pccshm-sdk 源码中的类型警告
    # pccshm-sdk 需要 C++17 标准
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-sign-conversion -Wno-old-style-cast -Wno-aligned-new -Wno-pedantic -Wno-error")
  else()
    message(FATAL_ERROR "pccshm-sdk not found at ${PCCSHM_SDK_PATH}")
  endif()
  
elseif(TRANSPORT STREQUAL "ub")
  set(CONFIG_TRANSPORT "UBTransport")
  set(CONFIG_HEADROOM 0)
  set(CONFIG_IS_AZURE false)
  set(CONFIG_IS_ROCE false)
  message(STATUS "Configuring UB shared memory transport")

  set(UBSM_INCLUDE_DIR "" CACHE PATH
      "Directory containing ubs_mem.h and ubs_mem_def.h")
  set(UBSM_LIBRARY "" CACHE FILEPATH "Path to libubsm_sdk.so")
  if(NOT UBSM_INCLUDE_DIR)
    find_path(UBSM_INCLUDE_DIR NAMES ubs_mem.h ubs_mem_def.h HINTS
      /usr/local/ubs_mem/include
      ${CMAKE_SOURCE_DIR}/../ubs-mem-master/src/app_lib/include)
  endif()
  if(NOT UBSM_LIBRARY)
    find_library(UBSM_LIBRARY NAMES ubsm_sdk HINTS
      /usr/local/ubs_mem/lib /usr/local/lib64 /usr/local/lib /usr/lib64 /usr/lib)
  endif()
  if(NOT UBSM_INCLUDE_DIR OR NOT UBSM_LIBRARY)
    message(FATAL_ERROR
      "UB transport requires UBSM_INCLUDE_DIR and UBSM_LIBRARY (libubsm_sdk.so)")
  endif()
  include_directories(${UBSM_INCLUDE_DIR})
  set(LIBRARIES ${LIBRARIES} ${UBSM_LIBRARY})
else()
  set(CONFIG_IS_AZURE false)
  find_library(IBVERBS_LIB ibverbs)
  if(NOT IBVERBS_LIB)
    message(FATAL_ERROR "ibverbs library not found")
  endif()

  set(LIBRARIES ${LIBRARIES} ibverbs)
  if(TRANSPORT STREQUAL "raw")
    set(CONFIG_TRANSPORT "RawTransport")
    set(CONFIG_HEADROOM 40)
    message(FATAL_ERROR
      "eRPC no longer supports Raw transport, please use DTRANSPORT=dpdk instead. "
      "If you wish to use the Raw transport, please use this version: "
      "https://github.com/erpc-io/eRPC/releases/tag/v0.1. You'll need to "
      "install an old version of Mellanox OFED (4.4 or older).")
  elseif(TRANSPORT STREQUAL "fake")
    set(CONFIG_TRANSPORT "FakeTransport")
    set(CONFIG_HEADROOM 40)
  elseif(TRANSPORT STREQUAL "infiniband")
    set(CONFIG_TRANSPORT "IBTransport")
    if(ROCE)
      set(CONFIG_HEADROOM 40)
      set(CONFIG_IS_ROCE true)
    else()
      set(CONFIG_HEADROOM 0)
      set(CONFIG_IS_ROCE false)
    endif()
  endif()
endif()

# Generate config.h
configure_file(src/config.h.in src/config.h)
include_directories(BEFORE ${CMAKE_BINARY_DIR}/src)

# MICA sources
set(MICA_SOURCES
  mica/src/mica/util/cityhash/city_mod.cc
  mica/src/mica/util/config.cc)

# The app to compile. Only one app is compiled to reduce compile time.
if(EXISTS "${CMAKE_SOURCE_DIR}/scripts/autorun_app_file")
  file(STRINGS "scripts/autorun_app_file" APP)
else()
  message(STATUS "No autorun_app_file found. No application will be compiled.")
  return()
endif()
message(STATUS "Compiling app = " ${APP})

# Add app-specific defines now, isolating them from the library and tests

# libpmem is installable from package managers in only recent distros. If it's
# not present, don't link it in.
find_library(PMEM_LIB pmem)
if(NOT PMEM_LIB)
  message(STATUS "Persistent memory library (libpmem) not found")
  set(PMEM "")
else()
  set(PMEM "pmem")
endif()

if(APP STREQUAL "smr")
  include_directories(mica/src)
  add_subdirectory(${CMAKE_SOURCE_DIR}/apps/smr/willemt_raft)
  include_directories(SYSTEM ${CMAKE_SOURCE_DIR}/apps/smr/willemt_raft/include)
  if (WIN32)
    link_directories(${CMAKE_SOURCE_DIR}/build)
    set(LIBRARIES ${LIBRARIES} raft.lib)
  else()
    link_directories(${CMAKE_SOURCE_DIR}/apps/smr/willemt_raft)
    set(LIBRARIES ${LIBRARIES} raft ${PMEM})
  endif()
elseif(APP STREQUAL "masstree_analytics")
  # CMake-based Masstree library from anujkaliaiitd/masstree-beta
  link_directories(${CMAKE_SOURCE_DIR}/third_party/masstree-beta)
  include_directories(SYSTEM ${CMAKE_SOURCE_DIR}/third_party/masstree-beta)
  add_definitions(-include ${CMAKE_SOURCE_DIR}/third_party/masstree-beta/config.h)
  set(APP_ADDITIONAL_SOURCES
    apps/masstree_analytics/mt_index_api.cc
    mica/src/mica/util/cityhash/city_mod.cc)
  set(LIBRARIES ${LIBRARIES} masstree)
  include_directories(mica/src)
elseif(APP STREQUAL "mica_test")
  include_directories(mica/src)
  set(APP_ADDITIONAL_SOURCES ${MICA_SOURCES})
elseif(APP STREQUAL "pmem_bw")
  set(LIBRARIES ${LIBRARIES} ${PMEM})
  message(STATUS "DPDK is needed for pmem_bw application")
  add_definitions(-fpermissive)
  set(DPDK_NEEDED "true")
elseif(APP STREQUAL "persistent_kv")
  set(LIBRARIES ${LIBRARIES} ${PMEM} cityhash)
elseif(APP STREQUAL "log")
  set(LIBRARIES ${LIBRARIES} ${PMEM})
elseif(APP STREQUAL "social_network_cxl")
  # Social Network CXL App - 编译所有服务模块
  message(STATUS "Building social_network_cxl application with CXL transport")

  # 设置源文件目录
  set(SN_CXL_DIR ${CMAKE_SOURCE_DIR}/apps/social_network_cxl)

  # 编译 Protobuf (如果需要)
  protobuf_generate_cpp(SN_PROTO_SRCS SN_PROTO_HDRS ${SN_CXL_DIR}/social_network.proto)

  # MongoDB 配置 - 使用 pkg-config 或直接查找库
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(MONGOC REQUIRED libmongoc-1.0)
  pkg_check_modules(BSON REQUIRED libbson-1.0)

  # 所有服务模块
  set(SN_CXL_PROGRAMS
    load_balance
    unique_id
    url_shorten
    nginx
    user_mention
    user_timeline
    post_storage
    home_timeline
    user_service
    compose_post
    client
  )

  # 编译 eRPC 库
  add_library(erpc ${SOURCES})
  target_include_directories(erpc PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/HdrHistogram_c/src
  )
  target_link_libraries(erpc PUBLIC
    shm-lib
    pthread
    numa
    memkind
  )

  foreach(program_name IN LISTS SN_CXL_PROGRAMS)
    set(EXTRA_SOURCES ${SN_CXL_DIR}/social_network_cxl.cc)
    add_executable(${program_name}
      ${SN_CXL_DIR}/${program_name}/${program_name}.cpp
      ${SN_CXL_DIR}/gflags_definitions.cc
      ${SN_PROTO_SRCS}
      ${SN_PROTO_HDRS}
      ${EXTRA_SOURCES}
    )
    target_include_directories(${program_name} PRIVATE
      ${SN_CXL_DIR}
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/third_party/HdrHistogram_c/src
      ${SN_CXL_DIR}/third_party/atomic_queue/include
      ${SN_CXL_DIR}/third_party/parallel_hashmap
      ${MONGOC_INCLUDE_DIRS}
      ${BSON_INCLUDE_DIRS}
    )
    target_link_libraries(${program_name}
      -Wl,--whole-archive erpc -Wl,--no-whole-archive
      -Wl,--whole-archive shm-lib -Wl,--no-whole-archive
      ${PROTOBUF_LIBRARIES}
      gflags
      hdr_histogram_static
      pthread
      numa
      memkind
      ${MONGOC_LIBRARIES}
      ${BSON_LIBRARIES}
    )
  endforeach()

  # 返回，避免执行后续的通用编译逻辑
  return()
endif()

if(DPDK_NEEDED STREQUAL "true")
  message(STATUS "DPDK needed to build eRPC")

  if (WIN32)
    message(STATUS "DPDK for Windows")

    # Set this to your DPDK directory
    set(DPDK_WINDIR "${CMAKE_CURRENT_SOURCE_DIR}/../dpdk")
    if(NOT EXISTS "${DPDK_WINDIR}/lib/eal/include/rte_common.h")
      message(FATAL_ERROR "rte_common.h not found. DPDK_WINDIR = ${DPDK_WINLIBS}")
      return()
    endif()

    set(DPDK_WINLIBS
      acl bbdev bitratestats bpf cfgfile cmdline compressdev cryptodev
      distributor eal efd ethdev eventdev fib flow_classify graph gro gso hash
      ip_frag ipsec jobstats kni kvargs latencystats lpm mbuf member mempool
      meter metrics net node pci pdump pipeline port power rawdev rcu regexdev
      reorder rib ring sched security stack table telemetry timer vhost)
    foreach(lib_name IN LISTS DPDK_WINLIBS)
      include_directories(SYSTEM "${DPDK_WINDIR}/lib/${lib_name}")
    endforeach()

    include_directories(SYSTEM "${DPDK_WINDIR}/lib/eal/include")
    include_directories(SYSTEM "${DPDK_WINDIR}/lib/eal/windows/include")
    include_directories(SYSTEM "${DPDK_WINDIR}/lib/eal/x86/include")
    include_directories(SYSTEM "${DPDK_WINDIR}/config")
    include_directories(SYSTEM "${DPDK_WINDIR}/build")

    link_directories("${DPDK_WINDIR}/build/lib")
    link_directories("${DPDK_WINDIR}/build/drivers")

    set(LIBRARIES ${LIBRARIES}
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_cfgfile.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_hash.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_cmdline.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_pci.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_ethdev.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_meter.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_net.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_mbuf.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_mempool.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_rcu.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_ring.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_eal.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_telemetry.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_kvargs.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_common_iavf.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_bus_pci.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_bus_vdev.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_common_mlx5.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_mempool_ring.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_net_i40e.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_net_ice.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_net_mlx5.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_net_vmxnet3.a"
      "-lws2_32" "-lmincore" "-ladvapi32" "-lsetupapi" "-ldbghelp"
      "C:/Program Files/Mellanox/MLNX_WinOF2_DevX_SDK/lib/mlx5devx.lib"
      "-Wl,/SUBSYSTEM:CONSOLE" "-lkernel32" "-luser32" "-lgdi32" "-lwinspool"
      "-lshell32" "-lole32" "-loleaut32" "-luuid" "-lcomdlg32" "-liphlpapi")

  else(WIN32)
    # Linux
    if(NOT DEFINED ENV{RTE_SDK})
      message(STATUS "RTE_SDK is not defined, try to use pkg-config")
      find_package(PkgConfig REQUIRED)
      pkg_check_modules(PKG_DPDK_LIB IMPORTED_TARGET libdpdk)
      if(NOT PKG_DPDK_LIB_FOUND) 
        message(FATAL_ERROR "DPDK library not found")
      endif()
      set(LIBRARIES ${LIBRARIES} "-Wl,--whole-archive -Wl,--as-needed" PkgConfig::PKG_DPDK_LIB pthread "-Wl,--no-whole-archive")
      include_directories(SYSTEM ${PKG_DPDK_LIB_INCLUDE_DIRS})
    else()
      set(RTE_SDK $ENV{RTE_SDK})
      set(DPDK_INSTALL_DIR "${RTE_SDK}/build/install/usr/local")

      # DPDK installs libraries into local/lib on Mariner, but
      # local/lib/x86_64-linux-gnu on Ubuntu
      set(DPDK_LIB_DIR "${DPDK_INSTALL_DIR}/lib/x86_64-linux-gnu")
      if(NOT EXISTS ${DPDK_LIB_DIR})
        set(DPDK_LIB_DIR "${DPDK_INSTALL_DIR}/lib")
        if(NOT EXISTS ${DPDK_LIB_DIR})
          message(FATAL_ERROR "${DPDK_LIB_DIR} not found. Did you run `ninja install`?")
        endif()
      endif()
  
      # We use some hacky command-line and CMake magic to construct DPDK library list
      execute_process(
        COMMAND bash -c "PKG_CONFIG_PATH=${DPDK_LIB_DIR}/pkgconfig pkg-config --static --libs-only-l libdpdk"
        OUTPUT_VARIABLE DPDK_PKGCONFIG_OUT RESULT_VARIABLE pkgconfig_ret)
      if(pkgconfig_ret EQUAL "1")
        message( FATAL_ERROR "Failed to run pkgconfig on DPDK (in ${DPDK_LIB_DIR}/pkgconfig). See error above.")
      endif()
  
      string(STRIP ${DPDK_PKGCONFIG_OUT} DPDK_PKGCONFIG_OUT) # Remove newline from pkg-config output
      set(LIBDPDK_LIBRARIES
        "-Wl,--whole-archive -Wl,--as-needed -L${DPDK_LIB_DIR} ${DPDK_PKGCONFIG_OUT} -lpthread -Wl,--no-whole-archive")
      set(LIBRARIES ${LIBRARIES} ${LIBDPDK_LIBRARIES})
  
      link_directories(${DPDK_LIB_DIR})
      include_directories(SYSTEM ${DPDK_INSTALL_DIR}/include)      
    endif()
  endif(WIN32)
else()
  message(STATUS "DPDK not needed to build eRPC")
endif()

set(LIBRARIES ${LIBRARIES} gflags hdr_histogram_static)
if(NOT WIN32)
  set(LIBRARIES ${LIBRARIES} rt numa pthread)
endif()

# Compile the library
add_library(erpc ${SOURCES})

if(TRANSPORT STREQUAL "ub")
  add_executable(erpc_ub_manager
    src/transport_impl/ub/ub_manager.cc
    src/transport_impl/ub/ub_machine.cc
    src/transport_impl/ub/ub_shared_allocator.cc)
  target_link_libraries(erpc_ub_manager ${UBSM_LIBRARY} pthread)

  add_executable(hello_ub_server hello_world_ub/server.cc)
  target_link_libraries(hello_ub_server erpc ${LIBRARIES})
  add_executable(hello_ub_client hello_world_ub/client.cc)
  target_link_libraries(hello_ub_client erpc ${LIBRARIES})
endif()

if(DPDK_NEEDED STREQUAL "true")
  if (NOT WIN32)
    # Build the eRPC DPDK management daemon for Linux only
    add_executable(erpc_dpdk_daemon
      src/transport_impl/dpdk/dpdk_daemon.cc
      src/transport_impl/dpdk/dpdk_externs.cc
      src/transport_impl/dpdk/dpdk_init.cc)
    target_link_libraries(erpc_dpdk_daemon ${LIBRARIES})
  endif()
endif()

# Using link-time optimization sometimes requires building with sources instead
# of liberpc. See the hello world example's Makefile for an example of
# compiling with liberpc.
add_executable(${APP} apps/${APP}/${APP}.cc ${APP_ADDITIONAL_SOURCES} ${SOURCES})

if(PERF)
  target_compile_options(${APP} PRIVATE "-flto")
endif()
target_link_libraries(${APP} ${LIBRARIES})

# Compile hello_world apps
add_executable(hello_server hello_world/server.cc)
target_link_libraries(hello_server erpc ${LIBRARIES})
add_executable(hello_client hello_world/client.cc)
target_link_libraries(hello_client erpc ${LIBRARIES})
add_executable(hello_pair_server hello_world/pair_server.cc)
target_link_libraries(hello_pair_server erpc ${LIBRARIES})
add_executable(hello_pair_client hello_world/pair_client.cc)
target_link_libraries(hello_pair_client erpc ${LIBRARIES})
add_executable(hello_mc_server hello_world/multi_client_server.cc)
target_link_libraries(hello_mc_server erpc ${LIBRARIES})
add_executable(hello_mc_client hello_world/multi_client_client.cc)
target_link_libraries(hello_mc_client erpc ${LIBRARIES})

# Compile the tests
if(TESTING)
  set(LIBRARIES ${LIBRARIES} gtest)

  # End-to-end tests with an RPC server and client
  set(CLIENT_TESTS
    create_session_test
    destroy_session_test
    small_msg_test
    large_msg_test
    req_in_cont_func_test
    req_in_req_func_test
    packet_loss_test
    #server_failure_test
    multi_process_test)
  foreach(test_name IN LISTS CLIENT_TESTS)
    add_executable(${test_name} tests/client_tests/${test_name}.cc)
    target_link_libraries(${test_name} erpc ${LIBRARIES})
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()

  # Tests for internal eRPC protocol implementation
  set(PROTOCOL_TESTS
    rpc_sm_test
    rpc_list_test
    rpc_req_test
    rpc_resp_test
    rpc_cr_test
    rpc_rfr_test
    rpc_kick_test)
  foreach(test_name IN LISTS PROTOCOL_TESTS)
    add_executable(${test_name} tests/protocol_tests/${test_name}.cc)
    target_link_libraries(${test_name} erpc ${LIBRARIES})
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()

  # Tests for the transport backend implementation
  if(TRANSPORT STREQUAL "raw")
    set(TRANSPORT_TESTS
      raw_transport_test)
  endif()
  if(TRANSPORT STREQUAL "dpdk")
    set(TRANSPORT_TESTS
      dpdk_ownership_memzone_test)
  endif()

  foreach(test_name IN LISTS TRANSPORT_TESTS)
    add_executable(${test_name} tests/transport_tests/${test_name}.cc)
    target_link_libraries(${test_name} erpc ${LIBRARIES})
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()

  # Utils that depend on liberpc. These tests are not run using ctest.
  set(UTIL_TESTS_DEP
    huge_alloc_test
    timing_wheel_test)

  foreach(test_name IN LISTS UTIL_TESTS_DEP)
    add_executable(${test_name} tests/util_tests/${test_name}.cc)
    target_link_libraries(${test_name} erpc ${LIBRARIES})
  endforeach()

  # Utils that don't depend on liberpc. These tests are not run using ctest.
  set(UTIL_TESTS_INDEP
    heartbeat_mgr_test
    rand_test
    misc_test
    fixed_vector_test
    timely_test
    numautil_test
    hdr_histogram_test
    udp_client_test
    udp_server_test)

  # Non-Windows tests
  if (NOT WIN32)
    set(UTIL_TESTS_INDEP ${UTIL_TESTS_INDEP}
      hugepage_caching_virt2phy_test)
  endif()

  foreach(test_name IN LISTS UTIL_TESTS_INDEP)
    add_executable(${test_name} tests/util_tests/${test_name}.cc)
    target_link_libraries(${test_name} erpc ${LIBRARIES})
  endforeach()
endif()

# --- Social Network CXL App ---
# set(SN_DIR ${PROJECT_SOURCE_DIR}/apps/social_network)
# protobuf_generate_cpp(SN_PROTO_SRCS SN_PROTO_HDRS ${SN_DIR}/social_network.proto)
# add_executable(sn_unique_id ...)
# target_link_libraries(sn_unique_id ...)
