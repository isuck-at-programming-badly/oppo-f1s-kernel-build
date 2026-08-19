/*
 *  Maple I/O scheduler
 *
 *  Features:
 *    - Single queue with deadline-based dispatch for low latency
 *    - Requests sorted by sector in rbtree for merge-friendly dispatch
 *    - FIFO expiration ensures no request is starved
 *    - Minimal overhead, designed for low-latency workloads
 *
 *  Based on the deadline and noop I/O schedulers by Jens Axboe.
 *
 *  Copyright (C) 2015 Maple developers
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
static const int maple_expire = HZ / 2;   /* max time before a request is dispatched */
static const int maple_batch = 16;       /* batch size for sequential dispatch */
static const int maple_front_merges = 1; /* enable front merges */

struct maple_data {
	/*
	 * run time data
	 *
	 * Requests are kept on both the rbtree (sorted by sector) and
	 * a single fifo list (sorted by insertion / expiration time).
	 * Unlike deadline, there is no read/write separation — this is
	 * a single queue for lowest latency.
	 */
	struct rb_root sort_list;
	struct list_head fifo_list;

	/* next request in sort order */
	struct request *next_rq;

	/* batch state */
	unsigned int batching;
	sector_t last_sector;

	/*
	 * tunables
	 */
	int fifo_expire;
	int fifo_batch;
	int front_merges;
};

static inline struct request *
maple_latter_request(struct request *rq)
{
	struct rb_node *node = rb_next(&rq->rb_node);

	if (node)
		return rb_entry_rq(node);

	return NULL;
}

static void
maple_add_rq_rb(struct maple_data *md, struct request *rq)
{
	elv_rb_add(&md->sort_list, rq);
}

static inline void
maple_del_rq_rb(struct maple_data *md, struct request *rq)
{
	if (md->next_rq == rq)
		md->next_rq = maple_latter_request(rq);

	elv_rb_del(&md->sort_list, rq);
}

/*
 * Add request to rbtree (sorted by sector) and fifo list (expiration time).
 */
static void
maple_add_request(struct request_queue *q, struct request *rq)
{
	struct maple_data *md = q->elevator->elevator_data;

	maple_add_rq_rb(md, rq);

	/* set expire time and add to fifo list */
	rq_set_fifo_time(rq, jiffies + md->fifo_expire);
	list_add_tail(&rq->queuelist, &md->fifo_list);
}

/*
 * Remove request from both rbtree and fifo.
 */
static void
maple_remove_request(struct request_queue *q, struct request *rq)
{
	struct maple_data *md = q->elevator->elevator_data;

	rq_fifo_clear(rq);
	maple_del_rq_rb(md, rq);
}

