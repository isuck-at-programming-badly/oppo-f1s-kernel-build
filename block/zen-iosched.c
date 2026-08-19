/*
 *  Zen I/O scheduler
 *
 *  Features:
 *    - Prioritizes synchronous (read) requests over asynchronous (write) requests
 *    - FIFO dispatch within each category (read / write)
 *    - Configurable read/write batch sizes and expiration
 *    - Designed for desktop and mobile use cases
 *
 *  Based on the deadline and noop I/O schedulers by Jens Axboe.
 *  Zen scheduler concept originally by Con Kolivas.
 *
 *  Copyright (C) 2015 Zen developers
 *  Copyright (C) 2002-2014 Jens Axboe <axboe@kernel.dk>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/rbtree.h>

/*
 * Tunable defaults
 */
static const int zen_read_expire = HZ / 2;     /* max time before a read is dispatched */
static const int zen_write_expire = 2 * HZ;    /* max time before a write is dispatched */
static const int zen_writes_starved = 4;       /* max times reads starve writes */
static const int zen_front_merges = 1;         /* enable front merges */

struct zen_data {
	/*
	 * run time data
	 *
	 * Requests are kept on both the rbtree (sorted by sector) and
	 * the fifo list (sorted by insertion / expiration time).
	 */
	struct rb_root sort_list[2];   /* READ=0, WRITE=1 */
	struct list_head fifo_list[2]; /* READ=0, WRITE=1 */

	/* next request in sort order for each direction */
	struct request *next_rq[2];

	/* batch state */
	unsigned int batching;        /* sequential requests in current batch */
	sector_t last_sector;        /* last dispatched sector */
	unsigned int starved;        /* times reads have starved writes */

	/*
	 * tunables
	 */
	int fifo_expire[2];
	int writes_starved;
	int front_merges;
	int fifo_batch;
};

static inline struct rb_root *
zen_rb_root(struct zen_data *zd, struct request *rq)
{
	return &zd->sort_list[rq_data_dir(rq)];
}

static inline struct request *
zen_latter_request(struct request *rq)
{
	struct rb_node *node = rb_next(&rq->rb_node);

	if (node)
		return rb_entry_rq(node);

	return NULL;
}

static void
zen_add_rq_rb(struct zen_data *zd, struct request *rq)
{
	struct rb_root *root = zen_rb_root(zd, rq);

	elv_rb_add(root, rq);
}

static inline void
zen_del_rq_rb(struct zen_data *zd, struct request *rq)
{
	const int data_dir = rq_data_dir(rq);

	if (zd->next_rq[data_dir] == rq)
		zd->next_rq[data_dir] = zen_latter_request(rq);

	elv_rb_del(zen_rb_root(zd, rq), rq);
}

/*
 * Add request to rbtree (sorted) and fifo list (expiration time).
 */
static void
zen_add_request(struct request_queue *q, struct request *rq)
{
	struct zen_data *zd = q->elevator->elevator_data;
	const int data_dir = rq_data_dir(rq);

	zen_add_rq_rb(zd, rq);

	/* set expire time and add to fifo list */
	rq_set_fifo_time(rq, jiffies + zd->fifo_expire[data_dir]);
	list_add_tail(&rq->queuelist, &zd->fifo_list[data_dir]);
}

/*
 * Remove request from both rbtree and fifo.
 */
static void
zen_remove_request(struct request_queue *q, struct request *rq)
{
	struct zen_data *zd = q->elevator->elevator_data;

	rq_fifo_clear(rq);
	zen_del_rq_rb(zd, rq);
}

static int
zen_merge(struct request_queue *q, struct request **req, struct bio *bio)
{
	struct zen_data *zd = q->elevator->elevator_data;
	struct request *__rq;
	int ret;

	/* check for front merge */
	if (zd->front_merges) {
		sector_t sector = bio_end_sector(bio);

		__rq = elv_rb_find(&zd->sort_list[bio_data_dir(bio)], sector);
		if (__rq) {
			BUG_ON(sector != blk_rq_pos(__rq));

			if (elv_rq_merge_ok(__rq, bio)) {
				ret = ELEVATOR_FRONT_MERGE;
				goto out;
			}
		}
	}

	return ELEVATOR_NO_MERGE;
out:
	*req = __rq;
	return ret;
}

