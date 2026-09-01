// SPDX-License-Identifier: GPL-2.0
/*
 * MigFlow kernel module: allocation tracking (ktrackd) and PEBS sampling
 * (kpebsd, see sampler.c) for the userspace migration daemon.
 *
 * ktrackd implements the 2Q mechanism of the allocation zone. Every page
 * faulted in by the target process (anonymous or file-backed) enters the
 * burst queue. After the burst period its access bit is cleared and it moves
 * to the grace queue. After the grace period the access bit is checked again
 * and the page is handed to userspace through the RB_ALLOC ring buffer
 * together with its NUMA node and whether it was accessed during the grace
 * period.
 *
 * Both periods are adapted at run time from the burst-access pressure, the
 * number of sampled memory accesses per page allocation (see adapt_periods()).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/page_idle.h>
#include <linux/pagewalk.h>
#include <linux/hugetlb.h>
#include <linux/mmu_notifier.h>
#include <linux/poll.h>
#include <linux/io.h>

#include "tracker.h"
#include "pebs.h"

#ifndef CONFIG_X86_64
#error "the page-fault hook reads the first argument from pt_regs::di (x86-64 only)"
#endif

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MigFlow allocation tracker and PEBS sampler");

static int burst_msec = 15000;
module_param(burst_msec, int, 0644);
MODULE_PARM_DESC(burst_msec, "base residence time in the burst queue (ms)");

static int grace_msec = 2000;
module_param(grace_msec, int, 0644);
MODULE_PARM_DESC(grace_msec, "base residence time in the grace queue (ms)");

static int adaptive = 1;
module_param(adaptive, int, 0644);
MODULE_PARM_DESC(adaptive, "extend both periods with the burst-access pressure (1) or keep them fixed (0)");

static int burst_extra_max_msec = 15000;
module_param(burst_extra_max_msec, int, 0644);
MODULE_PARM_DESC(burst_extra_max_msec, "upper bound of the pressure-driven extension of the burst period (ms)");

static int grace_extra_max_msec = 3000;
module_param(grace_extra_max_msec, int, 0644);
MODULE_PARM_DESC(grace_extra_max_msec, "upper bound of the pressure-driven extension of the grace period (ms)");

static int burst_scale = 20000;
module_param(burst_scale, int, 0644);
MODULE_PARM_DESC(burst_scale, "burst period extension in ms per 1000 sampled accesses per allocation");

static int grace_scale = 27000;
module_param(grace_scale, int, 0644);
MODULE_PARM_DESC(grace_scale, "grace period extension in ms per 1000 sampled accesses per allocation");

static int pressure_window_sec = 20;
module_param(pressure_window_sec, int, 0644);
MODULE_PARM_DESC(pressure_window_sec, "length of the window over which the burst-access pressure is measured (s)");

#define DEVICE_NAME "migflow"
#define CLASS_NAME  "migflow"

static dev_t dev_num;
static struct class *migflow_class;
static struct cdev migflow_cdev;

struct migflow_tracker g_tracker;

static unsigned long alloc_cnt;	/* allocations in the current pressure window */

static inline bool tracker_is_target(int tgid)
{
	return tgid == READ_ONCE(g_tracker.target_pid);
}

/* ------------------------------------------------------------------------
 * Access-bit helpers. The page-table walkers below follow mm/damon/vaddr.c
 * and mm/page_idle.c: clear the young bit of one virtual page (mkold) and
 * test whether it has been set again since (young).
 * ---------------------------------------------------------------------- */

static struct folio *tracker_get_folio(unsigned long pfn)
{
	struct page *page = pfn_to_online_page(pfn);
	struct folio *folio;

	if (!page || PageTail(page))
		return NULL;

	folio = page_folio(page);
	if (!folio_test_lru(folio) || !folio_try_get(folio))
		return NULL;
	if (unlikely(page_folio(page) != folio || !folio_test_lru(folio))) {
		folio_put(folio);
		folio = NULL;
	}
	return folio;
}

static void tracker_ptep_mkold(pte_t *pte, struct vm_area_struct *vma, unsigned long addr)
{
	struct folio *folio = tracker_get_folio(pte_pfn(ptep_get(pte)));

	if (!folio)
		return;

	if (ptep_clear_young_notify(vma, addr, pte))
		folio_set_young(folio);

	folio_set_idle(folio);
	folio_put(folio);
}

