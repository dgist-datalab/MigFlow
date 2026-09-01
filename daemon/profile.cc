/*
 * Profiling side of umigratord: drains the kernel ring buffers, maintains the
 * per-page profiles and the access-count histogram, and cools the histogram.
 */
#include <stdio.h>
#include <sys/epoll.h>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cerrno>
#include <string.h>
#include "migflow.h"
#include "profile.h"
using namespace std;
using namespace std::chrono;

static int epoll_fd;
static struct epoll_event ev, events[MAX_EVENTS];

static inline int get_rb_status(int fd)
{
	int flag = 0;

	if (ioctl(fd, IOCTL_GET_RB_STATUS, &flag) < 0) {
		perror("Failed to get the ring buffer status");
		return -1;
	}
	return flag;
}

/* Histogram bin of a hotness value: bin n covers [2^n - 1, 2^(n+1) - 1). */
static inline int get_idx(uint64_t num)
{
	int cnt = 0;

	if (num == UINT64_MAX)
		return -1;

	num++;
	while (1) {
		num = num >> 1;
		if (num)
			cnt++;
		else
			return cnt;
		if (cnt == NR_HIST_BINS - 1)
			break;
	}
	return cnt;
}

/*
 * Exponentially decayed access count. PEBS samples are scaled to accesses
 * with the fixed reference period; allocation records contribute their
 * access bit as-is.
 */
static inline uint64_t calc_hotness(uint64_t old_hotness, unsigned int nr_accesses,
				    int age_diff, double weight, bool is_pebs = false)
{
	if (is_pebs)
		nr_accesses = nr_accesses * PEBS_HOTNESS_REF_PERIOD;

	if (old_hotness == UINT64_MAX)
		return (uint64_t)nr_accesses;

	while (age_diff--)
		old_hotness = (uint64_t)((double)old_hotness * weight);
	return old_hotness + (uint64_t)nr_accesses;
}

void add_hist_bin_va(struct hist_bin *bin, unsigned long va, struct page_profile *page_info, int node)
{
	if (node < 0 || node >= MAX_NODES) {
		fprintf(stderr, "add_hist_bin_va: invalid node %d\n", node);
		ABORT_WITH_LOG();
	}
	page_info->hist_node = node;

	bin->va_lists[node].push_back({(void *)va, page_info});
	auto last = bin->va_lists[node].end();
	auto res = bin->va_set.insert({(void *)va, prev(last)});
	if (!res.second) {
		fprintf(stderr, "add_hist_bin_va: duplicate page\n");
		ABORT_WITH_LOG();
	}

	bin->nr_pages_tier[node]++;
	bin->nr_pages++;
	if (bin->nr_pages != bin->va_set.size()) {
		fprintf(stderr, "add_hist_bin_va: nr_pages %lu != set size %lu\n", bin->nr_pages, bin->va_set.size());
		ABORT_WITH_LOG();
	}
	bin->nr_added++;
}

void delete_hist_bin_va(struct hist_bin *bin, unsigned long va, int node)
{
	auto it = bin->va_set.find((void *)va);

	if (it == bin->va_set.end()) {
		fprintf(stderr, "delete_hist_bin_va: page not in bin\n");
		ABORT_WITH_LOG();
	}
	if (node < 0 || node >= MAX_NODES) {
		fprintf(stderr, "delete_hist_bin_va: invalid node %d\n", node);
		ABORT_WITH_LOG();
	}

	bin->va_lists[node].erase(it->second);
	bin->va_set.erase(it);
	bin->nr_pages_tier[node]--;
	bin->nr_pages--;
	if (bin->nr_pages != bin->va_set.size()) {
		fprintf(stderr, "delete_hist_bin_va: nr_pages %lu != set size %lu\n", bin->nr_pages, bin->va_set.size());
		ABORT_WITH_LOG();
	}
	bin->nr_deleted++;
}

