/*
 * Migration side of umigratord: quick demotion of cooled pages, cost-benefit
 * promotion, LFU demotion, the per-phase budget, and the main loop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <numa.h>
#include <numaif.h>
#include <limits.h>
#include <string.h>
#include <signal.h>
#include <cmath>
#include <cstdarg>
#include <algorithm>
#include <chrono>
#include <map>
#include <unordered_map>
#include <vector>
#include "migflow.h"
#include "profile.h"

using namespace std;
using namespace std::chrono;

/* the latency and cost tables and several log lines assume four tiers */
static_assert(MAX_NODES == 4, "the daemon is written for four memory tiers");

struct migflow kmig;

typedef vector<unordered_map<void *, struct page_profile *>> page_sets;

/*
 * Tier access latencies (ns) and page migration costs (memcpy of one 4 KB
 * page between tiers, in units of 10 ns) measured on the evaluation
 * platform: local DRAM, remote DRAM, local PMEM, remote PMEM. Up to the
 * scale factors below, the promotion gain of a page is
 *   gain = accesses * (lat[src] - lat[dst]) - alpha * cost[src][dst]
 * where alpha is scaled dynamically from the T0 hit ratio.
 */
static const uint64_t tier_lat[MAX_NODES] = { 80, 130, 300, 350 };
static const uint64_t mig_cost[MAX_NODES][MAX_NODES] = {
	{ UINT_MAX, 1091, 1917, 3879 },
	{ 1162, UINT_MAX, 2032, 3956 },
	{ 1934, 1802, UINT_MAX, 4249 },
	{ 3005, 2736, 2686, UINT_MAX } };
/* scale factors bringing the hotness (lower bound of a bin) and the cost to the same unit */
#define HOTNESS_BIN_SCALE 200
#define MIG_COST_SCALE 20000.0
#define ALPHA_BASE 4.0

static volatile sig_atomic_t stats_requested;

void mf_log(int level, const char *fmt, ...)
{
	if (level > kmig.opts.verbose_level)
		return;
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	fflush(stdout);
}

static inline long long get_numa_nr_free_pages(int node)
{
	long long fr;

	if (numa_node_size64(node, &fr) < 0)
		return 0;
	return fr / PAGE_SIZE;
}

static void print_move_stat(const char *name, struct move_stat &stat)
{
	mf_log(LOG_ALWAYS, "%s: iters %llu, tried %llu, moved %llu, succeeded %llu pages\n",
	       name, stat.nr_iters, stat.nr_try_pages, stat.nr_moved_pages, stat.nr_successed_pages);
	mf_log(LOG_ALWAYS, "  pages moved from tier (row) to tier (column):\n");
	for (int from = 0; from < MAX_NODES; from++) {
		mf_log(LOG_ALWAYS, "  ");
		for (int to = 0; to < MAX_NODES; to++)
			mf_log(LOG_ALWAYS, "%10llu", stat.nr_move_from_to[from][to]);
		mf_log(LOG_ALWAYS, "\n");
	}
}

static void print_stats(void)
{
	struct alloc_stat &a = kmig.astat;

	mf_log(LOG_ALWAYS, "\n=== umigratord statistics ===\n");
	mf_log(LOG_ALWAYS, "allocation tracker: %llu pages, cold %llu, warm %llu, re-accessed cold %llu\n",
	       a.nr_drain_pages, a.nr_cold_pages, a.nr_not_cold_pages, a.nr_reaccessed_cold_pages);
	mf_log(LOG_ALWAYS, "  allocated on tier 0..%d:", MAX_NODES - 1);
	for (int i = 0; i < MAX_NODES; i++)
		mf_log(LOG_ALWAYS, " %llu", a.nr_alloc_pages[i]);
	mf_log(LOG_ALWAYS, "\n");
	mf_log(LOG_ALWAYS, "profiler: %llu samples, %llu profiled pages at peak, %llu samples of untracked pages\n",
	       kmig.pfstat.nr_profiled, kmig.pfstat.nr_max_pages, kmig.pfstat.nr_untracked);
	print_move_stat("promotion", kmig.pstat);
	print_move_stat("quick demotion", kmig.qdstat);
	print_move_stat("LFU demotion", kmig.dstat);
	print_hist(kmig.hist, false);
}

