#!/bin/bash
# Run any application under MigFlow or under the Tiered-AutoNUMA baseline.
#
#   sudo bash scripts/run.sh migflow -- <command> [args...]   # MigFlow
#   sudo bash scripts/run.sh tiered  -- <command> [args...]   # Tiered-AutoNUMA
#
# The application is pinned to the CPUs of socket 0 (CPUS, default 0-23) so
# that node 0 is its local DRAM; MigFlow samples the same CPUs.
# Environment overrides:
#   CPUS            CPU list for the application and for PEBS sampling (default 0-23)
#   RATE_LIMIT_MBPS migration budget of Tiered-AutoNUMA in MB/s (default 80)
#   RESULT_DIR      where logs are written (default <repo>/results)
#   LOG_NAME        log file name prefix (default run-<scheme>)
#   LOG_TITLE       description line written at the top of the log
set -u -o pipefail

usage() { echo "usage: sudo bash $0 migflow|tiered -- <command> [args...]" >&2; exit 1; }

SCHEME=${1:-}; shift || usage
[ "${1:-}" = "--" ] && shift
[ $# -ge 1 ] || usage
case "$SCHEME" in migflow|tiered) ;; *) usage ;; esac
[ "$(id -u)" = 0 ] || { echo "run this script as root (sudo)" >&2; exit 1; }

MIGFLOW_ROOT=$(cd "$(dirname "$0")/.." && pwd)
NUMACTL=/usr/local/bin/numactl
DAEMON=$MIGFLOW_ROOT/daemon/umigratord
MODULE=$MIGFLOW_ROOT/module/migflow.ko
CPUS=${CPUS:-0-23}
RATE_LIMIT_MBPS=${RATE_LIMIT_MBPS:-80}
RESULT_DIR=${RESULT_DIR:-$MIGFLOW_ROOT/results}
LOG_NAME=${LOG_NAME:-run-$SCHEME}
LOG_TITLE=${LOG_TITLE:-}

KNOB_NUMA_BALANCING=/proc/sys/kernel/numa_balancing
KNOB_DEMOTION=/sys/kernel/mm/numa/demotion_enabled
KNOB_ZONE_RECLAIM=/proc/sys/vm/zone_reclaim_mode
KNOB_PROMOTE_RATE=/proc/sys/kernel/numa_balancing_promote_rate_limit_MBps
KNOB_LRU_GEN=/sys/kernel/mm/lru_gen/enabled
KNOB_MIG_LIMIT=/proc/sys/vm/migration_rate_limit_MBps

[ -x "$NUMACTL" ] || { echo "$NUMACTL not found (README step 2)" >&2; exit 1; }
[ -x /usr/bin/time ] || { echo "/usr/bin/time not found (apt install time)" >&2; exit 1; }
[ -w "$KNOB_MIG_LIMIT" ] || { echo "the running kernel lacks the MigFlow patch (README step 1)" >&2; exit 1; }
if [ "$SCHEME" = migflow ]; then
	[ -f "$MODULE" ] || { echo "module not built: $MODULE (README step 4)" >&2; exit 1; }
	[ -x "$DAEMON" ] || { echo "daemon not built: $DAEMON (README step 4)" >&2; exit 1; }
	{ "$NUMACTL" 2>&1 || true; } | grep -q top-down || { echo "$NUMACTL lacks --top-down (README step 2)" >&2; exit 1; }
fi
if [ "$(numactl -H | grep -c '^node [0-9]* size')" -ne 4 ]; then
	echo "expected exactly 4 NUMA nodes (2 DRAM + 2 PMEM); run scripts/setup_topology.sh first" >&2
	exit 1
fi
if pgrep -x umigratord >/dev/null; then
	echo "another MigFlow run is in progress (umigratord pid $(pgrep -x umigratord | head -1)); wait for it to finish" >&2
	exit 1
fi