/* Move a page between bins after its hotness changed. */
static void update_hist(unsigned long va, uint64_t old_hotness, uint64_t new_hotness,
			int old_node, int new_node, struct page_profile *page_info, struct hist_bin *hist)
{
	int old_bin = get_idx(old_hotness);
	int new_bin = get_idx(new_hotness);

	if (old_bin != -1) {
		if (old_bin != page_info->bin_idx) {
			fprintf(stderr, "update_hist: bin mismatch\n");
			ABORT_WITH_LOG();
		}
		delete_hist_bin_va(hist + old_bin, va, old_node);
		page_info->bin_idx = -1;
	}

	if (new_bin != -1) {
		if (page_info->bin_idx != -1) {
			fprintf(stderr, "update_hist: page already binned (bin_idx=%d old_bin=%d new_bin=%d)\n",
				page_info->bin_idx, old_bin, new_bin);
			ABORT_WITH_LOG();
		}
		add_hist_bin_va(hist + new_bin, va, page_info, new_node);
		page_info->bin_idx = new_bin;
	}
}

void print_hist(struct hist_bin *hist, bool clear_stat)
{
	static int iter = 0;
	unsigned long total_nr_pages = 0;
	struct hist_bin *bin;

	mf_log(LOG_DEBUG, "[Histogram] iter %d\n", iter++);
	for (int i = 0; i < NR_HIST_BINS; i++) {
		bin = hist + i;
		total_nr_pages += bin->nr_pages;
		mf_log(LOG_DEBUG, "bin %d: %lu pages (%lu KB), +%ld/-%ld\n", i, bin->nr_pages,
		       bin->nr_pages * PAGE_SIZE / 1024, bin->nr_added, bin->nr_deleted);
		if (clear_stat)
			bin->nr_added = bin->nr_deleted = 0;
	}
	mf_log(LOG_DEBUG, "histogram total: %lu MB\n", total_nr_pages * PAGE_SIZE / 1024 / 1024);
}

int init_profile(int pid)
{
	kmig.fd = -1;
	kmig.pid = -1;

	int fd = open(MIGFLOW_DEVICE, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("Failed to open " MIGFLOW_DEVICE);
		return -1;
	}

	for (unsigned long i = 0; i < MAX_NR_RB; i++) {
		char *mapped_mem = (char *)mmap(NULL, RB_BUF_SIZE + RB_HEADER_SIZE, PROT_READ, MAP_SHARED, fd, 0);

		if (mapped_mem == MAP_FAILED) {
			perror("mmap");
			close(fd);
			return -1;
		}
		kmig.rb[i] = (struct rb_head_t *)mapped_mem;
		kmig.rb_buf[i] = (struct rb_data_t *)(mapped_mem + RB_HEADER_SIZE);
	}
	kmig.fd = fd;
	kmig.pid = pid;
	return 0;
}

void destroy_profile(void)
{
	kmig.pid = -1;
	for (unsigned long i = 0; i < MAX_NR_RB; i++) {
		if (kmig.rb[i]) {
			munmap(kmig.rb[i], RB_BUF_SIZE + RB_HEADER_SIZE);
			kmig.rb[i] = NULL;
			kmig.rb_buf[i] = NULL;
		}
	}
	if (kmig.fd >= 0) {
		close(kmig.fd);
		kmig.fd = -1;
	}
}

int setup_drain(void)
{
	epoll_fd = epoll_create1(0);
	if (epoll_fd == -1) {
		perror("epoll_create1");
		return -1;
	}
	ev.events = EPOLLIN;
	ev.data.fd = kmig.fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, kmig.fd, &ev) == -1) {
		perror("epoll_ctl");
		return -1;
	}
	return 0;
}

int start_profile(void)
{
	if (ioctl(kmig.fd, IOCTL_SET_PID, &kmig.pid) < 0) {
		perror("Failed to set the target pid");
		return -1;
	}
	return 0;
}

/* Remove a page from the quick-demotion queue and the overflow list. */
static void dequeue_page(struct alloc_metadata_t &alloc_meta, void *key)
{
	auto qit = alloc_meta.pages_to_move_dict.find(key);
	if (qit != alloc_meta.pages_to_move_dict.end()) {
		alloc_meta.pages_to_move_lists[qit->second->second->node].erase(qit->second);
		alloc_meta.pages_to_move_dict.erase(qit);
	}
	auto ovf_it = alloc_meta.overflow_dict.find(key);
	if (ovf_it != alloc_meta.overflow_dict.end()) {
		alloc_meta.overflow_list.erase(ovf_it->second);
		alloc_meta.overflow_dict.erase(ovf_it);
	}
}

