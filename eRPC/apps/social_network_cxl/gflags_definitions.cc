#include <gflags/gflags.h>

DEFINE_string(config_file, "./config/config.json", "Config file path");
DEFINE_uint64(test_loop, 10, "Test loop");
DEFINE_uint64(concurrency, 0, "Concurrency for each request");
DEFINE_string(numa_0_ports, "", "Fabric ports on NUMA node 0");
DEFINE_string(numa_1_ports, "", "Fabric ports on NUMA node 1");
DEFINE_string(server_addr, "0:1234", "Server address for CXL");
DEFINE_uint64(client_num, 1, "Client thread num");
DEFINE_uint64(server_num, 1, "Server thread num");
DEFINE_uint64(numa_client_node, 0, "NUMA node for client");
DEFINE_uint64(numa_server_node, 0, "NUMA node for server");
DEFINE_uint64(bind_core_offset, 0, "Bind core offset");
DEFINE_uint64(timeout_second, UINT64_MAX, "Timeout second");
DEFINE_string(latency_file, "latency.txt", "Latency file");
DEFINE_string(bandwidth_file, "bandwidth.txt", "Bandwidth file");
DEFINE_uint64(rpc_id, 0, "Unique RPC ID for the process");
