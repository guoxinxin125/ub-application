#!/bin/bash
# 构建 CXL 版本的 SMR 用于性能测试

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ERPC_DIR="$SCRIPT_DIR"
PROJECT_ROOT="$(dirname "$ERPC_DIR")"
BUILD_DIR="$PROJECT_ROOT/eRPC/build_comparison"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

NUM_CORES=$(nproc)

echo "========== Building SMR for CXL Performance Test =========="
echo "eRPC source:  $ERPC_DIR"
echo "Build dir:    $BUILD_DIR"

# 创建 autorun_app_file
mkdir -p "$ERPC_DIR/scripts"
echo "smr" > "$ERPC_DIR/scripts/autorun_app_file"
mkdir -p "$BUILD_DIR"

# 创建 CXL 单机配置文件（3 Server + 1 Client）
cat > "$ERPC_DIR/scripts/autorun_process_file" << EOF
127.0.0.1 31850 0
127.0.0.1 31851 0
127.0.0.1 31852 0
127.0.0.1 31853 0
EOF

# 构建 CXL 版本
echo ""
echo -e "${GREEN}>>> Building CXL version...${NC}"

CXL_BUILD="$BUILD_DIR/cxl"
rm -rf "$CXL_BUILD"
mkdir -p "$CXL_BUILD"

# 使用 -S 和 -B 参数
cmake -S "$ERPC_DIR" -B "$CXL_BUILD" \
    -DTRANSPORT=cxl \
    -DCMAKE_BUILD_TYPE=Release \
    -DPERF=ON

if [ $? -ne 0 ]; then
    echo -e "${RED}ERROR: CMake configuration failed${NC}"
    exit 1
fi

cd "$CXL_BUILD"
make -j${NUM_CORES}

SMR_BINARY="$ERPC_DIR/build/smr"

if [ -f "$SMR_BINARY" ]; then
    cp "$SMR_BINARY" "$BUILD_DIR/smr_cxl"
    echo -e "${GREEN}✓ CXL version: $BUILD_DIR/smr_cxl${NC}"
else
    echo -e "${RED}ERROR: smr binary not found at $SMR_BINARY${NC}"
    exit 1
fi

# 复制配置文件到 build 目录
cp "$ERPC_DIR/scripts/autorun_process_file" "$BUILD_DIR/"

# ============================================================================
# 创建一键运行脚本 run_benchmark_cxl.sh
# ============================================================================
cat > "$BUILD_DIR/run_benchmark_cxl.sh" << 'SCRIPT'
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

SCRIPT
chmod +x "$BUILD_DIR/run_benchmark_cxl.sh"

echo ""
echo -e "${GREEN}========== Build Complete ==========${NC}"
echo ""
echo "Files created:"
echo "  Binary: $BUILD_DIR/smr_cxl"
echo "  Config: $BUILD_DIR/autorun_process_file"
echo "  Script: $BUILD_DIR/run_benchmark_cxl.sh"
echo ""
echo "=========================================="
echo "  How to Run"
echo "=========================================="
echo ""
echo "  cd $BUILD_DIR"
echo "  ./run_benchmark_cxl.sh"
echo ""
echo "=========================================="
echo "  Expected Output"
echo "=========================================="
echo ""
echo "  Client (stdout):"
echo "    smr: Latency us = {P50, P99, P99.9, P99.99, P99.999, max}"
echo ""
echo "  Leader (check logs):"
echo "    tail -f /tmp/smr_server_*.log"
echo "    smr: Leader commit latency (us) = {median, 99%}"
echo ""