/* SIGUSR1 requests a statistics dump from the migration thread. */
static void sig_handler_usr(int signo)
{
	stats_requested = 1;
}

static int __move_pages(int pid, int count, void **target_pages, int *nodes, int *status)
{
	int moved_pages = 0;

	while (moved_pages < count) {
		int nr = min(count - moved_pages, NR_MOVE_PAGES);

		if (move_pages(pid, nr, target_pages + moved_pages,
			       nodes ? nodes + moved_pages : NULL,
			       status + moved_pages, MPOL_MF_MOVE_ALL) < 0) {
			mf_log(LOG_DEBUG, "move_pages: %s\n", strerror(errno));
			break;
		}
		moved_pages += nr;
	}
	return moved_pages;
}

/*
 * target_pages[] holds page keys. Unpack them per owning process, issue
 * move_pages(2) with the real addresses and scatter the status back.
 */
static int move_pages_keyed(int count, void **key_pages, int *nodes, int *status)
{
	static vector<void *> vbuf;
	static vector<int> nbuf, sbuf, ibuf;
	unordered_map<int, int> owners;
	int total_moved = 0;

	if (!count)
		return 0;

	for (int i = 0; i < count; i++)
		owners[KEY_PID(key_pages[i])]++;

	for (auto &o : owners) {
		int pid = o.first;

		vbuf.clear(); nbuf.clear(); sbuf.clear(); ibuf.clear();
		for (int i = 0; i < count; i++) {
			if (KEY_PID(key_pages[i]) != pid)
				continue;
			vbuf.push_back((void *)KEY_VA(key_pages[i]));
			nbuf.push_back(nodes[i]);
			sbuf.push_back(INT_MAX);
			ibuf.push_back(i);
		}
		total_moved += __move_pages(pid, (int)vbuf.size(), vbuf.data(), nbuf.data(), sbuf.data());
		for (size_t k = 0; k < ibuf.size(); k++)
			status[ibuf[k]] = sbuf[k];
	}
	return total_moved;
}

/*
 * Gain-rank table: every (bin, src, dst) triple with a positive gain, sorted
 * from the highest gain down. alpha_factor = alpha / ALPHA_BASE.
 */
static vector<struct promo_path> calc_promo_order(double alpha_factor)
{
	multimap<int64_t, struct promo_path> by_gain;
	vector<struct promo_path> order;

	for (int i = 0; i < NR_HIST_BINS; i++) {
		int64_t min_access = (int64_t)((pow(2.0, i) - 1) * HOTNESS_BIN_SCALE);

		for (int src = 0; src < MAX_NODES; src++) {
			for (int dst = 0; dst < MAX_NODES; dst++) {
				if (src <= dst)
					continue;
				int64_t cost = (int64_t)((double)mig_cost[src][dst] * MIG_COST_SCALE * alpha_factor);
				int64_t benefit = min_access * (int64_t)(tier_lat[src] - tier_lat[dst]);
				by_gain.insert({benefit - cost, {i, src, dst}});
			}
		}
	}

	for (auto it = by_gain.rbegin(); it != by_gain.rend(); ++it) {
		if (it->first <= 0)
			continue;
		order.push_back(it->second);
	}

	mf_log(LOG_DEBUG, "[Promotion order] bin src dst\n");
	for (auto &p : order)
		mf_log(LOG_DEBUG, "%d %d %d\n", p.bin, p.src, p.dst);
	return order;
}

/*
 * alpha = max(ALPHA_BASE * h_T0, alpha_min), where h_T0 is the share of
 * sampled accesses served from tier 0 during the last phase. A phase without
 * samples keeps the previous alpha.
 */
