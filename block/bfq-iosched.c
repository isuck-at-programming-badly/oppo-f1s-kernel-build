/*
 *  BFQ (Budget Fair Queueing) I/O scheduler — simplified for kernel 3.10
 *
 *  This is a simplified implementation of the BFQ scheduling algorithm,
 *  built on the CFQ infrastructure (per-process queues, service trees
 *  using rbtrees, I/O priority classes). Instead of CFQ's time-slice
 *  based approach, this scheduler uses budget-based fair queueing:
 *  each queue is assigned a budget (in sectors) that determines how
 *  much I/O it can dispatch before yielding to the next queue.
 *
 *  Key differences from CFQ:
 *    - Budget-based dispatch instead of time slices
 *    - Budget is computed from the queue's weight (derived from I/O priority)
 *    - When a queue exhausts its budget, it is re-inserted into the service
 *      tree with an updated virtual finish time (vdisktime)
 *    - CFS-style virtual time (vdisktime) for fairness across queues
 *    - Weighted round-robin among queues in the service tree
 *
 *  This implementation is intentionally simplified compared to the full
 *  upstream BFQ: no hierarchical cgroup support, no early queue preemption,
 *  no throughput detection. It focuses on the core budget-based fairness.
 *
 *  Based on CFQ by Jens Axboe and the BFQ algorithm by Paolo Valente
 *  and Fabio Checconi.
 *
 *  Copyright (C) 2015 BFQ simplified implementation
 *  Copyright (C) 2003-2014 Jens Axboe <axboe@kernel.dk>
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
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/jiffies.h>
#include <linux/rbtree.h>
#include <linux/ioprio.h>
#include <linux/sched.h>
#include <linux/blktrace_api.h>
#include "blk.h"

/*
 * Tunables
 */
static const int bfq_quantum = 8;            /* default dispatch quantum */
static const int bfq_fifo_expire[2] = { HZ / 4, HZ / 8 };
static const int bfq_back_max = 16 * 1024;    /* max backwards seek (KiB) */
static const int bfq_back_penalty = 2;        /* backwards seek penalty */
static const int bfq_slice_idle = HZ / 125;  /* idle wait before expiring */
static int bfq_max_budget = 16 * 1024;       /* max budget in sectors */
static const int bfq_min_budget = 8;         /* min budget in sectors */
static const int bfq_hist_divisor = 4;

/*
 * Constants
 */
#define BFQ_IDLE_DELAY		(HZ / 5)
#define BFQ_MIN_TT		(2)
#define BFQ_HW_QUEUE_MIN	(5)
#define BFQ_SERVICE_SHIFT	12

#define BFQQ_SEEK_THR		(sector_t)(8 * 100)
#define BFQQ_CLOSE_THR		(sector_t)(8 * 1024)
#define BFQQ_SECT_THR_NONROT	(sector_t)(2 * 32)
#define BFQQ_SEEKY(bfqq)	(hweight32((bfqq)->seek_history) > 32/8)

#define RQ_BFQQ(rq)		(struct bfq_queue *)((rq)->elv.priv[0])

static struct kmem_cache *bfq_pool;

/*
 * Service tree — rbtree with cached leftmost node for O(1) min extraction.
 * Uses virtual disk time (vdisktime) for fair scheduling across queues.
 */
struct bfq_rb_root {
	struct rb_root rb;
	struct rb_node *left;
	unsigned int count;
	u64 min_vdisktime;
};
#define BFQ_RB_ROOT	(struct bfq_rb_root) { .rb = RB_ROOT }

/*
 * Per-process queue structure
 */
struct bfq_queue {
	/* reference count */
	int ref;
	/* various state flags, see below */
	unsigned int flags;
	/* parent bfq_data */
	struct bfq_data *bfqd;
	/* service tree member */
	struct rb_node rb_node;
	/* service tree key (virtual finish time) */
	unsigned long rb_key;
	/* sorted list of pending requests */
	struct rb_root sort_list;
	/* if fifo isn't expired, next request to serve */
	struct request *next_rq;
	/* requests queued in sort_list */
	int queued[2];
	/* currently allocated requests */
	int allocated[2];
	/* fifo list of requests */
	struct list_head fifo;

