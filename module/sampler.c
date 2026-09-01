// SPDX-License-Identifier: GPL-2.0
/*
 * kpebsd: PEBS-based memory access sampling for the target process.
 *
 * One perf event per (CPU, event type) is opened in kernel space and its
 * ring buffer is drained by a dedicated kthread. Each sample is queued for
 * ktrackd, which forwards it to userspace through the RB_PEBS ring buffer.
 * The kernel-side helpers kmig__perf_event_open(), kmig__perf_event_init()
 * and perf_event_get_period() are added by the MigFlow kernel patch.
 */
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/perf_event.h>
#include <linux/cpumask.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "perf_internal.h"
#include "tracker.h"
#include "pebs.h"

/* CPUs whose memory accesses are sampled */
static char sample_cpus[64] = "0-23";
module_param_string(sample_cpus, sample_cpus, sizeof(sample_cpus), 0444);
MODULE_PARM_DESC(sample_cpus, "CPU list sampled by kpebsd (default 0-23)");

/*
 * Target CPU share of kpebsd in permille. 0 keeps the initial sampling
 * period; otherwise the period is adjusted every 15 s to stay within the
 * quota (+/- 0.5%).
 */
static int sampler_cpu_quota;
module_param(sampler_cpu_quota, int, 0644);
MODULE_PARM_DESC(sampler_cpu_quota, "kpebsd CPU quota in permille (0 = fixed sampling period)");

/* perf ring buffer size per event, in pages */
#define PEBS_BUFFER_PAGES 32

/*
 * Candidate sampling periods (primes).
 * The sampler starts at index PEBS_INIT_PERIOD_IDX and, when a CPU quota is
 * configured, walks up or down this table to stay within the quota.
 */
#define PEBS_NR_PERIODS 35
static const unsigned int pebs_period_list[PEBS_NR_PERIODS] = {
	53, 101, 127, 149, 173, 199, 293, 401, 499, 599, 701, 797, 907, 997,
	1201, 1399, 1601, 1801, 1999, 2503, 3001, 3499, 4001, 4507, 4999,
	6007, 7001, 7993, 9001, 10007, 12007, 13999, 16001, 17989, 19997,
};
#define PEBS_INIT_PERIOD_IDX 10	/* 701 */

/* store sampling is much coarser than load sampling */
#define PEBS_NR_INST_PERIODS 5
static const unsigned int pebs_inst_period_list[PEBS_NR_INST_PERIODS] = {
	100003, 300007, 600011, 1000003, 1500003,
};

static unsigned long pebs_period_at(unsigned long idx)
{
	if (idx >= PEBS_NR_PERIODS)
		idx = PEBS_NR_PERIODS - 1;
	return pebs_period_list[idx];
}

static unsigned long pebs_inst_period_at(unsigned long idx)
{
	if (idx >= PEBS_NR_INST_PERIODS)
		idx = PEBS_NR_INST_PERIODS - 1;
	return pebs_inst_period_list[idx];
}

static const __u64 pebs_event_code[N_PEBS_EVENTS] = {
	[DRAMREAD] = DRAM_LLC_LOAD_MISS,
	[R_DRAMREAD] = REMOTE_DRAM_LLC_LOAD_MISS,
	[NVMREAD] = NVM_LLC_LOAD_MISS,
	[R_NVMREAD] = REMOTE_NVM_LLC_LOAD_MISS,
	[MEMWRITE] = ALL_STORES,
};

static struct cpumask sample_mask;
static struct task_struct *kpebsd;

/* per (CPU, event type): the perf event and the file that references it */
static struct perf_event **mem_event;
static struct file **mem_file;
#define EVENT_IDX(cpu, event) ((cpu) * N_PEBS_EVENTS + (event))

struct pebs_counters {
	unsigned long long nr_by_event[N_PEBS_EVENTS];
	unsigned long long nr_throttled;
	unsigned long long nr_lost;
	unsigned long long nr_unknown;
};