static double update_dyn_alpha(void)
{
	unsigned long long tot = 0;

	if (!kmig.opts.dyn_alpha) {
		kmig.alpha_cur = ALPHA_BASE;
		return 1.0;
	}
	for (int i = 0; i < MAX_NODES; i++)
		tot += kmig.nr_samples_node[i];
	if (tot > 0) {
		kmig.h_t0 = (double)kmig.nr_samples_node[0] / (double)tot;
		kmig.alpha_cur = max(ALPHA_BASE * kmig.h_t0, kmig.opts.alpha_min);
	} else if (kmig.alpha_cur <= 0) {
		kmig.alpha_cur = ALPHA_BASE;
	}
	mf_log(LOG_INFO, "[Alpha] samples per tier: %llu %llu %llu %llu, h_T0 %.3f, alpha %.2f\n",
	       kmig.nr_samples_node[0], kmig.nr_samples_node[1], kmig.nr_samples_node[2],
	       kmig.nr_samples_node[3], kmig.h_t0, kmig.alpha_cur);
	for (int i = 0; i < MAX_NODES; i++)
		kmig.nr_samples_node[i] = 0;
	return kmig.alpha_cur / ALPHA_BASE;
}

/* Walk the gain-rank table and pick up to nr_pages promotion candidates. */
static pair<vector<int>, vector<int>> select_promo_cand(int nr_pages, struct hist_bin *hist, page_sets &promo_target_pages)
{
	struct hist_bin *bin;
	struct page_profile *page_info;
	int nr_promo_pages = 0;
	int min_bin_for_tier[MAX_NODES];
	long long tier_free[MAX_NODES];
	vector<int> nr_promo_from(MAX_NODES, 0);
	vector<int> nr_promo_to(MAX_NODES, 0);

	auto promo_order = calc_promo_order(update_dyn_alpha());

	/* coldest occupied bin of every tier */
	for (int t = 0; t < MAX_NODES; t++) {
		min_bin_for_tier[t] = -1;
		for (int b = 0; b < NR_HIST_BINS; b++) {
			if (hist[b].nr_pages_tier[t] > 0) {
				min_bin_for_tier[t] = b;
				break;
			}
		}
	}
	for (int t = 0; t < MAX_NODES; t++)
		tier_free[t] = get_numa_nr_free_pages(t) - (long long)NR_MARGIN_PAGES;

	for (auto &path : promo_order) {
		bin = hist + path.bin;

		/* a full destination only accepts pages hotter than its coldest one */
		if (tier_free[path.dst] <= 0 && min_bin_for_tier[path.dst] >= 0 &&
		    path.bin <= min_bin_for_tier[path.dst])
			continue;
		if (bin->va_set.empty())
			continue;

		for (auto &cand : bin->va_lists[path.src]) {
			if (nr_promo_pages == nr_pages)
				break;
			page_info = cand.second;
			if (page_info->next_node != NO_MIG)
				continue;

			page_info->next_node = path.dst;
			promo_target_pages[page_info->node].insert({cand.first, page_info});
			nr_promo_pages++;
			nr_promo_from[page_info->node]++;
			nr_promo_to[path.dst]++;
		}
		if (nr_promo_pages == nr_pages)
			break;
	}

	mf_log(LOG_INFO, "[Select promotion] %d pages\n", nr_promo_pages);
	return {nr_promo_from, nr_promo_to};
}

/* Drop a page from the quick-demotion queue and the overflow list. */
static void forget_queued(void *key)
{
	struct alloc_metadata_t &alloc_meta = kmig.alloc_meta;

	auto it = alloc_meta.pages_to_move_dict.find(key);
	if (it != alloc_meta.pages_to_move_dict.end()) {
		alloc_meta.pages_to_move_lists[it->second->second->node].erase(it->second);
		alloc_meta.pages_to_move_dict.erase(it);
	}
	auto ovf_it = alloc_meta.overflow_dict.find(key);
	if (ovf_it != alloc_meta.overflow_dict.end()) {
		alloc_meta.overflow_list.erase(ovf_it->second);
		alloc_meta.overflow_dict.erase(ovf_it);
	}
}

/* Update the profiles after a batch of moves. Returns the number of pages moved as requested. */
static int account_moves(int count, void **target_pages, int *nodes, int *status,
			 struct move_stat &stat, bool demotion)
{
	int nr_successes = 0;
	struct page_profile *page_info;