	/* budget management */
	unsigned int budget;          /* current budget in sectors */
	unsigned int dispatched;     /* sectors dispatched this round */
	unsigned int max_budget;     /* max budget for this queue */

	/* time tracking */
	unsigned long dispatch_start;
	unsigned long slice_start;
	unsigned long slice_end;
	long slice_resid;

	/* I/O priority */
	unsigned short ioprio, org_ioprio;
	unsigned short ioprio_class;

	/* seek stats */
	u32 seek_history;
	sector_t last_request_pos;

	/* service tree this queue belongs to */
	struct bfq_rb_root *service_tree;
	pid_t pid;
};

/*
 * Per-device data structure
 */
struct bfq_data {
	struct request_queue *queue;

	/* service tree for all queues */
	struct bfq_rb_root service_tree;

	/* currently active queue */
	struct bfq_queue *active_queue;

	/* rb trees for sorting by sector */
	struct rb_root prio_trees[IOPRIO_BE_NR];

	unsigned int busy_queues;
	int rq_in_driver;
	int rq_in_flight[2];

	int rq_queued;
	int hw_tag;

	/* tunables */
	int fifo_expire[2];
	int bfq_quantum;
	int back_max;
	int back_penalty;
	int slice_idle;
	int max_budget;

	struct timer_list idle_slice_timer;
	struct work_struct unplug_work;
};

enum bfq_state_flags {
	BFQ_BFQQ_FLAG_on_rr = 0,       /* on round-robin list */
	BFQ_BFQQ_FLAG_wait_request,    /* waiting for a request */
	BFQ_BFQQ_FLAG_must_alloc,      /* must be allowed to allocate */
	BFQ_BFQQ_FLAG_must_alloc_slice,/* per-slice must allocate */
	BFQ_BFQQ_FLAG_idle_window,     /* has idle window */
	BFQ_BFQQ_FLAG_prio_changed,   /* priority changed */
};