/* a non-null address in the user half of the address space */
static bool valid_va(unsigned long addr)
{
	return addr && !(addr >> (PGDIR_SHIFT + 9));
}

static int pebs_open_event(int cpu, enum pebs_event type, pid_t pid)
{
	struct perf_event_attr attr;
	struct file *file;
	int fd, ret;

	memset(&attr, 0, sizeof(attr));
	attr.type = PERF_TYPE_RAW;
	attr.size = sizeof(attr);
	attr.config = pebs_event_code[type];
	attr.sample_period = type == MEMWRITE ? pebs_inst_period_at(0)
					      : pebs_period_at(PEBS_INIT_PERIOD_IDX);
	attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR;
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;
	attr.exclude_callchain_kernel = 1;
	attr.exclude_callchain_user = 1;
	attr.precise_ip = 1;
	attr.inherit = 1;
	attr.enable_on_exec = 1;

	/* installs a descriptor in the calling process (the daemon) */
	fd = kmig__perf_event_open(&attr, pid, cpu, -1, 0);
	if (fd < 0) {
		pr_err("migflow: perf_event_open failed on cpu %d: %d\n", cpu, fd);
		return fd;
	}
	file = fget(fd);
	if (!file)
		return -EBADF;
	mem_file[EVENT_IDX(cpu, type)] = file;
	mem_event[EVENT_IDX(cpu, type)] = file->private_data;

	ret = kmig__perf_event_init(mem_event[EVENT_IDX(cpu, type)], PEBS_BUFFER_PAGES);
	if (ret)
		pr_err("migflow: perf ring buffer allocation failed: %d\n", ret);
	return ret;
}

static void pebs_release(void)
{
	int cpu, event;

	if (!mem_event)
		return;
	for_each_cpu(cpu, &sample_mask) {
		for (event = 0; event < N_PEBS_EVENTS; event++) {
			int i = EVENT_IDX(cpu, event);

			if (mem_event[i])
				perf_event_disable(mem_event[i]);
			if (mem_file[i])
				fput(mem_file[i]);
		}
	}
	kfree(mem_event);
	kfree(mem_file);
	mem_event = NULL;
	mem_file = NULL;
}

static int pebs_init(pid_t pid)
{
	int cpu, event, ret;

	mem_event = kcalloc(nr_cpu_ids * N_PEBS_EVENTS, sizeof(*mem_event), GFP_KERNEL);
	mem_file = kcalloc(nr_cpu_ids * N_PEBS_EVENTS, sizeof(*mem_file), GFP_KERNEL);
	if (!mem_event || !mem_file) {
		kfree(mem_event);
		kfree(mem_file);
		mem_event = NULL;
		mem_file = NULL;
		return -ENOMEM;
	}

	for_each_cpu(cpu, &sample_mask) {
		for (event = 0; event < N_PEBS_EVENTS; event++) {
			ret = pebs_open_event(cpu, event, pid);
			if (ret) {
				pebs_release();
				return ret;
			}
		}
	}
	return 0;
}

static void pebs_update_period(uint64_t load_period, uint64_t store_period)
{
	int cpu, event;

	for_each_cpu(cpu, &sample_mask) {
		for (event = 0; event < N_PEBS_EVENTS; event++) {
			struct perf_event *ev = mem_event[EVENT_IDX(cpu, event)];

			if (ev && perf_event_period(ev, event == MEMWRITE ? store_period : load_period))
				pr_warn("migflow: failed to update the sampling period\n");
		}
	}
}

/* Average sampling period over all CPUs, for loads and stores separately. */
void pebs_get_period(uint64_t *read_period, uint64_t *write_period)
{
	uint64_t sum_read = 0, sum_write = 0;
	int cpu, event, cnt_read = 0, cnt_write = 0;

	*read_period = *write_period = 0;
	if (!mem_event)
		return;
	for_each_cpu(cpu, &sample_mask) {
		for (event = 0; event < N_PEBS_EVENTS; event++) {
			struct perf_event *ev = mem_event[EVENT_IDX(cpu, event)];

			if (!ev)
				continue;
			if (event == MEMWRITE) {
				sum_write += perf_event_get_period(ev);
				cnt_write++;
			} else {
				sum_read += perf_event_get_period(ev);
				cnt_read++;
			}
		}
	}
	if (cnt_read)
		*read_period = sum_read / cnt_read;
	if (cnt_write)
		*write_period = sum_write / cnt_write;
}