	for (int i = 0; i < count; i++) {
		page_info = kmig.g_page_map[target_pages[i]];
		bool is_qd = demotion && (page_info->next_node & FLAG_QD);
		int old_node = page_info->node;

		if (status[i] == INT_MAX) {
			/* not attempted */
			page_info->next_node = NO_MIG;
		} else if (status[i] == nodes[i]) {
			page_info->node = nodes[i];
			page_info->next_node = NO_MIG;
			nr_successes++;
			stat.nr_successed_pages++;
			stat.nr_move_from_to[old_node][nodes[i]]++;

			if (is_qd) {
				forget_queued(target_pages[i]);
			} else {
				delete_hist_bin_va(kmig.hist + page_info->bin_idx, (unsigned long)target_pages[i], page_info->hist_node);
				add_hist_bin_va(kmig.hist + page_info->bin_idx, (unsigned long)target_pages[i], page_info, page_info->node);
			}
		} else if (status[i] < 0) {
			if (status[i] == -EFAULT || status[i] == -ENOENT) {
				/* the page is gone: forget it */
				if (!is_qd)
					delete_hist_bin_va(kmig.hist + page_info->bin_idx, (unsigned long)target_pages[i],
							   page_info->hist_node);
				forget_queued(target_pages[i]);
				kmig.g_page_map.erase(target_pages[i]);
				delete page_info;
			} else {
				mf_log(LOG_DEBUG, "move_pages status %d\n", status[i]);
				page_info->next_node = NO_MIG;
			}
		} else {
			/* moved somewhere else than requested: record where it is now */
			page_info->node = status[i];
			page_info->next_node = NO_MIG;
			if (is_qd) {
				forget_queued(target_pages[i]);
			} else {
				delete_hist_bin_va(kmig.hist + page_info->bin_idx, (unsigned long)target_pages[i], page_info->hist_node);
				add_hist_bin_va(kmig.hist + page_info->bin_idx, (unsigned long)target_pages[i], page_info, page_info->node);
			}
		}
	}
	return nr_successes;
}

static int do_promotion(int count, void **target_pages, int *nodes, int *status,
			unordered_map<void *, struct page_profile *> &promo_target_pages)
{
	int idx = 0;

	if (!count)
		return 0;

	for (auto &p : promo_target_pages) {
		target_pages[idx] = p.first;
		nodes[idx] = p.second->next_node;
		status[idx] = INT_MAX;
		idx++;
	}
	if (idx != count) {
		fprintf(stderr, "promotion: %d candidates for %d slots\n", idx, count);
		ABORT_WITH_LOG();
	}

	int nr_moved = move_pages_keyed(count, target_pages, nodes, status);

	kmig.pstat.nr_iters++;
	kmig.pstat.nr_try_pages += count;
	kmig.pstat.nr_moved_pages += nr_moved;

	int nr_successes = account_moves(count, target_pages, nodes, status, kmig.pstat, false);

	promo_target_pages.clear();
	mf_log(LOG_INFO, "[Promotion] %d of %d pages moved\n", nr_successes, count);
	return nr_successes;
}

static int do_demotion(int count, void **target_pages, int *nodes, int *status,
		       unordered_map<void *, struct page_profile *> &demo_target_pages)
{
	int idx = 0;

	if (!count)
		return 0;

	for (auto &p : demo_target_pages) {
		target_pages[idx] = p.first;
		nodes[idx] = p.second->next_node & (FLAG_QD - 1);
		status[idx] = INT_MAX;
		idx++;
	}
	if (idx != count) {
		fprintf(stderr, "demotion: %d candidates for %d slots\n", idx, count);
		ABORT_WITH_LOG();
	}

	int nr_moved = move_pages_keyed(count, target_pages, nodes, status);
	bool is_qd = demo_target_pages.begin()->second->next_node & FLAG_QD;
	struct move_stat &stat = is_qd ? kmig.qdstat : kmig.dstat;

	stat.nr_iters++;
	stat.nr_try_pages += count;
	stat.nr_moved_pages += nr_moved;

	int nr_successes = account_moves(count, target_pages, nodes, status, stat, true);

	demo_target_pages.clear();
	mf_log(LOG_INFO, "[%s] %d of %d pages moved\n", is_qd ? "Quick demotion" : "LFU demotion", nr_successes, count);
	return nr_successes;
}

