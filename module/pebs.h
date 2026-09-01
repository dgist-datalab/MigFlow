/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PEBS event definitions for the MigFlow sampler (kpebsd).
 *
 * The raw event codes are for Intel Cascade Lake (Xeon Gold 62xx) with
 * Optane DC persistent memory attached as NUMA nodes. Adjust them for other
 * platforms (see `perf list` and the Intel SDM).
 */
#ifndef MIGFLOW_PEBS_H
#define MIGFLOW_PEBS_H

#include <uapi/linux/perf_event.h>

#define DRAM_LLC_LOAD_MISS        0x1d3		/* MEM_LOAD_L3_MISS_RETIRED.LOCAL_DRAM */
#define REMOTE_DRAM_LLC_LOAD_MISS 0x2d3		/* MEM_LOAD_L3_MISS_RETIRED.REMOTE_DRAM */
#define NVM_LLC_LOAD_MISS         0x80d1	/* MEM_LOAD_RETIRED.LOCAL_PMM */
#define REMOTE_NVM_LLC_LOAD_MISS  0x10d3	/* MEM_LOAD_L3_MISS_RETIRED.REMOTE_PMM */
#define ALL_STORES                0x82d0	/* MEM_INST_RETIRED.ALL_STORES */

enum pebs_event {
	DRAMREAD = 0,
	R_DRAMREAD,
	NVMREAD,
	R_NVMREAD,
	MEMWRITE,
	N_PEBS_EVENTS
};

/* Layout of a PERF_RECORD_SAMPLE with PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR. */
struct pebs_sample {
	struct perf_event_header header;
	__u64 ip;
	__u32 pid, tid;
	__u64 addr;
};

int kpebsd_init(pid_t pid);
void kpebsd_exit(void);
void pebs_get_period(uint64_t *read_period, uint64_t *write_period);

#endif