/* Copy @len bytes starting at ring offset @pos; a record may span data pages. */
static void pebs_rb_copy(struct perf_buffer *rb, u64 pos, void *dst, size_t len)
{
	int shift = PAGE_SHIFT + page_order(rb);
	unsigned long page_size = 1UL << shift;

	while (len) {
		unsigned long pg = (pos >> shift) & (rb->nr_pages - 1);
		unsigned long off = pos & (page_size - 1);
		size_t n = min_t(size_t, len, page_size - off);

		memcpy(dst, (char *)rb->data_pages[pg] + off, n);
		dst = (char *)dst + n;
		pos += n;
		len -= n;
	}
}

/* Consume every pending record of one perf buffer; returns the number of samples queued. */
static unsigned long pebs_drain_event(struct perf_event *event, enum pebs_event type,
				      struct list_head *samples, struct pebs_counters *cnt)
{
	struct perf_buffer *rb = READ_ONCE(event->rb);
	struct perf_event_mmap_page *up;
	struct pebs_entry *entry;
	unsigned long nr = 0;
	u64 head, tail;

	if (!rb)
		return 0;
	up = rb->user_page;
	head = READ_ONCE(up->data_head);
	tail = up->data_tail;
	smp_rmb();	/* records are read after data_head */

	while (tail != head) {
		struct pebs_sample sample;

		pebs_rb_copy(rb, tail, &sample.header, sizeof(sample.header));
		if (sample.header.size < sizeof(sample.header)) {
			cnt->nr_unknown++;
			tail = head;	/* unreadable stream: skip to the end */
			break;
		}
		switch (sample.header.type) {
		case PERF_RECORD_SAMPLE:
			if (sample.header.size < sizeof(sample)) {
				cnt->nr_unknown++;
				break;
			}
			pebs_rb_copy(rb, tail + sizeof(sample.header),
				     (char *)&sample + sizeof(sample.header),
				     sizeof(sample) - sizeof(sample.header));
			if (!valid_va(sample.addr))
				break;
			entry = kmem_cache_alloc(g_tracker.pebs_slab, GFP_KERNEL);
			if (!entry)
				break;
			entry->va = sample.addr;
			entry->type = type;
			entry->pid = (int)sample.pid;
			list_add_tail(&entry->list, samples);
			nr++;
			break;
		case PERF_RECORD_THROTTLE:
		case PERF_RECORD_UNTHROTTLE:
			cnt->nr_throttled++;
			break;
		case PERF_RECORD_LOST:
		case PERF_RECORD_LOST_SAMPLES:
			cnt->nr_lost++;
			break;
		default:
			cnt->nr_unknown++;
			break;
		}
		tail += sample.header.size;
	}

	smp_mb();	/* finish reading before handing the space back */
	WRITE_ONCE(up->data_tail, tail);
	return nr;
}