static void tracker_pmdp_mkold(pmd_t *pmd, struct vm_area_struct *vma, unsigned long addr)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	struct folio *folio = tracker_get_folio(pmd_pfn(pmdp_get(pmd)));

	if (!folio)
		return;

	if (pmdp_clear_young_notify(vma, addr, pmd))
		folio_set_young(folio);

	folio_set_idle(folio);
	folio_put(folio);
#endif
}

static struct mm_struct *tracker_get_mm(int pid)
{
	struct pid *p = find_get_pid(pid);
	struct task_struct *task;
	struct mm_struct *mm;

	if (!p)
		return NULL;
	task = get_pid_task(p, PIDTYPE_PID);
	put_pid(p);
	if (!task)
		return NULL;

	mm = get_task_mm(task);
	put_task_struct(task);
	return mm;
}

static int tracker_mkold_pmd_entry(pmd_t *pmd, unsigned long addr,
				   unsigned long next, struct mm_walk *walk)
{
	pte_t *pte;
	pmd_t pmde;
	spinlock_t *ptl;

	if (pmd_trans_huge(pmdp_get(pmd))) {
		ptl = pmd_lock(walk->mm, pmd);
		pmde = pmdp_get(pmd);

		if (!pmd_present(pmde)) {
			spin_unlock(ptl);
			return 0;
		}

		if (pmd_trans_huge(pmde)) {
			tracker_pmdp_mkold(pmd, walk->vma, addr);
			spin_unlock(ptl);
			return 0;
		}
		spin_unlock(ptl);
	}

	pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	if (!pte) {
		walk->action = ACTION_AGAIN;
		return 0;
	}
	if (!pte_present(ptep_get(pte)))
		goto out;
	tracker_ptep_mkold(pte, walk->vma, addr);
out:
	pte_unmap_unlock(pte, ptl);
	return 0;
}

#ifdef CONFIG_HUGETLB_PAGE
static void tracker_hugetlb_mkold(pte_t *pte, struct mm_struct *mm,
				  struct vm_area_struct *vma, unsigned long addr)
{
	bool referenced = false;
	pte_t entry = huge_ptep_get(pte);
	struct folio *folio = pfn_folio(pte_pfn(entry));
	unsigned long psize = huge_page_size(hstate_vma(vma));

	folio_get(folio);

	if (pte_young(entry)) {
		referenced = true;
		entry = pte_mkold(entry);
		set_huge_pte_at(mm, addr, pte, entry, psize);
	}

#ifdef CONFIG_MMU_NOTIFIER
	if (mmu_notifier_clear_young(mm, addr, addr + psize))
		referenced = true;
#endif

	if (referenced)
		folio_set_young(folio);

	folio_set_idle(folio);
	folio_put(folio);
}

static int tracker_mkold_hugetlb_entry(pte_t *pte, unsigned long hmask,
				       unsigned long addr, unsigned long end,
				       struct mm_walk *walk)
{
	struct hstate *h = hstate_vma(walk->vma);
	spinlock_t *ptl;
	pte_t entry;

	ptl = huge_pte_lock(h, walk->mm, pte);
	entry = huge_ptep_get(pte);
	if (!pte_present(entry))
		goto out;

	tracker_hugetlb_mkold(pte, walk->mm, walk->vma, addr);

out:
	spin_unlock(ptl);
	return 0;
}
#else
#define tracker_mkold_hugetlb_entry NULL
#endif

static const struct mm_walk_ops tracker_mkold_ops = {
	.pmd_entry = tracker_mkold_pmd_entry,
	.hugetlb_entry = tracker_mkold_hugetlb_entry,
	.walk_lock = PGWALK_RDLOCK,
};

static void tracker_va_mkold(struct mm_struct *mm, unsigned long addr)
{
	mmap_read_lock(mm);
	walk_page_range(mm, addr, addr + 1, &tracker_mkold_ops, NULL);
	mmap_read_unlock(mm);
}