#define BFQ_BFQQ_FNS(name)						\
static inline void bfq_mark_bfqq_##name(struct bfq_queue *bfqq)		\
{ (bfqq)->flags |= (1 << BFQ_BFQQ_FLAG_##name); }			\
static inline void bfq_clear_bfqq_##name(struct bfq_queue *bfqq)		\
{ (bfqq)->flags &= ~(1 << BFQ_BFQQ_FLAG_##name); }			\
static inline int bfq_bfqq_##name(const struct bfq_queue *bfqq)		\
{ return ((bfqq)->flags & (1 << BFQ_BFQQ_FLAG_##name)) != 0; }

BFQ_BFQQ_FNS(on_rr);
BFQ_BFQQ_FNS(wait_request);
BFQ_BFQQ_FNS(must_alloc);
BFQ_BFQQ_FNS(must_alloc_slice);
BFQ_BFQQ_FNS(idle_window);
BFQ_BFQQ_FNS(prio_changed);
#undef BFQ_BFQQ_FNS

/*
 * Compute the weight of a queue based on its I/O priority.
 * Higher priority = higher weight = larger budget.
 */
static unsigned int
bfq_prio_to_weight(unsigned short ioprio)
{
	/* IOPRIO_BE_NR is 8. Weight ranges from ~1 to ~100. */
	int prio = IOPRIO_PRIO_DATA(ioprio);
	unsigned int weight;

	if (prio >= IOPRIO_BE_NR)
		prio = IOPRIO_BE_NR - 1;

	/* weight = 100 / (1 + prio) — linear, higher prio = more weight */
	weight = 100 / (1 + prio);
	if (weight == 0)
		weight = 1;

	return weight;
}

/*
 * Compute the budget for a queue based on its weight and the global
 * max_budget tunable.
 */
static unsigned int
bfq_compute_budget(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	unsigned int weight = bfq_prio_to_weight(bfqq->ioprio);
	unsigned int budget;

	/*
	 * Budget = max_budget * (weight / total_weight_in_class).
	 * Simplified: budget = max_budget * weight / 100.
	 * This gives proportional fairness based on weight.
	 */
	budget = (bfqd->max_budget * weight) / 100;
	if (budget < bfq_min_budget)
		budget = bfq_min_budget;
	if (budget > (unsigned int)bfqd->max_budget)
		budget = bfqd->max_budget;

	return budget;
}

/*
 * Compute the virtual disk time key for inserting a queue into the
 * service tree. Uses the same approach as CFQ/CFS: vdisktime grows
 * inversely with weight, so heavier queues have smaller keys and
 * are served first.
 */
static u64
bfq_scale_delta(unsigned long delta, unsigned int weight)
{
	/* Scale by weight: delta * (base_weight / weight) */
	if (weight == 0)
		weight = 1;

	/* Use fixed-point arithmetic with BFQ_SERVICE_SHIFT */
	return (u64)delta * (1 << BFQ_SERVICE_SHIFT) / weight;
}

static unsigned long
bfq_service_tree_key(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	struct bfq_rb_root *st = &bfqd->service_tree;

	/*
	 * If the service tree is empty, use min_vdisktime.
	 * Otherwise, the key is min_vdisktime + scaled budget.
	 */
	return st->min_vdisktime +
	       bfq_scale_delta(bfqq->max_budget,
			       bfq_prio_to_weight(bfqq->ioprio));
}

/*
 * rbtree helpers for the service tree
 */
static void
bfq_rb_insert(struct bfq_rb_root *root, struct bfq_queue *bfqq)
{
	struct rb_node **p = &root->rb.rb_node;
	struct rb_node *parent = NULL;
	unsigned long key = bfqq->rb_key;

	while (*p) {
		struct bfq_queue *__bfqq;
		parent = *p;
		__bfqq = rb_entry(parent, struct bfq_queue, rb_node);

		if (key < __bfqq->rb_key)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	rb_link_node(&bfqq->rb_node, parent, p);
	rb_insert_color(&bfqq->rb_node, &root->rb);

	/* update leftmost */
	if (!root->left || key <
	    rb_entry(root->left, struct bfq_queue, rb_node)->rb_key)
		root->left = &bfqq->rb_node;

	root->count++;
}

static void
bfq_rb_erase(struct bfq_rb_root *root, struct bfq_queue *bfqq)
{
	if (root->left == &bfqq->rb_node)
		root->left = rb_next(&bfqq->rb_node);

	rb_erase(&bfqq->rb_node, &root->rb);
	root->count--;
}

static struct bfq_queue *
bfq_rb_first(struct bfq_rb_root *root)
{
	if (root->left)
		return rb_entry(root->left, struct bfq_queue, rb_node);

	return NULL;
}

/*
 * Request-level helpers
 */
static inline struct request *
bfq_latter_request(struct request *rq)
{
	struct rb_node *node = rb_next(&rq->rb_node);

	if (node)
		return rb_entry_rq(node);

	return NULL;
}

static inline struct request *
bfq_former_request(struct request *rq)
{
	struct rb_node *node = rb_prev(&rq->rb_node);

	if (node)
		return rb_entry_rq(node);

	return NULL;
}

static void
bfq_add_rq_rb(struct bfq_queue *bfqq, struct request *rq)
{
	elv_rb_add(&bfqq->sort_list, rq);
}

static inline void
bfq_del_rq_rb(struct bfq_queue *bfqq, struct request *rq)
{
	if (bfqq->next_rq == rq)
		bfqq->next_rq = bfq_latter_request(rq);

	elv_rb_del(&bfqq->sort_list, rq);
}

static void
bfq_add_request(struct request_queue *q, struct request *rq)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct bfq_queue *bfqq = RQ_BFQQ(rq);
	const int data_dir = rq_data_dir(rq);

	if (!bfqq) {
		/* This shouldn't happen, but handle gracefully */
		return;
	}

	bfq_add_rq_rb(bfqq, rq);

	/* set expire time and add to fifo */
	rq_set_fifo_time(rq, jiffies + bfqd->fifo_expire[data_dir]);
	list_add_tail(&rq->queuelist, &bfqq->fifo);

	bfqq->queued[data_dir]++;

	if (bfqq->next_rq == NULL)
		bfqq->next_rq = rq;
}

static void
bfq_remove_request(struct request_queue *q, struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);

	if (!bfqq)
		return;

	rq_fifo_clear(rq);
	bfq_del_rq_rb(bfqq, rq);

	bfqq->queued[rq_data_dir(rq)]--;
}

static int
bfq_merge(struct request_queue *q, struct request **req, struct bio *bio)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct request *__rq;
	struct bfq_queue *bfqq;
	struct rb_node *node;
	int ret = ELEVATOR_NO_MERGE;

	/*
	 * Search all queues' sort lists for a merge candidate.
	 * For simplicity, we search the service tree.
	 */
	node = rb_first(&bfqd->service_tree.rb);
	while (node) {
		bfqq = rb_entry(node, struct bfq_queue, rb_node);

		__rq = elv_rb_find(&bfqq->sort_list, bio_end_sector(bio));
		if (__rq && elv_rq_merge_ok(__rq, bio)) {
			*req = __rq;
			ret = ELEVATOR_FRONT_MERGE;
			break;
		}
		node = rb_next(node);
	}

	return ret;
}

static void
bfq_merged_request(struct request_queue *q, struct request *req, int type)
{
	struct bfq_queue *bfqq = RQ_BFQQ(req);

	if (type == ELEVATOR_FRONT_MERGE && bfqq) {
		elv_rb_del(&bfqq->sort_list, req);
		bfq_add_rq_rb(bfqq, req);
	}
}

static void
bfq_merged_requests(struct request_queue *q, struct request *req,
		   struct request *next)
{
	struct bfq_queue *bfqq = RQ_BFQQ(req);

	if (!list_empty(&req->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before(rq_fifo_time(next), rq_fifo_time(req))) {
			list_move(&req->queuelist, &next->queuelist);
			rq_set_fifo_time(req, rq_fifo_time(next));
		}
	}

	if (bfqq)
		bfqq->queued[rq_data_dir(next)]--;

	bfq_remove_request(q, next);
}

/*
 * Move a request from the queue's sort list to the dispatch list.
 */
static inline void
bfq_move_to_dispatch(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		     struct request *rq)
{
	struct request_queue *q = bfqd->queue;

	bfq_remove_request(q, rq);
	elv_dispatch_add_tail(q, rq);

	/* Track dispatched sectors for budget accounting */
	bfqq->dispatched += blk_rq_sectors(rq);
	bfqd->rq_in_driver++;
}

/*
 * Dispatch a single request from a queue.
 */
static int
bfq_dispatch_request(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	struct request *rq;

	if (list_empty(&bfqq->fifo))
		return 0;

	/*
	 * Pick the next request. Use the sort list for sequential dispatch,
	 * but check the fifo for expired requests.
	 */
	if (bfqq->next_rq) {
		/* Check if the fifo has expired */
		struct request *fifo_rq = rq_entry_fifo(bfqq->fifo.next);
		if (time_after_eq(jiffies, rq_fifo_time(fifo_rq)))
			rq = fifo_rq;
		else
			rq = bfqq->next_rq;
	} else {
		rq = rq_entry_fifo(bfqq->fifo.next);
	}

	bfq_move_to_dispatch(bfqd, bfqq, rq);

	return 1;
}

/*
 * Check if the active queue has consumed its budget.
 */
static int
bfq_budget_exhausted(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	return bfqq->dispatched >= bfqq->budget;
}

/*
 * Expire the active queue: remove it from the service tree, update
 * vdisktime, and re-insert if it still has pending requests.
 */
static void
bfq_expire_active_queue(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq = bfqd->active_queue;
	struct bfq_rb_root *st = &bfqd->service_tree;

	if (!bfqq)
		return;

	/*
	 * Update min_vdisktime to the key of this queue.
	 * This ensures fairness: the next queue's key will be at least
	 * as large as the current queue's.
	 */
	if (bfqq->rb_key > st->min_vdisktime)
		st->min_vdisktime = bfqq->rb_key;

	/* Remove from service tree */
	if (bfq_bfqq_on_rr(bfqq)) {
		bfq_rb_erase(st, bfqq);
		bfq_clear_bfqq_on_rr(bfqq);
	}

	/*
	 * Re-insert if the queue still has pending requests.
	 * Compute a new key (virtual finish time) based on the budget
	 * just consumed.
	 */
	if (!list_empty(&bfqq->fifo)) {
		unsigned int weight = bfq_prio_to_weight(bfqq->ioprio);

		/* New key: vdisktime + scaled dispatched (budget consumed) */
		bfqq->rb_key = st->min_vdisktime +
			       bfq_scale_delta(bfqq->dispatched, weight);
		bfqq->dispatched = 0;
		bfqq->budget = bfq_compute_budget(bfqd, bfqq);
		bfq_mark_bfqq_on_rr(bfqq);
		bfq_rb_insert(st, bfqq);
	}

	bfqd->active_queue = NULL;
}

/*
 * Select the next queue to serve: the one with the smallest key
 * (virtual finish time) in the service tree.
 */
static struct bfq_queue *
bfq_select_queue(struct bfq_data *bfqd)
{
	return bfq_rb_first(&bfqd->service_tree);
}

/*
 * Main dispatch function.
 */
static int
bfq_dispatch_requests(struct request_queue *q, int force)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct bfq_queue *bfqq;
	int dispatched = 0;
	int max_dispatch = bfqd->bfq_quantum;

	/*
	 * If we have an active queue that still has budget, keep dispatching
	 * from it.
	 */
	bfqq = bfqd->active_queue;

	if (bfqq && !bfq_budget_exhausted(bfqd, bfqq) &&
	    !list_empty(&bfqq->fifo)) {
		while (dispatched < max_dispatch &&
		       !bfq_budget_exhausted(bfqd, bfqq) &&
		       !list_empty(&bfqq->fifo)) {
			if (!bfq_dispatch_request(bfqd, bfqq))
				break;
			dispatched++;
		}

		if (dispatched)
			return dispatched;

		/* Budget exhausted — expire and pick next */
		if (bfq_budget_exhausted(bfqd, bfqq))
			bfq_expire_active_queue(bfqd);
	}

	/*
	 * Select the next queue from the service tree.
	 */
	bfqq = bfq_select_queue(bfqd);
	if (!bfqq)
		return 0;

	bfqd->active_queue = bfqq;

	/* Initialize budget if needed */
	if (bfqq->budget == 0)
		bfqq->budget = bfq_compute_budget(bfqd, bfqq);

	/*
	 * Dispatch from the selected queue up to the quantum or budget.
	 */
	while (dispatched < max_dispatch &&
	       !bfq_budget_exhausted(bfqd, bfqq) &&
	       !list_empty(&bfqq->fifo)) {
		if (!bfq_dispatch_request(bfqd, bfqq))
			break;
		dispatched++;
	}

	/*
	 * If the queue is now empty, expire it immediately.
	 */
	if (list_empty(&bfqq->fifo))
		bfq_expire_active_queue(bfqd);

	return dispatched;
}

static void
bfq_completed_request(struct request_queue *q, struct request *rq)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;

	bfqd->rq_in_driver--;
}

static void
bfq_exit_queue(struct elevator_queue *e)
{
	struct bfq_data *bfqd = e->elevator_data;

	BUG_ON(bfqd->active_queue);
	BUG_ON(!RB_EMPTY_ROOT(&bfqd->service_tree.rb));

	kfree(bfqd);
}

static int
bfq_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct bfq_data *bfqd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	bfqd = kmalloc_node(sizeof(*bfqd), GFP_KERNEL | __GFP_ZERO, q->node);
	if (!bfqd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = bfqd;

	bfqd->queue = q;
	bfqd->service_tree = BFQ_RB_ROOT;
	bfqd->active_queue = NULL;
	bfqd->busy_queues = 0;
	bfqd->rq_in_driver = 0;
	bfqd->rq_in_flight[0] = 0;
	bfqd->rq_in_flight[1] = 0;
	bfqd->rq_queued = 0;
	bfqd->hw_tag = -1;
	bfqd->fifo_expire[READ] = bfq_fifo_expire[READ];
	bfqd->fifo_expire[WRITE] = bfq_fifo_expire[WRITE];
	bfqd->bfq_quantum = bfq_quantum;
	bfqd->back_max = bfq_back_max;
	bfqd->back_penalty = bfq_back_penalty;
	bfqd->slice_idle = bfq_slice_idle;
	bfqd->max_budget = bfq_max_budget;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

/*
 * Per-request setup: allocate or find the bfq_queue for the requesting
 * task. This is a simplified version — we use a simple pid-based lookup
 * without iocontext integration.
 */
static int
bfq_set_request(struct request_queue *q, struct request *rq,
		struct bio *bio, gfp_t gfp_mask)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct bfq_queue *bfqq;
	const int is_sync = rq_is_sync(rq);
	unsigned long flags;

	might_sleep_if(gfp_mask & __GFP_WAIT);

	spin_lock_irqsave(q->queue_lock, flags);

	/*
	 * Simple approach: create a new queue per request.
	 * A full implementation would use iocontext/icq to share queues
	 * across requests from the same process. This simplified version
	 * creates a queue per request and adds it to the service tree.
	 */
	bfqq = kmem_cache_alloc_node(bfq_pool, gfp_mask | __GFP_ZERO, q->node);
	if (!bfqq) {
		spin_unlock_irqrestore(q->queue_lock, flags);
		return -ENOMEM;
	}

	bfqq->bfqd = bfqd;
	bfqq->ref = 1;
	bfqq->flags = 0;
	bfqq->sort_list = RB_ROOT;
	INIT_LIST_HEAD(&bfqq->fifo);
	bfqq->next_rq = NULL;
	bfqq->pid = current->pid;
	bfqq->ioprio = IOPRIO_NORM;
	bfqq->ioprio_class = IOPRIO_CLASS_BE;
	bfqq->org_ioprio = bfqq->ioprio;
	bfqq->budget = bfq_compute_budget(bfqd, bfqq);
	bfqq->max_budget = bfqq->budget;
	bfqq->dispatched = 0;
	bfqq->queued[0] = 0;
	bfqq->queued[1] = 0;
	bfqq->allocated[0] = 0;
	bfqq->allocated[1] = 0;
	bfqq->service_tree = NULL;

	/* Insert into service tree */
	bfqq->rb_key = bfq_service_tree_key(bfqd, bfqq);
	bfq_mark_bfqq_on_rr(bfqq);
	bfq_rb_insert(&bfqd->service_tree, bfqq);
	bfqd->busy_queues++;

	rq->elv.priv[0] = bfqq;

	spin_unlock_irqrestore(q->queue_lock, flags);
	return 0;
}

static void
bfq_put_request(struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);

	if (bfqq) {
		/* Decrement ref; free if last ref */
		if (--bfqq->ref == 0) {
			struct bfq_data *bfqd = bfqq->bfqd;

			if (bfq_bfqq_on_rr(bfqq)) {
				bfq_rb_erase(&bfqd->service_tree, bfqq);
				bfq_clear_bfqq_on_rr(bfqq);
			}
			bfqd->busy_queues--;
			kmem_cache_free(bfq_pool, bfqq);
		}
		rq->elv.priv[0] = NULL;
	}
}

/*
 * sysfs interface
 */

static ssize_t
bfq_var_show(int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
bfq_var_store(int *var, const char *page, size_t count)
{
	char *p = (char *)page;

	*var = simple_strtol(p, &p, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct bfq_data *bfqd = e->elevator_data;			\
	int __data = __VAR;						\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return bfq_var_show(__data, (page));				\
}
SHOW_FUNCTION(bfq_quantum_show, bfqd->bfq_quantum, 0);
SHOW_FUNCTION(bfq_fifo_expire_sync_show, bfqd->fifo_expire[READ], 1);
SHOW_FUNCTION(bfq_fifo_expire_async_show, bfqd->fifo_expire[WRITE], 1);
SHOW_FUNCTION(bfq_back_seek_max_show, bfqd->back_max, 0);
SHOW_FUNCTION(bfq_back_seek_penalty_show, bfqd->back_penalty, 0);
SHOW_FUNCTION(bfq_slice_idle_show, bfqd->slice_idle, 1);
SHOW_FUNCTION(bfq_max_budget_show, bfqd->max_budget, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count) \
{									\
	struct bfq_data *bfqd = e->elevator_data;			\
	int __data;							\
	int ret = bfq_var_store(&__data, (page), count);		\
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
STORE_FUNCTION(bfq_quantum_store, &bfqd->bfq_quantum, 1, INT_MAX, 0);
STORE_FUNCTION(bfq_fifo_expire_sync_store, &bfqd->fifo_expire[READ], 1, INT_MAX, 1);
STORE_FUNCTION(bfq_fifo_expire_async_store, &bfqd->fifo_expire[WRITE], 1, INT_MAX, 1);
STORE_FUNCTION(bfq_back_seek_max_store, &bfqd->back_max, 0, INT_MAX, 0);
STORE_FUNCTION(bfq_back_seek_penalty_store, &bfqd->back_penalty, 1, INT_MAX, 0);
STORE_FUNCTION(bfq_slice_idle_store, &bfqd->slice_idle, 0, INT_MAX, 1);
STORE_FUNCTION(bfq_max_budget_store, &bfqd->max_budget, 1, INT_MAX, 0);
#undef STORE_FUNCTION

#define BFQ_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, bfq_##name##_show, \
				 bfq_##name##_store)

static struct elv_fs_entry bfq_attrs[] = {
	BFQ_ATTR(quantum),
	BFQ_ATTR(fifo_expire_sync),
	BFQ_ATTR(fifo_expire_async),
	BFQ_ATTR(back_seek_max),
	BFQ_ATTR(back_seek_penalty),
	BFQ_ATTR(slice_idle),
	BFQ_ATTR(max_budget),
	__ATTR_NULL
};

static struct elevator_type iosched_bfq = {
	.ops = {
		.elevator_merge_fn		= bfq_merge,
		.elevator_merged_fn		= bfq_merged_request,
		.elevator_merge_req_fn		= bfq_merged_requests,
		.elevator_dispatch_fn		= bfq_dispatch_requests,
		.elevator_add_req_fn		= bfq_add_request,
		.elevator_completed_req_fn	= bfq_completed_request,
		.elevator_former_req_fn		= elv_rb_former_request,
		.elevator_latter_req_fn		= elv_rb_latter_request,
		.elevator_set_req_fn		= bfq_set_request,
		.elevator_put_req_fn		= bfq_put_request,
		.elevator_init_fn		= bfq_init_queue,
		.elevator_exit_fn		= bfq_exit_queue,
	},

	.elevator_attrs = bfq_attrs,
	.elevator_name = "bfq",
	.elevator_owner = THIS_MODULE,
};

static int __init bfq_init(void)
{
	int ret;

	bfq_pool = KMEM_CACHE(bfq_queue, 0);
	if (!bfq_pool)
		return -ENOMEM;

	ret = elv_register(&iosched_bfq);
	if (ret) {
		kmem_cache_destroy(bfq_pool);
		return ret;
	}

	return 0;
}

static void __exit bfq_exit(void)
{
	elv_unregister(&iosched_bfq);
	kmem_cache_destroy(bfq_pool);
}

module_init(bfq_init);
module_exit(bfq_exit);

MODULE_AUTHOR("Paolo Valente, Fabio Checconi (BFQ); simplified for 3.10");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BFQ (Budget Fair Queueing) IO scheduler");