/*
 * Allocation records: pages that left the 2Q tracker. Cold ones (not accessed
 * during the grace period) join the quick-demotion queue of their tier,
 * warm ones enter the access zone (the histogram).
 */
static int drain_rb_alloc(rb_head_t *rb, rb_data_t *rb_buf, struct alloc_metadata_t &alloc_meta)
{
	int head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE), tail = rb->tail;
	int len = (head + rb->size - tail) % rb->size;
	struct rb_data_alloc_t rb_alloc;
	unsigned long va;
	unordered_map<void *, struct page_profile *>::iterator it;
	struct page_profile *page_info;
	unsigned int last_accessed;
	uint64_t old_hotness, new_hotness;
	int old_node, new_node;
	int cur_age = kmig.age;

	for (int i = 0; i < len; i++) {
		rb_alloc = rb_buf[(tail + i) % rb->size].data.rb_alloc;
		old_node = new_node = UNKNOWN_NODE;

		if (rb_alloc.va % PAGE_SIZE)
			continue;

		kmig.astat.nr_drain_pages++;
		kmig.astat.phase_drain_pages++;

		if (rb_alloc.node < 0 || rb_alloc.node >= MAX_NODES)
			continue;

		old_hotness = UINT64_MAX;
		va = (unsigned long)MK_KEY(rb_alloc.pid, rb_alloc.va);
		last_accessed = rb_alloc.last_accessed;
		kmig.astat.nr_alloc_pages[rb_alloc.node]++;

		it = kmig.g_page_map.find((void *)va);
		if (it == kmig.g_page_map.end()) {
			page_info = new page_profile;
			kmig.g_page_map.insert({(void *)va, page_info});
			page_info->alloc_ts_ms = duration_cast<milliseconds>(high_resolution_clock::now().time_since_epoch()).count();
			new_hotness = calc_hotness(UINT64_MAX, last_accessed, 0, HOTNESS_WEIGHT);
			new_node = rb_alloc.node;
		} else {
			page_info = it->second;
			old_hotness = page_info->hotness;
			new_hotness = calc_hotness(old_hotness, last_accessed, cur_age - page_info->age, HOTNESS_WEIGHT);
			old_node = page_info->node;
			new_node = rb_alloc.node;
		}

		/* A page reported again is no longer pending in any queue. */
		dequeue_page(alloc_meta, (void *)va);

		if (!last_accessed) {
			kmig.astat.nr_cold_pages++;
			kmig.astat.phase_cold_pages++;
			if (kmig.opts.do_quick_demotion && page_info->bin_idx == -1 && new_node < MAX_NODES - 1) {
				alloc_meta.pages_to_move_lists[new_node].push_back({(void *)va, page_info});
				auto last = alloc_meta.pages_to_move_lists[new_node].end();
				alloc_meta.pages_to_move_dict.insert({(void *)va, prev(last)});

				/* Bounded queue: the oldest entries spill over. */
				while (alloc_meta.pages_to_move_lists[new_node].size() > QD_QUEUE_CAP) {
					auto &oldest = alloc_meta.pages_to_move_lists[new_node].front();
					void *evict_va = oldest.first;
					struct page_profile *evict_info = oldest.second;

					alloc_meta.pages_to_move_dict.erase(evict_va);
					alloc_meta.pages_to_move_lists[new_node].pop_front();

					if (new_node < MAX_NODES - 2) {
						/* fast tiers: keep for demotion in the migration phase */
						if (alloc_meta.overflow_dict.find(evict_va) == alloc_meta.overflow_dict.end()) {
							alloc_meta.overflow_list.push_back({evict_va, evict_info});
							auto ovf_last = alloc_meta.overflow_list.end();
							alloc_meta.overflow_dict.insert({evict_va, prev(ovf_last)});
						}
					} else {
						/* slow tiers: manage through the histogram */
						update_hist((unsigned long)evict_va, UINT64_MAX, evict_info->hotness,
							    UNKNOWN_NODE, evict_info->node, evict_info, kmig.hist);
					}
					kmig.astat.phase_qd_overflow++;
				}
			}
		} else {
			kmig.astat.nr_not_cold_pages++;
			kmig.astat.phase_not_cold_pages++;
		}

		page_info->hotness = new_hotness;
		page_info->age = cur_age;
		page_info->node = new_node;
		page_info->next_node = NO_MIG;

		if (!kmig.opts.do_quick_demotion || page_info->bin_idx != -1 || last_accessed) {
			if (page_info->bin_idx == -1) {
				old_hotness = UINT64_MAX;
				old_node = UNKNOWN_NODE;
			}
			update_hist(va, old_hotness, new_hotness, old_node, new_node, page_info, kmig.hist);
		}
	}
	return len;
}

