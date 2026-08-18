#!/bin/bash
# Run SMR without sudo

export autorun_app="smr"
export MLX4_SINGLE_THREADED=1
export MLX5_SINGLE_THREADED=1
export MLX5_SHUT_UP_BF=0
export MLX_QP_ALLOC_TYPE="HUGE"
export MLX_CQ_ALLOC_TYPE="HUGE"

if [ "$#" -lt 2 ]; then
  echo "Usage: ./run_smr.sh [process_id] [NUMA node]"
  exit 1
fi

epid=$1
numa_node=$2

echo "Launching SMR process $epid on NUMA node $numa_node"

numactl --cpunodebind=$numa_node --membind=$numa_node \
  ./build/smr $(cat apps/smr/config) \
  --process_id $epid --numa_node $numa_node