/*
 * LFU demotion: every tier that has to receive more promotions than it has
 * free pages demotes its least-frequently-used pages to the next slower
 * tier, at most NR_DEMOTE_PAGES per tier and lfu_budget pages in total.
 * Demotions into a tier consume its free pages and can in turn make it
 * demote to the tier below.
 */
static pair<vector<int>, vector<int>> select_demo_cand(vector<int> nr_promo_pages, struct hist_bin *hist,
						       page_sets &demo_target_pages, long long lfu_budget)
{
	/* pages a tier has to shed: incoming pages beyond its free space (negative = spare room) */
	vector<long long> nr_demo_pages(MAX_NODES, 0);
	vector<int> nr_demo_from(MAX_NODES, 0);
	vector<int> nr_demo_to(MAX_NODES, 0);
	struct hist_bin *bin;
	struct page_profile *page_info;
	bool need_to_scan = false;

	for (int i = 0; i < MAX_NODES; i++) {
		nr_demo_pages[i] = nr_promo_pages[i] - (get_numa_nr_free_pages(i) - (long long)NR_MARGIN_PAGES);
		if (i < MAX_NODES - 1 && nr_demo_pages[i] > 0)
			need_to_scan = true;
	}
	if (!need_to_scan)
		return {nr_demo_from, nr_demo_to};

	long long lfu_left = lfu_budget < 0 ? LLONG_MAX : lfu_budget;
	for (int cur_node = 0; cur_node < MAX_NODES - 1 && lfu_left > 0; cur_node++) {
		int demo_target = cur_node + 1;
		long long quota = min(nr_demo_pages[cur_node], (long long)NR_DEMOTE_PAGES);

		for (int bin_idx = 0; bin_idx < NR_HIST_BINS && quota > 0 && lfu_left > 0; bin_idx++) {
			bin = hist + bin_idx;
			for (auto &cand : bin->va_lists[cur_node]) {
				page_info = cand.second;
				if (page_info->next_node != NO_MIG)
					continue;

				page_info->next_node = demo_target;
				nr_demo_pages[demo_target]++;
				nr_demo_from[cur_node]++;
				nr_demo_to[demo_target]++;
				demo_target_pages[cur_node].insert({cand.first, page_info});
				quota--;
				lfu_left--;
				if (quota <= 0 || lfu_left <= 0)
					break;
			}
		}
	}

	mf_log(LOG_INFO, "[Select LFU demotion] from tiers: %d %d %d\n", nr_demo_from[0], nr_demo_from[1], nr_demo_from[2]);
	return {nr_demo_from, nr_demo_to};
}

/*
 * Quick demotion candidates, up to qd_budget pages: first the overflow list,
 * then the per-tier queues from the fastest tier down. Cooled pages go
 * directly to the slowest tier with free space. Selected pages stay in their
 * list until account_moves() records the outcome of the move.
 */
static int select_qd_cand(long long qd_budget, page_sets &demo_pages, long long *free_pages)
{
	struct alloc_metadata_t &alloc_meta = kmig.alloc_meta;
	int total_selected = 0;

	if (qd_budget <= 0)
		return 0;

	for (auto &cand : alloc_meta.overflow_list) {
		if (qd_budget <= 0)
			break;
		struct page_profile *pinfo = cand.second;
		int src_node = pinfo->node;
		int dtarget = MAX_NODES - 1;

		if (pinfo->next_node != NO_MIG)
			continue;
		while (dtarget > src_node && free_pages[dtarget] <= 0)
			dtarget--;
		if (dtarget <= src_node)
			continue;
		pinfo->next_node = dtarget | FLAG_QD;
		demo_pages[src_node].insert({cand.first, pinfo});
		free_pages[src_node]++;
		free_pages[dtarget]--;
		total_selected++;
		qd_budget--;
	}

	for (int i = 0; i < MAX_NODES - 1 && qd_budget > 0; i++) {
		for (auto &cand : alloc_meta.pages_to_move_lists[i]) {
			if (qd_budget <= 0)
				break;
			int dtarget = MAX_NODES - 1;

			while (dtarget > i && free_pages[dtarget] <= 0)
				dtarget--;
			if (dtarget <= i)
				break;

			struct page_profile *pinfo = cand.second;
			if (pinfo->next_node != NO_MIG)
				continue;
			pinfo->next_node = dtarget | FLAG_QD;
			demo_pages[i].insert({cand.first, pinfo});
			free_pages[i]++;
			free_pages[dtarget]--;
			total_selected++;
			qd_budget--;
		}
	}
	return total_selected;
}