static int
maple_merge(struct request_queue *q, struct request **req, struct bio *bio)
{
	struct maple_data *md = q->elevator->elevator_data;
	struct request *__rq;
	int ret;

	/* check for front merge */
	if (md->front_merges) {
		sector_t sector = bio_end_sector(bio);

		__rq = elv_rb_find(&md->sort_list, sector);
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
maple_merged_request(struct request_queue *q, struct request *req, int type)
{
	struct maple_data *md = q->elevator->elevator_data;

	/* reposition request on front merge */
	if (type == ELEVATOR_FRONT_MERGE) {
		elv_rb_del(&md->sort_list, req);
		maple_add_rq_rb(md, req);
	}
}

static void
maple_merged_requests(struct request_queue *q, struct request *req,
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

	maple_remove_request(q, next);
}

/*
 * Move a request from sort/fifo lists to the dispatch queue.
 */
static inline void
maple_move_to_dispatch(struct maple_data *md, struct request *rq)
{
	struct request_queue *q = rq->q;

	maple_remove_request(q, rq);
	elv_dispatch_add_tail(q, rq);
}

static void
maple_move_request(struct maple_data *md, struct request *rq)
{
	md->next_rq = maple_latter_request(rq);
	md->last_sector = rq_end_sector(rq);

	maple_move_to_dispatch(md, rq);
}

/*
 * Check if the first request on the fifo has expired.
 */
static inline int
maple_check_fifo(struct maple_data *md)
{
	struct request *rq = rq_entry_fifo(md->fifo_list.next);

	if (time_after_eq(jiffies, rq_fifo_time(rq)))
		return 1;

	return 0;
}

/*
 * Main dispatch function: single queue, deadline-based dispatch.
 *
 * Dispatch sequentially (batch) from the sort list. If the fifo deadline
 * has expired, jump to the expired request to guarantee latency.
 */
static int
maple_dispatch_requests(struct request_queue *q, int force)
{
	struct maple_data *md = q->elevator->elevator_data;

	if (list_empty(&md->fifo_list))
		return 0;

	/*
	 * Continue the current batch if we have a next sorted request
	 * and haven't exceeded the batch limit.
	 */
	if (md->next_rq && md->batching < md->fifo_batch)
		goto dispatch_request;

	/*
	 * Start a new batch. Check if the oldest request has expired;
	 * if so, dispatch from the fifo to honor the deadline.
	 */
	if (maple_check_fifo(md) || !md->next_rq) {
		struct request *rq;

		rq = rq_entry_fifo(md->fifo_list.next);
		md->batching = 0;
		maple_move_request(md, rq);
		md->batching++;
		return 1;
	}

dispatch_request:
	/*
	 * Dispatch the next request in sort order.
	 */
	md->batching++;
	maple_move_request(md, md->next_rq);

	return 1;
}

static void
maple_exit_queue(struct elevator_queue *e)
{
	struct maple_data *md = e->elevator_data;

	BUG_ON(!list_empty(&md->fifo_list));

	kfree(md);
}

/*
 * Initialize the elevator's private data for a queue.
 */
static int
maple_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct maple_data *md;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	md = kmalloc_node(sizeof(*md), GFP_KERNEL | __GFP_ZERO, q->node);
	if (!md) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = md;

	INIT_LIST_HEAD(&md->fifo_list);
	md->sort_list = RB_ROOT;
	md->fifo_expire = maple_expire;
	md->fifo_batch = maple_batch;
	md->front_merges = maple_front_merges;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

/*
 * sysfs interface
 */

static ssize_t
maple_var_show(int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
maple_var_store(int *var, const char *page, size_t count)
{
	char *p = (char *)page;

	*var = simple_strtol(p, &p, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct maple_data *md = e->elevator_data;			\
	int __data = __VAR;						\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return maple_var_show(__data, (page));				\
}
SHOW_FUNCTION(maple_fifo_expire_show, md->fifo_expire, 1);
SHOW_FUNCTION(maple_fifo_batch_show, md->fifo_batch, 0);
SHOW_FUNCTION(maple_front_merges_show, md->front_merges, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count) \
{									\
	struct maple_data *md = e->elevator_data;			\
	int __data;							\
	int ret = maple_var_store(&__data, (page), count);		\
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
STORE_FUNCTION(maple_fifo_expire_store, &md->fifo_expire, 0, INT_MAX, 1);
STORE_FUNCTION(maple_fifo_batch_store, &md->fifo_batch, 0, INT_MAX, 0);
STORE_FUNCTION(maple_front_merges_store, &md->front_merges, 0, 1, 0);
#undef STORE_FUNCTION

#define MAPLE_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, maple_##name##_show, \
				maple_##name##_store)

static struct elv_fs_entry maple_attrs[] = {
	MAPLE_ATTR(fifo_expire),
	MAPLE_ATTR(fifo_batch),
	MAPLE_ATTR(front_merges),
	__ATTR_NULL
};

static struct elevator_type iosched_maple = {
	.ops = {
		.elevator_merge_fn		= maple_merge,
		.elevator_merged_fn		= maple_merged_request,
		.elevator_merge_req_fn		= maple_merged_requests,
		.elevator_dispatch_fn		= maple_dispatch_requests,
		.elevator_add_req_fn		= maple_add_request,
		.elevator_former_req_fn		= elv_rb_former_request,
		.elevator_latter_req_fn		= elv_rb_latter_request,
		.elevator_init_fn		= maple_init_queue,
		.elevator_exit_fn		= maple_exit_queue,
	},

	.elevator_attrs = maple_attrs,
	.elevator_name = "maple",
	.elevator_owner = THIS_MODULE,
};

static int __init maple_init(void)
{
	return elv_register(&iosched_maple);
}

static void __exit maple_exit(void)
{
	elv_unregister(&iosched_maple);
}

module_init(maple_init);
module_exit(maple_exit);

MODULE_AUTHOR("Maple developers, based on Jens Axboe's deadline scheduler");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Maple IO scheduler");
