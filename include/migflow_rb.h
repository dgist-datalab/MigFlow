/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Ring buffers shared between the MigFlow kernel module and umigratord.
 *
 * The module owns two ring buffers that userspace maps read-only:
 *   RB_ALLOC  pages that left the 2Q allocation tracker (ktrackd)
 *   RB_PEBS   PEBS access samples (kpebsd)
 * The module publishes records by advancing head (a release store); the
 * daemon reads head with an acquire load, consumes records from tail and
 * acknowledges them with IOCTL_MOVE_RB_TAIL, which advances tail.
 */
#ifndef MIGFLOW_RB_H
#define MIGFLOW_RB_H

#define RB_ALLOC 0
#define RB_PEBS  1
#define MAX_NR_RB 2

/* the daemon is woken up once a ring holds more than this many records */
#define RB_THRESHOLD_ALLOC 1024
#define RB_THRESHOLD_PEBS  1024
#define RB_BUF_SIZE (3UL * 1024 * 1024)
#define RB_HEADER_SIZE 4096

#define RB_FULL(rb)   (((rb->head + 1) % rb->size) == rb->tail)
#define RB_LEN(rb)    ((rb->head + rb->size - rb->tail) % rb->size)
#define RB_DEL(rb,nr) (rb->tail = (rb->tail + nr) % rb->size)

#define RB_TYPE_TO_FLAG(type) (1 << (type + 1))

struct rb_reply_t {
	int type;
	int nr_items;
};

/* A page that finished its burst and grace periods in the 2Q tracker. */
struct rb_data_alloc_t {
	unsigned long va;
	int node;		/* NUMA node the page resides on, or a negative errno */
	int last_accessed;	/* accessed during the grace period */
	int pid;		/* owning tgid */
};

/* One PEBS sample. */
struct rb_data_pebs_t {
	unsigned long va;	/* page-aligned virtual address */
	int type;		/* enum pebs_event */
	int pid;		/* owning tgid */
};

struct rb_data_t {
	union {
		struct rb_data_alloc_t rb_alloc;
		struct rb_data_pebs_t rb_pebs;
	} data;
};

struct rb_head_t {
	int head;
	int tail;
	int size;		/* number of records the ring can hold */
};

struct pebs_period {
	uint64_t read;
	uint64_t write;
};

/* ioctl interface of /dev/migflow */
#define MIGFLOW_DEVICE "/dev/migflow"
#define IOCTL_SET_PID       _IOW('a', 1, int)
#define IOCTL_MOVE_RB_TAIL  _IOW('a', 2, struct rb_reply_t)
#define IOCTL_GET_RB_STATUS _IOR('a', 3, int)
#define IOCTL_GET_PERIOD    _IOR('a', 4, struct pebs_period)

#endif