static void
zen_merged_request(struct request_queue *q, struct request *req, int type)
{
	struct zen_data *zd = q->elevator->elevator_data;

	/* reposition request on front merge */
	if (type == ELEVATOR_FRONT_MERGE) {
		elv_rb_del(zen_rb_root(zd, req), req);
		zen_add_rq_rb(zd, req);
	}
}

static void
zen_merged_requests(struct request_queue *q, struct request *req,
		    struct request *next)
{
	/*
	 * If next expires before req, assign its expire time to req
	 * and move into next's position in the fifo.
	 */
	if (!list_empty(&req->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before(rq_fifo_time(next), rq_fifo_time(req))) {
			list_move(&req->queuelist, &next->queuelist);
			rq_set_fifo_time(req, rq_fifo_time(next));
		}
	}

	zen_remove_request(q, next);
}

/*
 * Move a request from sort/fifo lists to the dispatch queue.
 */
static inline void
zen_move_to_dispatch(struct zen_data *zd, struct request *rq)
{
	struct request_queue *q = rq->q;

	zen_remove_request(q, rq);
	elv_dispatch_add_tail(q, rq);
}

static void
zen_move_request(struct zen_data *zd, struct request *rq)
{
	const int data_dir = rq_data_dir(rq);

	zd->next_rq[READ] = NULL;
	zd->next_rq[WRITE] = NULL;
	zd->next_rq[data_dir] = zen_latter_request(rq);

	zd->last_sector = rq_end_sector(rq);

	zen_move_to_dispatch(zd, rq);
}

/*
 * Check if the first request on a fifo list has expired.
 */
static inline int
zen_check_fifo(struct zen_data *zd, int ddir)
{
	struct request *rq = rq_entry_fifo(zd->fifo_list[ddir].next);

	if (time_after_eq(jiffies, rq_fifo_time(rq)))
		return 1;

	return 0;
}

/*
 * Main dispatch function: prioritize reads over writes, FIFO within
 * each category.
 */
static int
zen_dispatch_requests(struct request_queue *q, int force)
{
	struct zen_data *zd = q->elevator->elevator_data;
	const int reads = !list_empty(&zd->fifo_list[READ]);
	const int writes = !list_empty(&zd->fifo_list[WRITE]);
	struct request *rq;
	int data_dir;

	/*
	 * If we have a pending next request in the current batch
	 * and haven't exhausted the batch limit, continue dispatching.
	 */
	if (zd->next_rq[WRITE])
		rq = zd->next_rq[WRITE];
	else
		rq = zd->next_rq[READ];

	if (rq && zd->batching < zd->fifo_batch)
		goto dispatch_request;

	/*
	 * Not running a batch — select the appropriate data direction.
	 * Reads are prioritized over writes (writes_starved controls how
	 * many read batches can run before a write batch is forced).
	 */
	if (reads) {
		BUG_ON(RB_EMPTY_ROOT(&zd->sort_list[READ]));

		if (writes && (zd->starved++ >= zd->writes_starved))
			goto dispatch_writes;

		data_dir = READ;
		goto dispatch_find_request;
	}

	/*
	 * No reads or writes have been starved long enough.
	 */
	if (writes) {
dispatch_writes:
		BUG_ON(RB_EMPTY_ROOT(&zd->sort_list[WRITE]));

		zd->starved = 0;
		data_dir = WRITE;
		goto dispatch_find_request;
	}

	return 0;

dispatch_find_request:
	/*
	 * Find the best request for the selected direction.
	 * If the fifo has expired or we don't have a next sorted request,
	 * start from the request with the earliest expiry time.
	 */
	if (zen_check_fifo(zd, data_dir) || !zd->next_rq[data_dir]) {
		rq = rq_entry_fifo(zd->fifo_list[data_dir].next);
	} else {
		rq = zd->next_rq[data_dir];
	}

	zd->batching = 0;

dispatch_request:
	/*
	 * Dispatch the selected request.
	 */
	zd->batching++;
	zen_move_request(zd, rq);

	return 1;
}

