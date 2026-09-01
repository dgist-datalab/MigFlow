#!/bin/bash
# Configure the four-tier memory topology used in the MigFlow evaluation:
# two sockets, each with DRAM (node 0/1) and PMEM exposed as a NUMA node
# (node 2/3). DRAM nodes are trimmed to DRAM_GB by offlining memory blocks;
# PMEM (kmem) blocks are onlined as ZONE_NORMAL.
#
# Prerequisites (one-time):
#   - PMEM namespaces in devdax mode (ndctl create-namespace --mode=devdax)
#   - boot parameter memhp_default_state=offline, so kmem blocks are not
#     onlined as ZONE_MOVABLE automatically
# Run after every boot: sudo bash setup_topology.sh
set -u

DRAM_GB=${DRAM_GB:-32}
DAX_DEVICES=${DAX_DEVICES:-"dax0.0 dax1.0"}
PMEM_NODES=${PMEM_NODES:-"2 3"}
DRAM_NODES=${DRAM_NODES:-"0 1"}

[ "$(id -u)" = 0 ] || { echo "run this script as root (sudo)" >&2; exit 1; }

echo "== onlining PMEM nodes as system RAM (ZONE_NORMAL) =="
modprobe kmem 2>/dev/null
daxctl migrate-device-model >/dev/null 2>&1
for dev in $DAX_DEVICES; do
	# hand the device to the kmem driver without onlining its memory
	daxctl reconfigure-device --mode=system-ram --no-online $dev >/dev/null
done
sleep 1
for n in $PMEM_NODES; do
	cnt=0
	for mb in /sys/devices/system/node/node$n/memory[0-9]*; do
		[ -d "$mb" ] || continue
		st=/sys/devices/system/memory/memory${mb##*/memory}/state
		if [ "$(cat $st)" = "offline" ]; then
			timeout 10 bash -c "echo online_kernel > $st" 2>/dev/null && cnt=$((cnt + 1))
		fi
	done
	echo "node$n: $cnt memory blocks onlined"
done

echo "== trimming DRAM nodes to ${DRAM_GB} GiB =="
TARGET_KB=$((DRAM_GB * 1024 * 1024))
for n in $DRAM_NODES; do
	while true; do
		cur=$(awk '/MemTotal/{print $4}' /sys/devices/system/node/node$n/meminfo)
		[ "$cur" -le "$TARGET_KB" ] && break
		done_one=0
		for mb in $(ls -d /sys/devices/system/node/node$n/memory[0-9]* 2>/dev/null | sed 's/.*memory//' | sort -rn); do
			st=/sys/devices/system/memory/memory$mb/state
			if [ "$(cat $st)" = "online" ] && timeout 10 bash -c "echo offline > $st" 2>/dev/null; then
				done_one=1
				cur=$(awk '/MemTotal/{print $4}' /sys/devices/system/node/node$n/meminfo)
				[ "$cur" -le "$TARGET_KB" ] && break
			fi
		done
		[ $done_one -eq 0 ] && { echo "node$n: no more blocks can be offlined (${cur} KB)"; break; }
	done
	echo "node$n: $(awk '/MemTotal/{print $4}' /sys/devices/system/node/node$n/meminfo) KB"
done

echo "== topology =="
numactl -H | grep -E "^available|^node [0-9]+ size"