static int tracker_young_pmd_entry(pmd_t *pmd, unsigned long addr,
				   unsigned long next, struct mm_walk *walk)
{
	pte_t *pte;
	pte_t ptent;
	spinlock_t *ptl;
	struct folio *folio;
	bool *young = walk->private;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (pmd_trans_huge(pmdp_get(pmd))) {
		pmd_t pmde;

		ptl = pmd_lock(walk->mm, pmd);
		pmde = pmdp_get(pmd);

		if (!pmd_present(pmde)) {
			spin_unlock(ptl);
			return 0;
		}

		if (!pmd_trans_huge(pmde)) {
			spin_unlock(ptl);
			goto regular_page;
		}
		folio = tracker_get_folio(pmd_pfn(pmde));
		if (!folio)
			goto huge_out;
		if (pmd_young(pmde) || !folio_test_idle(folio) ||
		    mmu_notifier_test_young(walk->mm, addr))
			*young = true;
		folio_put(folio);
huge_out:
		spin_unlock(ptl);
		return 0;
	}

regular_page:
#endif
	pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	if (!pte) {
		walk->action = ACTION_AGAIN;
		return 0;
	}
	ptent = ptep_get(pte);
	if (!pte_present(ptent))
		goto out;
	folio = tracker_get_folio(pte_pfn(ptent));
	if (!folio)
		goto out;
	if (pte_young(ptent) || !folio_test_idle(folio) ||
	    mmu_notifier_test_young(walk->mm, addr))
		*young = true;
	folio_put(folio);
out:
	pte_unmap_unlock(pte, ptl);
	return 0;
}

#ifdef CONFIG_HUGETLB_PAGE
static int tracker_young_hugetlb_entry(pte_t *pte, unsigned long hmask,
				       unsigned long addr, unsigned long end,
				       struct mm_walk *walk)
{
	bool *young = walk->private;
	struct hstate *h = hstate_vma(walk->vma);
	struct folio *folio;
	spinlock_t *ptl;
	pte_t entry;

	ptl = huge_pte_lock(h, walk->mm, pte);
	entry = huge_ptep_get(pte);
	if (!pte_present(entry))
		goto out;

	folio = pfn_folio(pte_pfn(entry));
	folio_get(folio);

	if (pte_young(entry) || !folio_test_idle(folio) ||
	    mmu_notifier_test_young(walk->mm, addr))
		*young = true;

	folio_put(folio);

out:
	spin_unlock(ptl);
	return 0;
}
#else
#define tracker_young_hugetlb_entry NULL
#endif

static const struct mm_walk_ops tracker_young_ops = {
	.pmd_entry = tracker_young_pmd_entry,
	.hugetlb_entry = tracker_young_hugetlb_entry,
	.walk_lock = PGWALK_RDLOCK,
};

static bool tracker_va_young(struct mm_struct *mm, unsigned long addr)
{
	bool young = false;

	mmap_read_lock(mm);
	walk_page_range(mm, addr, addr + 1, &tracker_young_ops, &young);
	mmap_read_unlock(mm);
	return young;
}

/* NUMA node of the page mapped at @addr, or a negative errno (mm/migrate.c) */
static int tracker_va_node(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *vma;
	struct page *page;
	int err = -EFAULT;

	mmap_read_lock(mm);
	vma = vma_lookup(mm, addr);
	if (!vma)
		goto out;

	page = follow_page(vma, addr, FOLL_GET | FOLL_DUMP);
	err = PTR_ERR(page);
	if (IS_ERR(page))
		goto out;

	err = -ENOENT;
	if (!page)
		goto out;

	if (!is_zone_device_page(page))
		err = page_to_nid(page);
	put_page(page);
out:
	mmap_read_unlock(mm);
	return err;
}

/* ------------------------------------------------------------------------
 * Queues
 * ---------------------------------------------------------------------- */

static void init_alloc_list(struct alloc_list *list)
{
	INIT_LIST_HEAD(&list->head);
	list->length = 0;
}

static void add_alloc_entry(struct alloc_list *list, struct alloc_entry *entry)
{
	list_add_tail(&entry->list, &list->head);
	list->length++;
}

static void move_alloc_entry(struct alloc_list *from, struct alloc_list *to,
			     struct alloc_entry *entry)
{
	list_move_tail(&entry->list, &to->head);
	from->length--;
	to->length++;
}

static void remove_alloc_entry(struct alloc_list *list, struct alloc_entry *entry)
{
	list_del(&entry->list);
	list->length--;
}

static void splice_alloc_list(struct alloc_list *from, struct alloc_list *to)
{
	list_splice_tail_init(&from->head, &to->head);
	to->length += from->length;
	from->length = 0;
}

/* Free every entry of @list; the caller holds alloc_lock. */
static int free_alloc_list(struct alloc_list *list)
{
	struct alloc_entry *entry, *tmp;
	int len = 0;

	list_for_each_entry_safe(entry, tmp, &list->head, list) {
		remove_alloc_entry(list, entry);
		kmem_cache_free(g_tracker.alloc_slab, entry);
		len++;
	}
	return len;
}

