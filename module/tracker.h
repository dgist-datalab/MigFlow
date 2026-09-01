/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MIGFLOW_TRACKER_H
#define MIGFLOW_TRACKER_H

#include <linux/list.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

#include "../include/migflow_rb.h"

/* A page tracked by the 2Q allocation tracker. */
struct alloc_entry {
	unsigned long va;
	struct list_head list;
	unsigned long ts_jiffies;	/* time of entry into the current queue */
	int pid;
	int node;			/* NUMA node found by the grace-period check */
	bool last_accessed;		/* accessed during the grace period */
	bool checked;			/* grace-period check done */
};

/* A PEBS sample queued by kpebsd for ktrackd. */
struct pebs_entry {
	unsigned long va;
	struct list_head list;
	int type;			/* enum pebs_event */
	int pid;
};

struct alloc_list {
	struct list_head head;
	int length;
};

struct tracker_stat {
	unsigned long nr_alloc;
	unsigned long nr_accessed;
	unsigned long nr_copied;
	unsigned long nr_copied_accessed;
	int max_rb_size;
};

struct migflow_tracker {
	int target_pid;
	struct alloc_list burst_queue;
	struct alloc_list grace_queue;
	struct list_head pebs_list;	/* samples handed from kpebsd to ktrackd */
	struct kmem_cache *alloc_slab;
	struct kmem_cache *pebs_slab;
	struct task_struct *ktrackd;
	spinlock_t alloc_lock;		/* the two queues and RB_ALLOC */
	spinlock_t pebs_lock;		/* RB_PEBS */
	struct mutex pebs_mutex;	/* pebs_list */
	struct rb_head_t *rb[MAX_NR_RB];
	struct rb_data_t *rb_buf[MAX_NR_RB];
	int cur_rb_idx;			/* next ring buffer handed out by mmap() */
	int threshold_wakeup[MAX_NR_RB];
	wait_queue_head_t poll_wq;
	struct tracker_stat stat;
	int burst_msec;			/* current burst-queue residence time */
	int grace_msec;			/* current grace-queue residence time */
};

extern struct migflow_tracker g_tracker;

#endif
