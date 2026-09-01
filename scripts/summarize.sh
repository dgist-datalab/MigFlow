#!/bin/bash
# Summarize a log written by run.sh, independently of the workload: execution
# time and migration volume (from umigratord's exit statistics for MigFlow,
# from /proc/vmstat deltas for Tiered-AutoNUMA). Workload-specific metrics
# such as throughput are printed by the workload itself and stay in the log.
LOG=${1:?usage: summarize.sh <log>}
gb() { awk -v p="$1" 'BEGIN{printf "%.2f", p*4/1024/1024}'; }
succ() { grep -a "^$1: .*succeeded" "$LOG" | tail -1 | sed -E 's/.*succeeded ([0-9]+) pages.*/\1/'; }
delta() { a=$(grep -a "^vmstat-before $1 " "$LOG" | awk '{print $3}'); b=$(grep -a "^vmstat-after $1 " "$LOG" | awk '{print $3}'); echo $(( ${b:-0} - ${a:-0} )); }
SCHEME=$(grep -ao "^== scheme: [a-z]*" "$LOG" | head -1 | awk '{print $3}')
[ -z "$SCHEME" ] && { grep -aq "^promotion:" "$LOG" && SCHEME=migflow || SCHEME=tiered; }
T=$(grep -ao "execution_time [0-9.]*" "$LOG" | tail -1 | awk '{print $2}')

echo "== summary ($SCHEME) =="
printf "%-22s %s s\n" "execution time" "${T:-n/a}"
if [ "$SCHEME" = migflow ]; then
	P=$(succ promotion); Q=$(succ "quick demotion"); L=$(succ "LFU demotion"); : ${P:=0} ${Q:=0} ${L:=0}
	printf "%-22s %s GB (%'d pages)\n" "promotion" "$(gb $P)" "$P"
	printf "%-22s %s GB\n" "quick demotion" "$(gb $Q)"
	printf "%-22s %s GB\n" "LFU demotion" "$(gb $L)"
	printf "%-22s %s GB\n" "migration total" "$(gb $((P+Q+L)))"
else
	# pgpromote_success counts NUMA-fault promotions into the DRAM tiers;
	# reclaim demotion is done by kswapd and, rarely, by direct reclaim
	P=$(delta pgpromote_success); D=$(( $(delta pgdemote_kswapd) + $(delta pgdemote_direct) ))
	printf "%-22s %s GB (NUMA-fault promotion)\n" "promotion" "$(gb $P)"
	printf "%-22s %s GB (reclaim demotion)\n" "demotion" "$(gb $D)"
	printf "%-22s %s GB\n" "migration total" "$(gb $((P+D)))"
fi