static int free_pebs_list(struct list_head *list)
{
	struct pebs_entry *sample, *tmp;
	int len = 0;

	list_for_each_entry_safe(sample, tmp, list, list) {
		list_del(&sample->list);
		kmem_cache_free(g_tracker.pebs_slab, sample);
		len++;
	}
	return len;
}

/* ------------------------------------------------------------------------
 * Ring buffers shared with userspace
 * ---------------------------------------------------------------------- */

static int init_tracker(void)
{
	int i;
	int thresholds[MAX_NR_RB] = { RB_THRESHOLD_ALLOC, RB_THRESHOLD_PEBS };

	if (burst_msec <= 0 || grace_msec <= 0 || burst_extra_max_msec < 0 ||
	    grace_extra_max_msec < 0 || burst_scale < 0 || grace_scale < 0 ||
	    pressure_window_sec <= 0 || pressure_window_sec > INT_MAX / HZ) {
		pr_err("migflow: invalid module parameters\n");
		return -EINVAL;
	}

	memset(&g_tracker, 0, sizeof(g_tracker));
	g_tracker.burst_msec = burst_msec;
	g_tracker.grace_msec = grace_msec;
	g_tracker.target_pid = -1;
	init_alloc_list(&g_tracker.burst_queue);
	init_alloc_list(&g_tracker.grace_queue);
	INIT_LIST_HEAD(&g_tracker.pebs_list);
	init_waitqueue_head(&g_tracker.poll_wq);
	spin_lock_init(&g_tracker.alloc_lock);
	spin_lock_init(&g_tracker.pebs_lock);
	mutex_init(&g_tracker.pebs_mutex);

	g_tracker.alloc_slab = kmem_cache_create("migflow_alloc", sizeof(struct alloc_entry), 0,
						  SLAB_HWCACHE_ALIGN, NULL);
	g_tracker.pebs_slab = kmem_cache_create("migflow_pebs", sizeof(struct pebs_entry), 0,
						 SLAB_HWCACHE_ALIGN, NULL);
	if (!g_tracker.alloc_slab || !g_tracker.pebs_slab)
		goto fail;

	for (i = 0; i < MAX_NR_RB; i++) {
		struct rb_head_t *rb = kmalloc(RB_HEADER_SIZE + RB_BUF_SIZE, GFP_KERNEL);

		if (!rb) {
			pr_err("migflow: failed to allocate ring buffer %d\n", i);
			goto fail;
		}
		rb->head = rb->tail = 0;
		rb->size = RB_BUF_SIZE / sizeof(struct rb_data_t);
		g_tracker.rb[i] = rb;
		g_tracker.rb_buf[i] = (struct rb_data_t *)((char *)rb + RB_HEADER_SIZE);
		g_tracker.threshold_wakeup[i] = thresholds[i];
	}

	pr_info("migflow: burst %d ms, grace %d ms (%s)\n", burst_msec, grace_msec,
		adaptive ? "adaptive" : "fixed");
	return 0;

fail:
	for (i = 0; i < MAX_NR_RB; i++)
		kfree(g_tracker.rb[i]);
	kmem_cache_destroy(g_tracker.alloc_slab);
	kmem_cache_destroy(g_tracker.pebs_slab);
	return -ENOMEM;
}

static void destroy_tracker(void)
{
	int i;

	spin_lock(&g_tracker.alloc_lock);
	pr_info("migflow: freed %d burst-queue entries\n", free_alloc_list(&g_tracker.burst_queue));
	pr_info("migflow: freed %d grace-queue entries\n", free_alloc_list(&g_tracker.grace_queue));
	spin_unlock(&g_tracker.alloc_lock);
	free_pebs_list(&g_tracker.pebs_list);

	for (i = 0; i < MAX_NR_RB; i++)
		kfree(g_tracker.rb[i]);
	kmem_cache_destroy(g_tracker.alloc_slab);
	kmem_cache_destroy(g_tracker.pebs_slab);
}

/* Both publish functions run under the lock of their ring buffer. */
static bool rb_publish_alloc(struct alloc_entry *entry)
{
	struct rb_head_t *rb = g_tracker.rb[RB_ALLOC];
	struct rb_data_alloc_t *rec;

	if (RB_FULL(rb))
		return false;

	rec = &g_tracker.rb_buf[RB_ALLOC][rb->head].data.rb_alloc;
	rec->va = entry->va;
	rec->node = entry->node;
	rec->last_accessed = entry->last_accessed;
	rec->pid = entry->pid;
	smp_store_release(&rb->head, (rb->head + 1) % rb->size);
	return true;
}

