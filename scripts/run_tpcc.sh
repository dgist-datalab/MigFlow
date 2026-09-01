#!/bin/bash
# Run Silo TPC-C under MigFlow (default) or Tiered-AutoNUMA.
#
#   sudo bash scripts/run_tpcc.sh [migflow|tiered]
#
# Environment overrides:
#   SCALE_FACTOR   TPC-C warehouses (default 28: 66 GB database, ~270 GB peak footprint)
#   OPS_PER_WORKER transactions per worker thread (default 25000000)
#   NTHREADS       worker threads, pinned to socket 0 (default 24)
set -u
SCHEME=${1:-migflow}
MIGFLOW_ROOT=$(cd "$(dirname "$0")/.." && pwd)
DBTEST=$MIGFLOW_ROOT/workloads/silo/out-perf.masstree/benchmarks/dbtest
SCALE_FACTOR=${SCALE_FACTOR:-28}
OPS_PER_WORKER=${OPS_PER_WORKER:-25000000}
NTHREADS=${NTHREADS:-24}

[ -x "$DBTEST" ] || { echo "Silo is not built: $DBTEST (README step 5)" >&2; exit 1; }
RESULT_DIR=${RESULT_DIR:-$MIGFLOW_ROOT/results}
mkdir -p "$RESULT_DIR" && cd "$RESULT_DIR"    # Silo writes jemalloc.stats to the working directory
export RESULT_DIR
CPUS=0-$((NTHREADS - 1)) LOG_NAME=tpcc-$SCHEME \
LOG_TITLE="TPC-C: scale factor $SCALE_FACTOR, $OPS_PER_WORKER ops/worker, $NTHREADS threads" \
	exec bash "$MIGFLOW_ROOT/scripts/run.sh" "$SCHEME" -- \
	"$DBTEST" --verbose --bench tpcc --num-threads $NTHREADS \
	--scale-factor $SCALE_FACTOR --ops-per-worker=$OPS_PER_WORKER
