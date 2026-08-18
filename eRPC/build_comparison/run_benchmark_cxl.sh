#!/bin/bash
# 单机运行 CXL SMR 集群（3 Server + 1 Client）

set -e

BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ERPC_SCRIPTS="$BUILD_DIR/../scripts"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

run_gdb() {
    local cmd="$1"
    local log="$2"
    gdb -batch -ex "run" -ex "bt" --args $cmd > "$log" 2>&1
}

# 确保配置文件存在
mkdir -p "$ERPC_SCRIPTS"
cp "$BUILD_DIR/autorun_process_file" "$ERPC_SCRIPTS/autorun_process_file"

cd "$BUILD_DIR/.." || exit 1

echo "=========================================="
echo "  CXL SMR Benchmark (Single Machine)"
echo "  3 Raft Servers + 1 Client"
echo "=========================================="
echo ""

# 清理旧进程
echo "Cleaning up old processes..."
pkill -f "smr_cxl" 2>/dev/null || true
sleep 1

# 启动 Server 0
echo -e "${GREEN}Starting Server 0 (process_id=0)...${NC}"
$BUILD_DIR/smr_cxl --process_id=1 --num_raft_servers=3 --numa_node=0 \
    > /tmp/smr_server_1.log 2>&1 &
PID0=$!
sleep 1

# 启动 Server 1
echo -e "${GREEN}Starting Server 1 (process_id=1)...${NC}"
$BUILD_DIR/smr_cxl --process_id=0 --num_raft_servers=3 --numa_node=0 \
    > /tmp/smr_server_0.log 2>&1 &
PID1=$!
sleep 1

# 启动 Server 2
echo -e "${GREEN}Starting Server 2 (process_id=2)...${NC}"
$BUILD_DIR/smr_cxl --process_id=2 --num_raft_servers=3 --numa_node=0 \
    > /tmp/smr_server_2.log 2>&1 &
PID2=$!

echo ""
echo "Servers started with PIDs: $PID0, $PID1, $PID2"
echo -e "${YELLOW}Waiting for leader election (10 seconds)...${NC}"
sleep 10

# 启动 Client
echo ""
echo "=========================================="
echo -e "${GREEN}Starting Client (process_id=3)...${NC}"
echo "=========================================="
echo ""
echo "Client will print latency statistics every 100,000 requests."
echo "Leader commit latency is logged to /tmp/smr_server_*.log"
echo "Press Ctrl+C to stop."
echo ""

# 捕获 Ctrl+C，清理所有进程
cleanup() {
    echo ""
    echo "Stopping all processes..."
    kill -9 $PID0 $PID1 $PID2 2>/dev/null || true
    wait 2>/dev/null
    echo "Done."
    exit 0
}
trap cleanup SIGINT SIGTERM

# 前台运行 Client，显示延迟输出
# gdb -ex "catch signal SIGSEGV" -ex "run" -ex "bt" -ex "p/x queue_base" -ex "p *this" --args $BUILD_DIR/smr_cxl --process_id=3 --num_raft_servers=3 --numa_node=0
$BUILD_DIR/smr_cxl --process_id=3 --num_raft_servers=3 --numa_node=0

# 正常退出时也清理
cleanup