static void
zen_exit_queue(struct elevator_queue *e)
{
	struct zen_data *zd = e->elevator_data;

	BUG_ON(!list_empty(&zd->fifo_list[READ]));
	BUG_ON(!list_empty(&zd->fifo_list[WRITE]));

	kfree(zd);
}

/*
 * Initialize the elevator's private data for a queue.
 */
static int
zen_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct zen_data *zd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	zd = kmalloc_node(sizeof(*zd), GFP_KERNEL | __GFP_ZERO, q->node);
	if (!zd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = zd;

	INIT_LIST_HEAD(&zd->fifo_list[READ]);
	INIT_LIST_HEAD(&zd->fifo_list[WRITE]);
	zd->sort_list[READ] = RB_ROOT;
	zd->sort_list[WRITE] = RB_ROOT;
	zd->fifo_expire[READ] = zen_read_expire;
	zd->fifo_expire[WRITE] = zen_write_expire;
	zd->writes_starved = zen_writes_starved;
	zd->front_merges = zen_front_merges;
	zd->fifo_batch = 16;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

/*
 * sysfs interface
 */

static ssize_t
zen_var_show(int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
zen_var_store(int *var, const char *page, size_t count)
{
	char *p = (char *)page;

	*var = simple_strtol(p, &p, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct zen_data *zd = e->elevator_data;				\
	int __data = __VAR;						\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return zen_var_show(__data, (page));				\
}
SHOW_FUNCTION(zen_read_expire_show, zd->fifo_expire[READ], 1);
SHOW_FUNCTION(zen_write_expire_show, zd->fifo_expire[WRITE], 1);
SHOW_FUNCTION(zen_writes_starved_show, zd->writes_starved, 0);
SHOW_FUNCTION(zen_front_merges_show, zd->front_merges, 0);
SHOW_FUNCTION(zen_fifo_batch_show, zd->fifo_batch, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count) \
{									\
	struct zen_data *zd = e->elevator_data;				\
	int __data;							\
	int ret = zen_var_store(&__data, (page), count);		\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	if (__CONV)							\
		*(__PTR) = msecs_to_jiffies(__data);			\
	else								\
		*(__PTR) = __data;					\
	return ret;							\
}
STORE_FUNCTION(zen_read_expire_store, &zd->fifo_expire[READ], 0, INT_MAX, 1);
STORE_FUNCTION(zen_write_expire_store, &zd->fifo_expire[WRITE], 0, INT_MAX, 1);
STORE_FUNCTION(zen_writes_starved_store, &zd->writes_starved, INT_MIN, INT_MAX, 0);
STORE_FUNCTION(zen_front_merges_store, &zd->front_merges, 0, 1, 0);
STORE_FUNCTION(zen_fifo_batch_store, &zd->fifo_batch, 0, INT_MAX, 0);
#undef STORE_FUNCTION

#define ZEN_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, zen_##name##_show, \
			      zen_##name##_store)

static struct elv_fs_entry zen_attrs[] = {
	ZEN_ATTR(read_expire),
	ZEN_ATTR(write_expire),
	ZEN_ATTR(writes_starved),
	ZEN_ATTR(front_merges),
	ZEN_ATTR(fifo_batch),
	__ATTR_NULL
};

static struct elevator_type iosched_zen = {
	.ops = {
		.elevator_merge_fn		= zen_merge,
		.elevator_merged_fn		= zen_merged_request,
		.elevator_merge_req_fn		= zen_merged_requests,
		.elevator_dispatch_fn		= zen_dispatch_requests,
		.elevator_add_req_fn		= zen_add_request,
		.elevator_former_req_fn		= elv_rb_former_request,
		.elevator_latter_req_fn		= elv_rb_latter_request,
		.elevator_init_fn		= zen_init_queue,
		.elevator_exit_fn		= zen_exit_queue,
	},

	.elevator_attrs = zen_attrs,
	.elevator_name = "zen",
	.elevator_owner = THIS_MODULE,
};

static int __init zen_init(void)
{
	return elv_register(&iosched_zen);
}

static void __exit zen_exit(void)
{
	elv_unregister(&iosched_zen);
}

module_init(zen_init);
module_exit(zen_exit);

MODULE_AUTHOR("Zen developers, based on Jens Axboe's deadline scheduler");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Zen IO scheduler");