static int kpebsd_fn(void *data)
{
	struct task_struct *t = current;
	struct pebs_counters cnt = { { 0 } };
	unsigned long sleep_timeout = usecs_to_jiffies(2000);
	unsigned long quota_period = msecs_to_jiffies(15000);
	unsigned long period_idx = PEBS_INIT_PERIOD_IDX, inst_period_idx = 0;
	unsigned long last_quota_check = jiffies, total_start = jiffies;
	unsigned long long nr_sampled = 0;
	u64 last_runtime = t->se.sum_exec_runtime, start_runtime = last_runtime, cputime = 0;
	LIST_HEAD(new_samples);
	int cpu, event;

	pebs_update_period(pebs_period_at(period_idx), pebs_inst_period_at(inst_period_idx));

	while (!kthread_should_stop()) {
		for_each_cpu(cpu, &sample_mask) {
			for (event = 0; event < N_PEBS_EVENTS; event++) {
				struct perf_event *ev = mem_event[EVENT_IDX(cpu, event)];

				if (ev)
					cnt.nr_by_event[event] += pebs_drain_event(ev, event, &new_samples, &cnt);
			}
		}

		mutex_lock(&g_tracker.pebs_mutex);
		list_splice_tail_init(&new_samples, &g_tracker.pebs_list);
		mutex_unlock(&g_tracker.pebs_mutex);

		schedule_timeout_interruptible(sleep_timeout);

		if (!sampler_cpu_quota)
			continue;

		/* Adjust the sampling period to the CPU quota. */
		if (jiffies - last_quota_check >= quota_period) {
			u64 runtime = t->se.sum_exec_runtime;
			u64 elapsed_us = jiffies_to_usecs(jiffies - last_quota_check);
			u64 used = div64_u64(runtime - last_runtime, elapsed_us);

			cputime = cputime ? ((used << 3) + (cputime << 1)) / 10 : used;

			if (cputime > sampler_cpu_quota + 5 && period_idx + 1 < PEBS_NR_PERIODS) {
				period_idx++;
				if (inst_period_idx + 1 < PEBS_NR_INST_PERIODS)
					inst_period_idx++;
				pebs_update_period(pebs_period_at(period_idx), pebs_inst_period_at(inst_period_idx));
			} else if (cputime < sampler_cpu_quota - 5 && period_idx > 0) {
				period_idx--;
				if (inst_period_idx > 0)
					inst_period_idx--;
				pebs_update_period(pebs_period_at(period_idx), pebs_inst_period_at(inst_period_idx));
			}
			last_quota_check = jiffies;
			last_runtime = runtime;
		}
	}

	for (event = 0; event < N_PEBS_EVENTS; event++)
		nr_sampled += cnt.nr_by_event[event];
	pr_info("migflow: kpebsd sampled %llu (dram %llu, rdram %llu, nvm %llu, rnvm %llu, store %llu), throttled %llu, lost %llu, unknown %llu\n",
		nr_sampled, cnt.nr_by_event[DRAMREAD], cnt.nr_by_event[R_DRAMREAD],
		cnt.nr_by_event[NVMREAD], cnt.nr_by_event[R_NVMREAD], cnt.nr_by_event[MEMWRITE],
		cnt.nr_throttled, cnt.nr_lost, cnt.nr_unknown);
	pr_info("migflow: kpebsd cpu time %llu ms over %u ms\n",
		div64_u64(t->se.sum_exec_runtime - start_runtime, 1000000),
		jiffies_to_msecs(jiffies - total_start));
	return 0;
}

int kpebsd_init(pid_t pid)
{
	const struct cpumask *node_mask;
	int ret;

	if (kpebsd)
		return -EBUSY;
	if (cpulist_parse(sample_cpus, &sample_mask) || cpumask_empty(&sample_mask) ||
	    !cpumask_subset(&sample_mask, cpu_online_mask)) {
		pr_err("migflow: invalid sample_cpus \"%s\"\n", sample_cpus);
		return -EINVAL;
	}

	ret = pebs_init(pid);
	if (ret)
		return ret;

	kpebsd = kthread_create(kpebsd_fn, NULL, "kpebsd");
	if (IS_ERR(kpebsd)) {
		ret = PTR_ERR(kpebsd);
		kpebsd = NULL;
		pebs_release();
		return ret;
	}
	/* run on the socket that is sampled */
	node_mask = cpumask_of_node(cpu_to_node(cpumask_first(&sample_mask)));
	if (!cpumask_empty(node_mask))
		kthread_bind_mask(kpebsd, node_mask);
	wake_up_process(kpebsd);
	return 0;
}

void kpebsd_exit(void)
{
	if (kpebsd) {
		kthread_stop(kpebsd);
		kpebsd = NULL;
	}
	pebs_release();
}