/* Quick demotion on its own timer, QD_RATE_LIMIT pages at a time. */
static int do_quick_demotion_standalone(void **target_pages, int *nodes, int *status)
{
	static unsigned long long accum_qd_count = 0;
	struct alloc_metadata_t &alloc_meta = kmig.alloc_meta;
	page_sets demo_pages(MAX_NODES);
	long long free_pages[MAX_NODES];
	int total_moved = 0;

	auto start = high_resolution_clock::now();
	for (int i = 0; i < MAX_NODES; i++)
		free_pages[i] = get_numa_nr_free_pages(i) - (long long)NR_MARGIN_PAGES;

	select_qd_cand((long long)QD_RATE_LIMIT, demo_pages, free_pages);
	for (int i = MAX_NODES - 1; i >= 0; i--)
		total_moved += do_demotion(demo_pages[i].size(), target_pages, nodes, status, demo_pages[i]);
	accum_qd_count += total_moved;

	auto duration = duration_cast<milliseconds>(high_resolution_clock::now() - start);
	mf_log(LOG_INFO, "[QD %ldms] moved %d MB (total %.1f GB) | queued T0:%lu MB T1:%lu MB T2:%lu MB | overflow %lu MB\n",
	       duration.count(), total_moved * 4 / 1024, (double)accum_qd_count * 4 / 1024 / 1024,
	       alloc_meta.pages_to_move_lists[0].size() * 4 / 1024,
	       alloc_meta.pages_to_move_lists[1].size() * 4 / 1024,
	       alloc_meta.pages_to_move_lists[2].size() * 4 / 1024,
	       alloc_meta.overflow_list.size() * 4 / 1024);
	return total_moved;
}

static void print_phase_stats(struct hist_bin *hist)
{
	struct alloc_stat &a = kmig.astat;
	unsigned long long total_alloc = a.phase_cold_pages + a.phase_not_cold_pages;
	double cold_ratio = total_alloc > 0 ? (double)a.phase_cold_pages / total_alloc * 100.0 : 0;
	double reaccess_ratio = a.phase_cold_pages > 0 ? (double)a.phase_reaccessed_cold / a.phase_cold_pages * 100.0 : 0;

	mf_log(LOG_ALWAYS, "[Alloc] pages %llu, cold %llu (%.1f%%), warm %llu, re-accessed %llu (%.1f%%), overflow %llu | queued T0:%lu MB T1:%lu MB T2:%lu MB\n",
	       a.phase_drain_pages, a.phase_cold_pages, cold_ratio, a.phase_not_cold_pages,
	       a.phase_reaccessed_cold, reaccess_ratio, a.phase_qd_overflow,
	       kmig.alloc_meta.pages_to_move_lists[0].size() * 4 / 1024,
	       kmig.alloc_meta.pages_to_move_lists[1].size() * 4 / 1024,
	       kmig.alloc_meta.pages_to_move_lists[2].size() * 4 / 1024);
	a.phase_drain_pages = a.phase_cold_pages = a.phase_not_cold_pages = 0;
	a.phase_reaccessed_cold = 0;
	a.phase_qd_overflow = 0;

	unsigned long long *bh = kmig.burst_hist_phase;
	unsigned long long bh_total = 0;
	for (int i = 0; i < 8; i++)
		bh_total += bh[i];
	if (bh_total > 0)
		mf_log(LOG_INFO, "[Burst] time to first sampled access <1s:%llu <2s:%llu <4s:%llu <8s:%llu <16s:%llu <32s:%llu <60s:%llu >60s:%llu\n",
		       bh[0], bh[1], bh[2], bh[3], bh[4], bh[5], bh[6], bh[7]);
	memset(kmig.burst_hist_phase, 0, sizeof(kmig.burst_hist_phase));

	for (int t = 0; t < MAX_NODES; t++) {
		unsigned long total = 0, hot = 0;

		for (int b = 0; b < NR_HIST_BINS; b++) {
			total += hist[b].nr_pages_tier[t];
			if (b > 16)
				hot += hist[b].nr_pages_tier[t];
		}
		if (total > 0)
			mf_log(LOG_INFO, "[Tier %d] %lu MB profiled, %lu MB in bins above 16\n", t, total * 4 / 1024, hot * 4 / 1024);
	}
}

