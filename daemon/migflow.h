/*
 * umigratord: userspace migration daemon of MigFlow.
 *
 * Core data structures and tunables. The daemon consumes allocation and
 * PEBS records from the kernel module, keeps a per-page profile, and runs
 * quick demotion, cost-benefit promotion and LFU demotion through
 * move_pages(2).
 */
#ifndef MIGFLOW_H
#define MIGFLOW_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <list>
#include "../include/migflow_rb.h"

#define ABORT_WITH_LOG() do { \
	fprintf(stderr, "abort at %s:%d\n", __FILE__, __LINE__); \
	abort(); \
} while (0)

#define PAGE_SIZE 4096UL
#define MAX_NODES 4		/* memory tiers, index 0 = fastest */
#define NR_MOVE_PAGES 512	/* pages per move_pages(2) call */

/* Access-count histogram: bin n holds pages with hotness in [2^n - 1, 2^(n+1) - 1). */
#define NR_HIST_BINS 32
#define HOTNESS_WEIGHT 0.5	/* decay applied per cooling round */

/*
 * A PEBS sample adds PEBS_HOTNESS_REF_PERIOD accesses to a page's hotness,
 * independently of the current sampling period.
 */
#define PEBS_HOTNESS_REF_PERIOD 1399
#define COOLING_INTERVAL 2000000	/* profiled accesses between cooling rounds */
#define PERIOD_INTERVAL 10		/* seconds between sampling-period refreshes */

/*
 * Migration phase: every MIG_INTERVAL seconds, one shared budget of
 * NR_TOTAL_BUDGET pages is spent in this order:
 *   (1) quick demotion, at most NR_QD_FIRST_PAGES
 *   (2) cost-benefit promotion candidates, at most NR_PROMOTE_PAGES
 *   (3) LFU demotion to make room for (2)
 *   (4) the promotions themselves
 *   (5) any remaining budget on further quick demotion
 */
#define MIG_INTERVAL 10
#define NR_PROMOTE_PAGES 51200UL		/* 200 MB */
#define NR_DEMOTE_PAGES 51200UL			/* 200 MB per tier and phase */
#define NR_TOTAL_BUDGET 204800UL		/* 800 MB per phase */
#define NR_QD_FIRST_PAGES (NR_TOTAL_BUDGET / 2)

/* Quick demotion also runs on its own timer, independently of the phase. */
#define QD_INTERVAL 2				/* seconds */
#define QD_RATE_LIMIT 10240UL			/* pages per QD run (40 MB) */
#define QD_QUEUE_CAP (NR_DEMOTE_PAGES * 4)	/* pages kept per tier queue */

#define NR_MARGIN_PAGES 0			/* free pages to leave on every tier */
#define NR_MAX_MIG_PAGES NR_TOTAL_BUDGET	/* largest batch of one migration step */

#define NO_MIG -1
#define UNKNOWN_NODE -1
#define FLAG_QD 16	/* or-ed into next_node for quick-demotion moves */

struct alloc_stat {
	unsigned long long nr_drain_pages;
	unsigned long long nr_cold_pages;
	unsigned long long nr_not_cold_pages;
	unsigned long long nr_reaccessed_cold_pages;
	unsigned long long nr_alloc_pages[MAX_NODES];

	/* per-phase counters, reset every migration phase */
	unsigned long long phase_drain_pages;
	unsigned long long phase_cold_pages;
	unsigned long long phase_not_cold_pages;
	unsigned long long phase_reaccessed_cold;
	unsigned long long phase_qd_overflow;
};

struct profile_stat {
	unsigned long long nr_profiled;
	unsigned long long nr_max_pages;
	unsigned long long nr_untracked;	/* samples of pages never seen by the tracker */
};

struct move_stat {
	unsigned long long nr_iters;
	unsigned long long nr_try_pages;
	unsigned long long nr_moved_pages;
	unsigned long long nr_successed_pages;
	unsigned long long nr_move_from_to[MAX_NODES][MAX_NODES];
};