/* PEBS records: aggregate samples per page until the next profiling pass. */
static int drain_rb_pebs(rb_head_t *rb, rb_data_t *rb_buf, struct pebs_metadata_t &pebs_meta)
{
	int head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE), tail = rb->tail;
	int len = (head + rb->size - tail) % rb->size;
	struct rb_data_pebs_t pdata;

	for (int i = 0; i < len; i++) {
		pdata = rb_buf[(tail + i) % rb->size].data.rb_pebs;
		if (pdata.va % PAGE_SIZE)
			continue;
		pebs_meta.profiled_va[MK_KEY(pdata.pid, pdata.va)]++;
	}

	kmig.pfstat.nr_profiled += len;
	return len;
}

static int __drain(int type, rb_head_t **rb, rb_data_t **rb_buf)
{
	struct rb_reply_t reply;
	int ret;

	switch (type) {
	case RB_ALLOC:
		ret = drain_rb_alloc(rb[type], rb_buf[type], kmig.alloc_meta);
		break;
	case RB_PEBS:
		ret = drain_rb_pebs(rb[type], rb_buf[type], kmig.pebs_meta);
		break;
	default:
		return -1;
	}

	reply.type = type;
	reply.nr_items = ret;
	if (ioctl(kmig.fd, IOCTL_MOVE_RB_TAIL, &reply) < 0) {
		perror("Failed to acknowledge ring buffer records");
		return -1;
	}
	return ret;
}

int drain(bool &alloc_occured, bool &profile_occured)
{
	int nfds, n, flag;

	nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT);
	if (nfds == -1) {
		if (errno == EINTR)
			return 0;
		perror("epoll_wait");
		return -1;
	}

	for (n = 0; n < nfds; ++n) {
		if (events[n].data.fd != kmig.fd || !(events[n].events & EPOLLIN))
			continue;
		flag = get_rb_status(kmig.fd);
		if (flag < 0)
			return -1;
		if (flag & RB_TYPE_TO_FLAG(RB_ALLOC)) {
			if (__drain(RB_ALLOC, kmig.rb, kmig.rb_buf) < 0)
				return -1;
			alloc_occured = true;
		}
		if (flag & RB_TYPE_TO_FLAG(RB_PEBS)) {
			if (__drain(RB_PEBS, kmig.rb, kmig.rb_buf) < 0)
				return -1;
			profile_occured = true;
		}
	}
	return 0;
}

