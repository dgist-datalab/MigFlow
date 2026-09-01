/* SPDX-License-Identifier: GPL-2.0 */
/*
 * struct perf_buffer as defined in kernel/events/internal.h of Linux 6.6
 * (HMSDK v2.0). kpebsd reads the perf ring buffers through this structure,
 * which is not part of the installed kernel headers. It matches the kernel
 * the module is built for.
 */
#ifndef MIGFLOW_PERF_INTERNAL_H
#define MIGFLOW_PERF_INTERNAL_H

#include <linux/perf_event.h>
#include <linux/refcount.h>
#include <linux/workqueue.h>
#include <asm/local.h>

struct perf_buffer {
	refcount_t			refcount;
	struct rcu_head			rcu_head;
#ifdef CONFIG_PERF_USE_VMALLOC
	struct work_struct		work;
	int				page_order;	/* allocation order  */
#endif
	int				nr_pages;	/* nr of data pages  */
	int				overwrite;	/* can overwrite itself */
	int				paused;		/* can write into ring buffer */

	atomic_t			poll;		/* POLL_ for wakeups */

	local_t				head;		/* write position    */
	unsigned int			nest;		/* nested writers    */
	local_t				events;		/* event limit       */
	local_t				wakeup;		/* wakeup stamp      */
	local_t				lost;		/* nr records lost   */

	long				watermark;	/* wakeup watermark  */
	long				aux_watermark;
	spinlock_t			event_lock;
	struct list_head		event_list;

	atomic_t			mmap_count;
	unsigned long			mmap_locked;
	struct user_struct		*mmap_user;

	/* AUX area */
	long				aux_head;
	unsigned int			aux_nest;
	long				aux_wakeup;	/* last aux_watermark boundary crossed by aux_head */
	unsigned long			aux_pgoff;
	int				aux_nr_pages;
	int				aux_overwrite;
	atomic_t			aux_mmap_count;
	unsigned long			aux_mmap_locked;
	void				(*free_aux)(void *);
	refcount_t			aux_refcount;
	int				aux_in_sampling;
	void				**aux_pages;
	void				*aux_priv;

	struct perf_event_mmap_page	*user_page;
	void				*data_pages[];
};

#ifdef CONFIG_PERF_USE_VMALLOC
static inline int page_order(struct perf_buffer *rb)
{
	return rb->page_order;
}
#else
static inline int page_order(struct perf_buffer *rb)
{
	return 0;
}
#endif

#endif