/* One migration phase with a single shared budget (see migflow.h). */
static void do_migration(int nr_promo_pages, struct hist_bin *hist, void **target_pages,
			 int *nodes, int *status, bool do_quick_demotion)
{
	static unsigned long long accum_promo_count = 0, accum_demo_count = 0;
	page_sets promo_target_pages(MAX_NODES);
	page_sets demo_target_pages(MAX_NODES);
	long long budget = (long long)NR_TOTAL_BUDGET;
	int qd1_count = 0, qd2_count = 0, demo_count = 0, promo_count = 0;

	auto start = high_resolution_clock::now();
	print_phase_stats(hist);

	/* 1. quick demotion, at most half of the budget */
	if (do_quick_demotion) {
		page_sets qd_pages(MAX_NODES);
		long long qd_free[MAX_NODES];

		for (int i = 0; i < MAX_NODES; i++)
			qd_free[i] = get_numa_nr_free_pages(i) - (long long)NR_MARGIN_PAGES;
		select_qd_cand(min((long long)NR_QD_FIRST_PAGES, budget), qd_pages, qd_free);
		for (int i = MAX_NODES - 1; i >= 0; i--)
			qd1_count += do_demotion(qd_pages[i].size(), target_pages, nodes, status, qd_pages[i]);
		budget -= qd1_count;
	}

	/* 2. promotion candidates */
	long long promo_budget = min((long long)nr_promo_pages, budget);
	vector<int> promo_from(MAX_NODES, 0), promo_to(MAX_NODES, 0);
	if (promo_budget > 0)
		tie(promo_from, promo_to) = select_promo_cand((int)promo_budget, hist, promo_target_pages);

	/* 3. LFU demotion to make room, keeping budget for the promotions */
	long long promo_selected = 0;
	for (int i = 0; i < MAX_NODES; i++)
		promo_selected += promo_from[i];
	auto [demo_from, demo_to] = select_demo_cand(promo_to, hist, demo_target_pages, max(0LL, budget - promo_selected));
	for (int i = MAX_NODES - 1; i >= 0; i--)
		demo_count += do_demotion(demo_from[i], target_pages, nodes, status, demo_target_pages[i]);
	budget -= demo_count;

	/* 4. promotions */
	for (int i = MAX_NODES - 1; i >= 0; i--)
		promo_count += do_promotion(promo_from[i], target_pages, nodes, status, promo_target_pages[i]);
	budget -= promo_count;

	/* 5. remaining budget: more quick demotion */
	if (do_quick_demotion && budget > 0) {
		page_sets qd_pages(MAX_NODES);
		long long qd_free[MAX_NODES];

		for (int i = 0; i < MAX_NODES; i++)
			qd_free[i] = get_numa_nr_free_pages(i) - (long long)NR_MARGIN_PAGES;
		select_qd_cand(budget, qd_pages, qd_free);
		for (int i = MAX_NODES - 1; i >= 0; i--)
			qd2_count += do_demotion(qd_pages[i].size(), target_pages, nodes, status, qd_pages[i]);
		budget -= qd2_count;
	}

	accum_promo_count += promo_count;
	accum_demo_count += demo_count + qd1_count + qd2_count;
	auto duration = duration_cast<milliseconds>(high_resolution_clock::now() - start);
	mf_log(LOG_ALWAYS, "[Phase %ldms] quick demotion %d MB, promotion %d MB (total %.1f GB), LFU demotion %d MB, quick demotion %d MB | %d of %lu MB used, alpha %.2f\n",
	       duration.count(), qd1_count * 4 / 1024,
	       promo_count * 4 / 1024, (double)accum_promo_count * 4 / 1024 / 1024,
	       demo_count * 4 / 1024, qd2_count * 4 / 1024,
	       (promo_count + demo_count + qd1_count + qd2_count) * 4 / 1024,
	       NR_TOTAL_BUDGET * 4 / 1024, kmig.alpha_cur);
}