static bool rb_publish_pebs(struct pebs_entry *sample)
{
	struct rb_head_t *rb = g_tracker.rb[RB_PEBS];
	struct rb_data_pebs_t *rec;

	if (RB_FULL(rb))
		return false;

	rec = &g_tracker.rb_buf[RB_PEBS][rb->head].data.rb_pebs;
	rec->va = sample->va & PAGE_MASK;
	rec->type = sample->type;
	rec->pid = sample->pid;
	smp_store_release(&rb->head, (rb->head + 1) % rb->size);
	return true;
}

static bool need_to_wakeup(int type)
{
	return RB_LEN(g_tracker.rb[type]) > g_tracker.threshold_wakeup[type];
}

/* ------------------------------------------------------------------------
 * Allocation hook: every page fault of the target enters the burst queue.
 * The kprobe handler runs in the faulting task with interrupts disabled.
 * ---------------------------------------------------------------------- */

static int page_fault_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct task_struct *task = current;
	struct vm_fault *vmf;
	struct alloc_entry *entry;
	unsigned long flags;

	if (!tracker_is_target(task->tgid))
		return 0;

	vmf = (struct vm_fault *)regs->di;
	entry = kmem_cache_alloc(g_tracker.alloc_slab, GFP_ATOMIC | __GFP_NOWARN);
	if (!entry)
		return 0;

	entry->va = vmf->address;
	entry->last_accessed = false;
	entry->checked = false;
	entry->node = -1;
	entry->ts_jiffies = jiffies;
	entry->pid = task->tgid;

	spin_lock_irqsave(&g_tracker.alloc_lock, flags);
	add_alloc_entry(&g_tracker.burst_queue, entry);
	alloc_cnt++;
	g_tracker.stat.nr_alloc++;
	spin_unlock_irqrestore(&g_tracker.alloc_lock, flags);
	return 0;
}

static struct kprobe kp_do_anonymous_page = {
	.symbol_name = "do_anonymous_page",
	.pre_handler = page_fault_handler,
};

static struct kprobe kp_do_fault = {
	.symbol_name = "do_fault",
	.pre_handler = page_fault_handler,
};

/* ------------------------------------------------------------------------
 * Burst-access pressure
 *
 * pressure = (PEBS samples * load sampling period) / page allocations, i.e.
 * the estimated number of memory accesses per allocated page, measured over
 * pressure_window_sec (store samples are counted like load samples). Each
 * period is its base value plus pressure * scale / 1000 ms, bounded by
 * *_extra_max_msec. A window without any allocation uses the bounds.
 * ---------------------------------------------------------------------- */

static unsigned long long window_samples;
static unsigned long window_start;

static void adapt_periods(int nr_new_samples)
{
	unsigned long allocs;
	uint64_t read_period = 0, write_period = 0;
	uint64_t pressure = 0, extra_burst, extra_grace;

	if (!adaptive) {
		g_tracker.burst_msec = burst_msec;
		g_tracker.grace_msec = grace_msec;
		return;
	}
	window_samples += nr_new_samples;
	if (!window_start) {
		window_start = jiffies;
		return;
	}
	if (time_before(jiffies, window_start + pressure_window_sec * HZ))
		return;

	spin_lock(&g_tracker.alloc_lock);
	allocs = alloc_cnt;
	alloc_cnt = 0;
	spin_unlock(&g_tracker.alloc_lock);

	pebs_get_period(&read_period, &write_period);
	if (!read_period)
		read_period = 1;

	if (allocs == 0) {
		extra_burst = burst_extra_max_msec;
		extra_grace = grace_extra_max_msec;
	} else {
		pressure = window_samples * read_period / allocs;
		extra_burst = min_t(uint64_t, pressure * burst_scale / 1000, burst_extra_max_msec);
		extra_grace = min_t(uint64_t, pressure * grace_scale / 1000, grace_extra_max_msec);
	}
	g_tracker.burst_msec = burst_msec + (int)extra_burst;
	g_tracker.grace_msec = grace_msec + (int)extra_grace;

	pr_info("migflow: pressure %llu (allocs %lu, samples %llu, period %llu) -> burst %d ms, grace %d ms\n",
		(unsigned long long)pressure, allocs, window_samples,
		(unsigned long long)read_period, g_tracker.burst_msec, g_tracker.grace_msec);

	window_samples = 0;
	window_start = jiffies;
}