/* Apply the aggregated PEBS samples to the page profiles and the histogram. */
static int profile_pages_pebs(struct pebs_metadata_t &pebs_meta, int cur_age, struct alloc_metadata_t &alloc_meta)
{
	int nr_accesses = 0;
	unordered_map<void *, struct page_profile *>::iterator it;
	uint64_t old_hotness, new_hotness;
	int old_node, new_node;
	struct page_profile *page_info;

	if (pebs_meta.profiled_va.empty())
		return 0;

	uint64_t now_ms = duration_cast<milliseconds>(high_resolution_clock::now().time_since_epoch()).count();
	for (auto &pva : pebs_meta.profiled_va) {
		int nr = pva.second;

		nr_accesses += nr;

		/* Only pages that went through the allocation tracker are profiled. */
		it = kmig.g_page_map.find(pva.first);
		if (it == kmig.g_page_map.end()) {
			kmig.pfstat.nr_untracked += nr;
			continue;
		}
		page_info = it->second;

		if (page_info->alloc_ts_ms > 0) {
			uint64_t delta = now_ms - page_info->alloc_ts_ms;
			int bucket = delta < 1000 ? 0 : delta < 2000 ? 1 : delta < 4000 ? 2 :
				     delta < 8000 ? 3 : delta < 16000 ? 4 : delta < 32000 ? 5 :
				     delta < 60000 ? 6 : 7;
			kmig.burst_hist_phase[bucket]++;
			page_info->alloc_ts_ms = 0;
		}

		old_hotness = page_info->hotness;
		old_node = page_info->node;
		if (old_node >= 0 && old_node < MAX_NODES)
			kmig.nr_samples_node[old_node] += nr;
		new_hotness = calc_hotness(old_hotness, nr, cur_age - page_info->age, HOTNESS_WEIGHT, true);

		/* A page in the quick-demotion queue that is accessed again is warm: keep it. */
		if (alloc_meta.pages_to_move_dict.count(pva.first)) {
			kmig.astat.nr_reaccessed_cold_pages++;
			kmig.astat.phase_reaccessed_cold++;
		}
		dequeue_page(alloc_meta, pva.first);

		page_info->hotness = new_hotness;
		page_info->age = cur_age;
		page_info->next_node = NO_MIG;
		new_node = page_info->node;
		if (page_info->bin_idx == -1) {
			old_hotness = UINT64_MAX;
			old_node = UNKNOWN_NODE;
		}
		update_hist((unsigned long)pva.first, old_hotness, new_hotness, old_node, new_node, page_info, kmig.hist);
	}

	kmig.pfstat.nr_max_pages = max(kmig.pfstat.nr_max_pages, (unsigned long long)kmig.g_page_map.size());
	return nr_accesses;
}

int profile_pages(int age)
{
	int ret = profile_pages_pebs(kmig.pebs_meta, age, kmig.alloc_meta);

	kmig.pebs_meta.profiled_va.clear();
	kmig.profiled_accesses += ret;
	return ret;
}

static unsigned long cooling_one_bin(struct hist_bin *bin, int cur_age)
{
	uint64_t old_hotness, new_hotness;
	int old_age, new_bin;
	struct page_profile *page_info;
	unsigned long va;
	unsigned long nr_cooled_pages = 0;

	for (auto it = bin->va_set.begin(); it != bin->va_set.end();) {
		page_info = it->second->second;
		va = (unsigned long)it->first;
		old_age = page_info->age;
		old_hotness = page_info->hotness;
		new_hotness = calc_hotness(old_hotness, 0, cur_age - old_age, HOTNESS_WEIGHT);
		new_bin = get_idx(new_hotness);

		if (kmig.hist + new_bin == bin) {
			page_info->age = cur_age;
			page_info->hotness = new_hotness;
			page_info->bin_idx = new_bin;
			it++;
			continue;
		}

		add_hist_bin_va(kmig.hist + new_bin, va, page_info, page_info->hist_node);
		page_info->age = cur_age;
		page_info->hotness = new_hotness;
		page_info->bin_idx = new_bin;

		bin->va_lists[page_info->hist_node].erase(it->second);
		it = bin->va_set.erase(it);
		bin->nr_deleted++;
		bin->nr_pages--;
		bin->nr_pages_tier[page_info->hist_node]--;
		nr_cooled_pages++;
	}
	return nr_cooled_pages;
}

/* Decay the hotness of every page in the histogram by one cooling round. */
unsigned long do_cooling(struct hist_bin *hist, int cur_age)
{
	unsigned long total = 0;
	auto start = high_resolution_clock::now();

	for (int i = 1; i < NR_HIST_BINS; i++)
		total += cooling_one_bin(hist + i, cur_age);

	auto duration = duration_cast<milliseconds>(high_resolution_clock::now() - start);
	mf_log(LOG_INFO, "[Cooling %ldms] %lu pages moved to colder bins\n", duration.count(), total);
	return total;
}