static inline int get_pebs_period(int fd)
{
	if (ioctl(fd, IOCTL_GET_PERIOD, &kmig.pebs_meta.period) < 0) {
		perror("Failed to read the sampling period");
		return -1;
	}
	return 0;
}

static void *migflow_run(void *arg)
{
	bool do_quick_demotion = kmig.opts.do_quick_demotion;
	int print_itv = kmig.opts.print_itv;
	bool alloc_occured, profile_occured;

	void **target_pages = (void **)calloc(NR_MAX_MIG_PAGES, sizeof(void *));
	int *nodes = (int *)calloc(NR_MAX_MIG_PAGES, sizeof(int));
	int *status = (int *)calloc(NR_MAX_MIG_PAGES, sizeof(int));
	if (!target_pages || !nodes || !status) {
		fprintf(stderr, "umigratord: out of memory\n");
		return (void *)-1;
	}

	auto cur_time = high_resolution_clock::now();
	auto prev_print_time = cur_time;
	auto prev_period_time = cur_time;
	auto prev_mig_time = cur_time;
	auto prev_qd_time = cur_time;

	get_pebs_period(kmig.fd);
	mf_log(LOG_ALWAYS, "[Sampling period] loads %lu, stores %lu\n",
	       kmig.pebs_meta.period.read, kmig.pebs_meta.period.write);

	while (!kmig.thread_stop) {
		alloc_occured = profile_occured = false;
		if (drain(alloc_occured, profile_occured) < 0) {
			fprintf(stderr, "umigratord: lost the kernel module; the application continues unmanaged\n");
			break;
		}

		if (alloc_occured && !do_quick_demotion) {
			for (int i = 0; i < MAX_NODES; i++)
				kmig.alloc_meta.pages_to_move_lists[i].clear();
			kmig.alloc_meta.pages_to_move_dict.clear();
		}
		if (profile_occured)
			profile_pages(kmig.age);

		cur_time = high_resolution_clock::now();
		if (stats_requested ||
		    (print_itv != -1 && duration_cast<milliseconds>(cur_time - prev_print_time).count() >= print_itv * 1000)) {
			stats_requested = 0;
			print_stats();
			prev_print_time = cur_time;
		}
		if (duration_cast<milliseconds>(cur_time - prev_period_time).count() >= PERIOD_INTERVAL * 1000) {
			get_pebs_period(kmig.fd);
			prev_period_time = cur_time;
		}
		if (do_quick_demotion && duration_cast<milliseconds>(cur_time - prev_qd_time).count() >= QD_INTERVAL * 1000) {
			do_quick_demotion_standalone(target_pages, nodes, status);
			prev_qd_time = cur_time;
		}
		if (duration_cast<milliseconds>(cur_time - prev_mig_time).count() >= MIG_INTERVAL * 1000) {
			print_hist(kmig.hist, false);
			do_migration(NR_PROMOTE_PAGES, kmig.hist, target_pages, nodes, status, do_quick_demotion);
			prev_mig_time = cur_time;
		}
		if (kmig.profiled_accesses >= COOLING_INTERVAL) {
			print_hist(kmig.hist, true);
			kmig.age++;
			do_cooling(kmig.hist, kmig.age);
			kmig.profiled_accesses = 0;
		}
	}

	free(target_pages);
	free(nodes);
	free(status);
	return NULL;
}

int migflow_init(int pid, struct opts *opts)
{
	kmig.opts = *opts;

	if (init_profile(pid) < 0)
		return -1;
	if (setup_drain() < 0 || start_profile() < 0) {
		destroy_profile();
		return -1;
	}

	kmig.thread_stop = false;
	signal(SIGUSR1, sig_handler_usr);
	if (pthread_create(&kmig.tid, NULL, migflow_run, NULL)) {
		perror("pthread_create");
		destroy_profile();
		return -1;
	}
	pthread_setname_np(kmig.tid, "umigratord");
	return 0;
}

void migflow_destroy(void)
{
	kmig.thread_stop = true;
	pthread_join(kmig.tid, NULL);
	destroy_profile();
	print_stats();
}