/* ------------------------------------------------------------------------
 * ktrackd: drives the two queues and forwards records to userspace.
 * ---------------------------------------------------------------------- */

static int ktrackd_fn(void *data)
{
	unsigned long sleep_timeout = usecs_to_jiffies(1000);
	/* grace-period checks per tick: what the ring buffer can take */
	int max_checked = g_tracker.rb[RB_ALLOC]->size - 1;
	struct alloc_list burst_done, grace_done;
	struct list_head pebs_done;
	struct alloc_entry *entry, *tmp;
	struct pebs_entry *sample, *stmp;
	struct mm_struct *mm;
	unsigned long now;
	int nr_checked;

	init_alloc_list(&burst_done);
	init_alloc_list(&grace_done);
	INIT_LIST_HEAD(&pebs_done);

	pr_info("migflow: ktrackd started\n");

	while (!kthread_should_stop()) {
		int nr_samples = 0;

		if (READ_ONCE(g_tracker.target_pid) <= 0) {
			/* no target: drop whatever is left of the previous one */
			spin_lock(&g_tracker.alloc_lock);
			free_alloc_list(&g_tracker.burst_queue);
			free_alloc_list(&g_tracker.grace_queue);
			free_alloc_list(&burst_done);
			free_alloc_list(&grace_done);
			spin_unlock(&g_tracker.alloc_lock);
			mutex_lock(&g_tracker.pebs_mutex);
			free_pebs_list(&g_tracker.pebs_list);
			mutex_unlock(&g_tracker.pebs_mutex);
			free_pebs_list(&pebs_done);
			schedule_timeout_interruptible(sleep_timeout);
			continue;
		}

		now = jiffies;

		/* 1. Expire entries whose period has elapsed. */
		spin_lock(&g_tracker.alloc_lock);
		while (!list_empty(&g_tracker.burst_queue.head)) {
			entry = list_first_entry(&g_tracker.burst_queue.head, struct alloc_entry, list);
			if (time_before(now, entry->ts_jiffies + msecs_to_jiffies(g_tracker.burst_msec)))
				break;
			entry->ts_jiffies = now;
			move_alloc_entry(&g_tracker.burst_queue, &burst_done, entry);
		}
		while (!list_empty(&g_tracker.grace_queue.head)) {
			entry = list_first_entry(&g_tracker.grace_queue.head, struct alloc_entry, list);
			if (time_before(now, entry->ts_jiffies + msecs_to_jiffies(g_tracker.grace_msec)))
				break;
			move_alloc_entry(&g_tracker.grace_queue, &grace_done, entry);
		}
		spin_unlock(&g_tracker.alloc_lock);

		mm = tracker_get_mm(g_tracker.target_pid);
		if (mm) {
			/* 2. Burst period over: clear the access bit. */
			list_for_each_entry(entry, &burst_done.head, list)
				tracker_va_mkold(mm, entry->va);

			/* 3. Grace period over: test the access bit and look up the node. */
			nr_checked = 0;
			list_for_each_entry(entry, &grace_done.head, list) {
				if (entry->checked)
					continue;
				if (nr_checked >= max_checked)
					break;
				entry->last_accessed = tracker_va_young(mm, entry->va);
				entry->node = tracker_va_node(mm, entry->va);
				entry->checked = true;
				nr_checked++;
				if (entry->last_accessed)
					g_tracker.stat.nr_accessed++;
			}
			mmput(mm);
		}

		/*
		 * 4. Move burst-done entries to the grace queue and publish the
		 * checked grace-done entries. Entries are checked in arrival
		 * order, so the unchecked ones form the tail of the list.
		 */
		spin_lock(&g_tracker.alloc_lock);
		splice_alloc_list(&burst_done, &g_tracker.grace_queue);
		list_for_each_entry_safe(entry, tmp, &grace_done.head, list) {
			if (!entry->checked || !rb_publish_alloc(entry))
				break;
			if (entry->last_accessed)
				g_tracker.stat.nr_copied_accessed++;
			remove_alloc_entry(&grace_done, entry);
			kmem_cache_free(g_tracker.alloc_slab, entry);
		}
		if (need_to_wakeup(RB_ALLOC))
			wake_up_interruptible(&g_tracker.poll_wq);
		spin_unlock(&g_tracker.alloc_lock);

		/* 5. Forward PEBS samples queued by kpebsd. */
		mutex_lock(&g_tracker.pebs_mutex);
		list_splice_tail_init(&g_tracker.pebs_list, &pebs_done);
		mutex_unlock(&g_tracker.pebs_mutex);

		spin_lock(&g_tracker.pebs_lock);
		list_for_each_entry_safe(sample, stmp, &pebs_done, list) {
			if (!rb_publish_pebs(sample))
				break;
			nr_samples++;
			list_del(&sample->list);
			kmem_cache_free(g_tracker.pebs_slab, sample);
		}
		if (need_to_wakeup(RB_PEBS))
			wake_up_interruptible(&g_tracker.poll_wq);
		spin_unlock(&g_tracker.pebs_lock);

		adapt_periods(nr_samples);

		g_tracker.stat.max_rb_size = max(g_tracker.stat.max_rb_size, RB_LEN(g_tracker.rb[RB_ALLOC]));
		schedule_timeout_interruptible(sleep_timeout);
	}

	pr_info("migflow: ktrackd stopping (burst %d, grace %d, pending %d/%d)\n",
		g_tracker.burst_queue.length, g_tracker.grace_queue.length,
		burst_done.length, grace_done.length);
	spin_lock(&g_tracker.alloc_lock);
	free_alloc_list(&burst_done);
	free_alloc_list(&grace_done);
	spin_unlock(&g_tracker.alloc_lock);
	free_pebs_list(&pebs_done);
	return 0;
}