mkdir -p "$RESULT_DIR"
LOG=$RESULT_DIR/$LOG_NAME-$(date +%Y%m%d-%H%M%S).log
log() { echo "$*" | tee -a "$LOG"; }
vmstat_snapshot() { grep -E "^(pgdemote_kswapd|pgdemote_direct|pgpromote_success|numa_pages_migrated) " /proc/vmstat | sed "s/^/$1 /" >> "$LOG"; }

# Kernel knobs are set explicitly for the scheme and restored afterwards.
SAVED_NUMA_BALANCING=$(cat $KNOB_NUMA_BALANCING)
SAVED_DEMOTION=$(cat $KNOB_DEMOTION)
SAVED_ZONE_RECLAIM=$(cat $KNOB_ZONE_RECLAIM)
SAVED_PROMOTE_RATE=$(cat $KNOB_PROMOTE_RATE)
SAVED_LRU_GEN=$(cat $KNOB_LRU_GEN)
SAVED_MIG_LIMIT=$(cat $KNOB_MIG_LIMIT)
set_knobs() {	# numa_balancing demotion zone_reclaim promote_rate lru_gen migration_limit
	echo "$1" > $KNOB_NUMA_BALANCING
	echo "$2" > $KNOB_DEMOTION
	echo "$3" > $KNOB_ZONE_RECLAIM
	echo "$4" > $KNOB_PROMOTE_RATE
	echo "$5" > $KNOB_LRU_GEN
	echo "$6" > $KNOB_MIG_LIMIT
}
restore_knobs() {
	set_knobs "$SAVED_NUMA_BALANCING" "$SAVED_DEMOTION" "$SAVED_ZONE_RECLAIM" \
		  "$SAVED_PROMOTE_RATE" "$SAVED_LRU_GEN" "$SAVED_MIG_LIMIT"
	rmmod migflow 2>/dev/null
}
trap restore_knobs EXIT

: > "$LOG"
log "== scheme: $SCHEME =="
[ -n "$LOG_TITLE" ] && log "== $LOG_TITLE =="
log "== command: $* =="

swapoff -a
sync; echo 3 > /proc/sys/vm/drop_caches; echo 1 > /proc/sys/vm/compact_memory
rmmod migflow 2>/dev/null
DMESG_MARK="migflow run $LOG_NAME $(date +%s)"
echo "$DMESG_MARK" > /dev/kmsg

case "$SCHEME" in
migflow)
	set_knobs 0 0 0 "$SAVED_PROMOTE_RATE" n 0
	log "== loading migflow.ko (sample_cpus=$CPUS) =="
	insmod "$MODULE" sample_cpus="$CPUS" || exit 1
	LAUNCH="$DAEMON -- $NUMACTL --cpunodebind=0 --physcpubind=$CPUS --top-down=0,1,2,3 --"
	;;
tiered)
	set_knobs 2 1 15 20 y "$RATE_LIMIT_MBPS"
	log "Tiered-AutoNUMA: numa_balancing=2, demotion on, zone_reclaim_mode=15, MGLRU on, promotion rate limit 20 MB/s, migration limit ${RATE_LIMIT_MBPS} MB/s"
	LAUNCH="$NUMACTL --cpunodebind=0 --physcpubind=$CPUS --"
	;;
esac
vmstat_snapshot "vmstat-before"

log "== running =="
echo "log: $LOG"
/usr/bin/time -f "execution_time %e (s)" $LAUNCH "$@" 2>&1 | tee -a "$LOG"

vmstat_snapshot "vmstat-after"
if [ "$SCHEME" = migflow ]; then
	rmmod migflow
	log "== kernel module summary =="
	dmesg | sed -n "/$DMESG_MARK/,\$p" | grep "migflow:" | grep -E "kpebsd sampled|allocations" | tee -a "$LOG"
fi
[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER": "$RESULT_DIR"
bash "$MIGFLOW_ROOT/scripts/summarize.sh" "$LOG" | tee -a "$LOG"