struct page_profile;
typedef std::list<std::pair<void *, struct page_profile *>> page_list;
typedef std::unordered_map<void *, page_list::iterator> page_index;

/* Allocation zone: cold pages waiting for quick demotion, one queue per tier. */
struct alloc_metadata_t {
	page_list pages_to_move_lists[MAX_NODES];
	page_index pages_to_move_dict;		/* every queued page */
	/* pages evicted from a full queue on T0/T1; demoted in the next phase */
	page_list overflow_list;
	page_index overflow_dict;
};

struct hist_bin {
	unsigned long nr_pages;
	unsigned long nr_pages_tier[MAX_NODES];
	long nr_added;
	long nr_deleted;
	page_index va_set;
	page_list va_lists[MAX_NODES];
};

/*
 * Pages are keyed by (pid, virtual address) packed into one pointer-sized
 * word: 29 bits of pid above 35 bits of page frame number, which covers the
 * 47-bit user address space.
 */
#define PT_VA_SHIFT 12
#define PT_VA_BITS 35
#define PT_VA_MASK ((1UL << PT_VA_BITS) - 1)
#define PT_PID_SHIFT PT_VA_BITS

static inline void *MK_KEY(int pid, unsigned long va)
{
	return (void *)(((unsigned long)pid << PT_PID_SHIFT) | ((va >> PT_VA_SHIFT) & PT_VA_MASK));
}
static inline unsigned long KEY_VA(void *key)
{
	return (((unsigned long)key) & PT_VA_MASK) << PT_VA_SHIFT;
}
static inline int KEY_PID(void *key)
{
	return (int)(((unsigned long)key) >> PT_PID_SHIFT);
}

struct page_profile {
	uint64_t hotness = UINT64_MAX;
	int age = 0;		/* cooling round of the last hotness update */
	int node = -1;		/* tier the page resides on */
	int next_node = NO_MIG;	/* migration target selected in this phase */
	int bin_idx = -1;	/* histogram bin, -1 if not in the histogram */
	int hist_node = -1;	/* tier list the page is filed under in its bin */
	uint64_t alloc_ts_ms = 0;	/* allocation time; 0 once the first sample arrived */
};

struct promo_path {
	int bin;
	int src;
	int dst;
};

struct pebs_metadata_t {
	/* samples per page since the last profiling pass */
	std::unordered_map<void *, int> profiled_va;
	struct pebs_period period;
};

struct opts {
	int idx;
	char *exename;
	int do_quick_demotion;
	int dyn_alpha;
	double alpha_min;
	int print_itv;
	int verbose_level;
};

struct migflow {
	int fd;
	int pid;
	struct rb_head_t *rb[MAX_NR_RB];
	struct rb_data_t *rb_buf[MAX_NR_RB];
	struct alloc_metadata_t alloc_meta;
	struct pebs_metadata_t pebs_meta;
	int profiled_accesses;	/* since the last cooling round */
	/* sampled accesses per resident tier in the current phase (for h_T0) */
	unsigned long long nr_samples_node[MAX_NODES];
	double h_t0;
	double alpha_cur;
	std::unordered_map<void *, struct page_profile *> g_page_map;
	struct hist_bin hist[NR_HIST_BINS];

	pthread_t tid;
	int age;		/* current cooling round */
	std::atomic<bool> thread_stop;
	struct opts opts;

	struct alloc_stat astat;
	struct profile_stat pfstat;
	struct move_stat pstat;	/* promotion */
	struct move_stat dstat;	/* LFU demotion */
	struct move_stat qdstat;	/* quick demotion */

	/* time from allocation to first sampled access: <1s <2s <4s <8s <16s <32s <60s >60s */
	unsigned long long burst_hist_phase[8];
};

extern struct migflow kmig;

#define LOG_ALWAYS 0	/* summary and one line per migration phase */
#define LOG_INFO 1	/* every migration step */
#define LOG_DEBUG 2

void mf_log(int level, const char *fmt, ...);
int migflow_init(int pid, struct opts *opts);
void migflow_destroy(void);

#endif