/* ------------------------------------------------------------------------
 * Character device
 * ---------------------------------------------------------------------- */

static long migflow_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case IOCTL_SET_PID: {
		int pid, ret;

		if (copy_from_user(&pid, (int __user *)arg, sizeof(pid)))
			return -EFAULT;
		if (pid <= 0)
			return -EINVAL;
		if (READ_ONCE(g_tracker.target_pid) > 0)
			return -EBUSY;

		WRITE_ONCE(g_tracker.target_pid, pid);
		ret = kpebsd_init(pid);
		if (ret < 0) {
			WRITE_ONCE(g_tracker.target_pid, -1);
			pr_err("migflow: failed to start kpebsd: %d\n", ret);
			return ret;
		}
		pr_info("migflow: tracking pid %d\n", pid);
		return 0;
	}
	case IOCTL_MOVE_RB_TAIL: {
		struct rb_reply_t reply;

		if (copy_from_user(&reply, (void __user *)arg, sizeof(reply)))
			return -EFAULT;
		if (reply.type < 0 || reply.type >= MAX_NR_RB || reply.nr_items < 0)
			return -EINVAL;
		if (reply.type == RB_ALLOC) {
			spin_lock(&g_tracker.alloc_lock);
			if (reply.nr_items > RB_LEN(g_tracker.rb[RB_ALLOC]))
				reply.nr_items = RB_LEN(g_tracker.rb[RB_ALLOC]);
			RB_DEL(g_tracker.rb[RB_ALLOC], reply.nr_items);
			g_tracker.stat.nr_copied += reply.nr_items;
			spin_unlock(&g_tracker.alloc_lock);
		} else {
			spin_lock(&g_tracker.pebs_lock);
			if (reply.nr_items > RB_LEN(g_tracker.rb[RB_PEBS]))
				reply.nr_items = RB_LEN(g_tracker.rb[RB_PEBS]);
			RB_DEL(g_tracker.rb[RB_PEBS], reply.nr_items);
			spin_unlock(&g_tracker.pebs_lock);
		}
		return 0;
	}
	case IOCTL_GET_RB_STATUS: {
		int flag = 0;

		spin_lock(&g_tracker.alloc_lock);
		if (need_to_wakeup(RB_ALLOC))
			flag |= RB_TYPE_TO_FLAG(RB_ALLOC);
		spin_unlock(&g_tracker.alloc_lock);

		spin_lock(&g_tracker.pebs_lock);
		if (need_to_wakeup(RB_PEBS))
			flag |= RB_TYPE_TO_FLAG(RB_PEBS);
		spin_unlock(&g_tracker.pebs_lock);

		if (copy_to_user((int __user *)arg, &flag, sizeof(flag)))
			return -EFAULT;
		return 0;
	}
	case IOCTL_GET_PERIOD: {
		struct pebs_period period;

		pebs_get_period(&period.read, &period.write);
		if (copy_to_user((struct pebs_period __user *)arg, &period, sizeof(period)))
			return -EFAULT;
		return 0;
	}
	default:
		return -EINVAL;
	}
}

/* Each mmap() hands out the next ring buffer: first RB_ALLOC, then RB_PEBS. */
static int migflow_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;

	if (len != RB_BUF_SIZE + RB_HEADER_SIZE || vma->vm_pgoff)
		return -EINVAL;
	if (vma->vm_flags & VM_WRITE)
		return -EPERM;
	if (g_tracker.cur_rb_idx >= MAX_NR_RB)
		return -EINVAL;

	return remap_pfn_range(vma, vma->vm_start,
			       virt_to_phys(g_tracker.rb[g_tracker.cur_rb_idx++]) >> PAGE_SHIFT,
			       len, vma->vm_page_prot);
}

static __poll_t migflow_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &g_tracker.poll_wq, wait);
	if (need_to_wakeup(RB_ALLOC) || need_to_wakeup(RB_PEBS))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static int migflow_open(struct inode *inode, struct file *file)
{
	g_tracker.cur_rb_idx = 0;
	return 0;
}

/* The daemon closed the device: stop sampling; ktrackd drops the queues. */
static int migflow_release(struct inode *inode, struct file *file)
{
	WRITE_ONCE(g_tracker.target_pid, -1);
	kpebsd_exit();
	return 0;
}

static const struct file_operations migflow_fops = {
	.owner = THIS_MODULE,
	.open = migflow_open,
	.release = migflow_release,
	.unlocked_ioctl = migflow_ioctl,
	.mmap = migflow_mmap,
	.poll = migflow_poll,
};

static int __init migflow_init(void)
{
	int ret;

	ret = init_tracker();
	if (ret < 0)
		return ret;

	ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
	if (ret < 0)
		goto err_tracker;

	migflow_class = class_create(CLASS_NAME);
	if (IS_ERR(migflow_class)) {
		ret = PTR_ERR(migflow_class);
		goto err_region;
	}

	if (IS_ERR(device_create(migflow_class, NULL, dev_num, NULL, DEVICE_NAME))) {
		ret = -ENODEV;
		goto err_class;
	}

	cdev_init(&migflow_cdev, &migflow_fops);
	ret = cdev_add(&migflow_cdev, dev_num, 1);
	if (ret < 0)
		goto err_device;

	ret = register_kprobe(&kp_do_anonymous_page);
	if (ret < 0) {
		pr_err("migflow: failed to probe do_anonymous_page: %d\n", ret);
		goto err_cdev;
	}
	ret = register_kprobe(&kp_do_fault);
	if (ret < 0) {
		pr_err("migflow: failed to probe do_fault: %d\n", ret);
		goto err_kprobe1;
	}

	g_tracker.ktrackd = kthread_run(ktrackd_fn, NULL, "ktrackd");
	if (IS_ERR(g_tracker.ktrackd)) {
		ret = PTR_ERR(g_tracker.ktrackd);
		g_tracker.ktrackd = NULL;
		pr_err("migflow: failed to create ktrackd: %d\n", ret);
		goto err_kprobe2;
	}
	return 0;

err_kprobe2:
	unregister_kprobe(&kp_do_fault);
err_kprobe1:
	unregister_kprobe(&kp_do_anonymous_page);
err_cdev:
	cdev_del(&migflow_cdev);
err_device:
	device_destroy(migflow_class, dev_num);
err_class:
	class_destroy(migflow_class);
err_region:
	unregister_chrdev_region(dev_num, 1);
err_tracker:
	destroy_tracker();
	return ret;
}

static void __exit migflow_exit(void)
{
	unregister_kprobe(&kp_do_anonymous_page);
	unregister_kprobe(&kp_do_fault);
	kthread_stop(g_tracker.ktrackd);
	kpebsd_exit();

	cdev_del(&migflow_cdev);
	device_destroy(migflow_class, dev_num);
	class_destroy(migflow_class);
	unregister_chrdev_region(dev_num, 1);

	destroy_tracker();

	pr_info("migflow: allocations %lu, accessed in grace %lu, published %lu (%lu accessed), max rb %d\n",
		g_tracker.stat.nr_alloc, g_tracker.stat.nr_accessed, g_tracker.stat.nr_copied,
		g_tracker.stat.nr_copied_accessed, g_tracker.stat.max_rb_size);
}

module_init(migflow_init);
module_exit(migflow_exit);
