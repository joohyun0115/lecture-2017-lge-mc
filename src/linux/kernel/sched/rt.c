/*
 * Real-Time Scheduling Class (mapped to the SCHED_FIFO and SCHED_RR
 * policies)
 */

#include "sched.h"

#include <linux/slab.h>
#include <linux/irq_work.h>

#include "walt.h"

int sched_rr_timeslice = RR_TIMESLICE;

void init_rt_rq(struct rt_rq *rt_rq)
{
	struct rt_prio_array *array;
	int i;

	array = &rt_rq->active;
	for (i = 0; i < MAX_RT_PRIO; i++) {
		INIT_LIST_HEAD(array->queue + i);
		__clear_bit(i, array->bitmap);
	}
	/* delimiter for bitsearch: */
	__set_bit(MAX_RT_PRIO, array->bitmap);

	/* We start is dequeued state, because no RT tasks are queued */
	rt_rq->rt_queued = 0;

	rt_rq->rt_throttled = 0;
}

#define rt_entity_is_task(rt_se) (1)

static inline struct task_struct *rt_task_of(struct sched_rt_entity *rt_se)
{
	return container_of(rt_se, struct task_struct, rt);
}

static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return container_of(rt_rq, struct rq, rt);
}

static inline struct rq *rq_of_rt_se(struct sched_rt_entity *rt_se)
{
	struct task_struct *p = rt_task_of(rt_se);

	return task_rq(p);
}

static inline struct rt_rq *rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	struct rq *rq = rq_of_rt_se(rt_se);

	return &rq->rt;
}

static void enqueue_top_rt_rq(struct rt_rq *rt_rq);
static void dequeue_top_rt_rq(struct rt_rq *rt_rq);

static inline int on_rt_rq(struct sched_rt_entity *rt_se)
{
	return !list_empty(&rt_se->run_list);
}

typedef struct rt_rq *rt_rq_iter_t;

#define for_each_rt_rq(rt_rq, iter, rq) \
	for ((void) iter, rt_rq = &rq->rt; rt_rq; rt_rq = NULL)

#define for_each_sched_rt_entity(rt_se) \
	for (; rt_se; rt_se = NULL)

static inline struct rt_rq *group_rt_rq(struct sched_rt_entity *rt_se)
{
	return NULL;
}

static inline int rt_rq_throttled(struct rt_rq *rt_rq)
{
	return rt_rq->rt_throttled;
}

static inline int rt_se_prio(struct sched_rt_entity *rt_se)
{
	return rt_task_of(rt_se)->prio;
}

/*
 * Update the current task's runtime statistics. Skip current tasks that
 * are not in our scheduling class.
 */
static void update_curr_rt(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 delta_exec;

	if (curr->sched_class != &rt_sched_class)
		return;

	delta_exec = rq_clock_task(rq) - curr->se.exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = rq_clock_task(rq);
	cpuacct_charge(curr, delta_exec);

}

static void
dequeue_top_rt_rq(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	BUG_ON(&rq->rt != rt_rq);

	if (!rt_rq->rt_queued)
		return;

	BUG_ON(!rq->nr_running);

	sub_nr_running(rq, rt_rq->rt_nr_running);
	rt_rq->rt_queued = 0;
}

static void
enqueue_top_rt_rq(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	BUG_ON(&rq->rt != rt_rq);

	if (rt_rq->rt_queued)
		return;
	if (rt_rq_throttled(rt_rq) || !rt_rq->rt_nr_running)
		return;

	add_nr_running(rq, rt_rq->rt_nr_running);
	rt_rq->rt_queued = 1;
}

static inline
void inc_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	int prio = rt_se_prio(rt_se);

	WARN_ON(!rt_prio(prio));
	rt_rq->rt_nr_running += 1;

}

static inline
void dec_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	WARN_ON(!rt_prio(rt_se_prio(rt_se)));
	WARN_ON(!rt_rq->rt_nr_running);
	rt_rq->rt_nr_running -= 1;
}

static void __enqueue_rt_entity(struct sched_rt_entity *rt_se, bool head)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;
	struct list_head *queue = array->queue + rt_se_prio(rt_se);

	if (head)
		list_add(&rt_se->run_list, queue);
	else
		list_add_tail(&rt_se->run_list, queue);
	__set_bit(rt_se_prio(rt_se), array->bitmap);

	inc_rt_tasks(rt_se, rt_rq);
}

static void __dequeue_rt_entity(struct sched_rt_entity *rt_se)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;

	list_del_init(&rt_se->run_list);
	if (list_empty(array->queue + rt_se_prio(rt_se)))
		__clear_bit(rt_se_prio(rt_se), array->bitmap);

	dec_rt_tasks(rt_se, rt_rq);
}

/*
 * Because the prio of an upper entry depends on the lower
 * entries, we must remove entries top - down.
 */
static void dequeue_rt_stack(struct sched_rt_entity *rt_se)
{
	struct sched_rt_entity *back = NULL;

	for_each_sched_rt_entity(rt_se) {
		rt_se->back = back;
		back = rt_se;
	}

	dequeue_top_rt_rq(rt_rq_of_se(back));

	for (rt_se = back; rt_se; rt_se = rt_se->back) {
		if (on_rt_rq(rt_se))
			__dequeue_rt_entity(rt_se);
	}
}

static void enqueue_rt_entity(struct sched_rt_entity *rt_se, bool head)
{
	struct rq *rq = rq_of_rt_se(rt_se);

	dequeue_rt_stack(rt_se);
	for_each_sched_rt_entity(rt_se)
		__enqueue_rt_entity(rt_se, head);
	enqueue_top_rt_rq(&rq->rt);
}

static void dequeue_rt_entity(struct sched_rt_entity *rt_se)
{
	struct rq *rq = rq_of_rt_se(rt_se);

	dequeue_rt_stack(rt_se);

	for_each_sched_rt_entity(rt_se) {
		struct rt_rq *rt_rq = group_rt_rq(rt_se);

		if (rt_rq && rt_rq->rt_nr_running)
			__enqueue_rt_entity(rt_se, false);
	}
	enqueue_top_rt_rq(&rq->rt);
}

/*
 * Adding/removing a task to/from a priority array:
 */
static void
enqueue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;

	if (flags & ENQUEUE_WAKEUP)
		rt_se->timeout = 0;

	enqueue_rt_entity(rt_se, flags & ENQUEUE_HEAD);
	walt_inc_cumulative_runnable_avg(rq, p);

}

static void dequeue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;

	update_curr_rt(rq);
	dequeue_rt_entity(rt_se);
	walt_dec_cumulative_runnable_avg(rq, p);
}

/*
 * Put task to the head or the end of the run list without the overhead of
 * dequeue followed by enqueue.
 */
static void
requeue_rt_entity(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se, int head)
{
	if (on_rt_rq(rt_se)) {
		struct rt_prio_array *array = &rt_rq->active;
		struct list_head *queue = array->queue + rt_se_prio(rt_se);

		if (head)
			list_move(&rt_se->run_list, queue);
		else
			list_move_tail(&rt_se->run_list, queue);
	}
}

static void requeue_task_rt(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq;

	for_each_sched_rt_entity(rt_se) {
		rt_rq = rt_rq_of_se(rt_se);
		requeue_rt_entity(rt_rq, rt_se, head);
	}
}


/*
 * Preempt the current task with a newly woken task if needed:
 */
static void check_preempt_curr_rt(struct rq *rq, struct task_struct *p, int flags)
{
	if (p->prio < rq->curr->prio) {
		resched_curr(rq);
		return;
	}

}

static struct sched_rt_entity *pick_next_rt_entity(struct rq *rq,
						   struct rt_rq *rt_rq)
{
	struct rt_prio_array *array = &rt_rq->active;
	struct sched_rt_entity *next = NULL;
	struct list_head *queue;
	int idx;

	idx = sched_find_first_bit(array->bitmap);
	BUG_ON(idx >= MAX_RT_PRIO);

	queue = array->queue + idx;
	next = list_entry(queue->next, struct sched_rt_entity, run_list);

	return next;
}

static struct task_struct *_pick_next_task_rt(struct rq *rq)
{
	struct sched_rt_entity *rt_se;
	struct task_struct *p;
	struct rt_rq *rt_rq  = &rq->rt;

	do {
		rt_se = pick_next_rt_entity(rq, rt_rq);
		BUG_ON(!rt_se);
		rt_rq = group_rt_rq(rt_se);
	} while (rt_rq);

	p = rt_task_of(rt_se);
	p->se.exec_start = rq_clock_task(rq);

	return p;
}

static struct task_struct *
pick_next_task_rt(struct rq *rq, struct task_struct *prev)
{
	struct task_struct *p;
	struct rt_rq *rt_rq = &rq->rt;

	/*
	 * We may dequeue prev's rt_rq in put_prev_task().
	 * So, we update time before rt_nr_running check.
	 */
	if (prev->sched_class == &rt_sched_class)
		update_curr_rt(rq);

	if (!rt_rq->rt_queued) {
		/*
		 * The next task to be picked on this rq will have a lower
		 * priority than rt tasks so we can spend some time to update
		 * the capacity used by rt tasks based on the last activity.
		 * This value will be the used as an estimation of the next
		 * activity.
		 */
		return NULL;
	}

	put_prev_task(rq, prev);

	p = _pick_next_task_rt(rq);

	return p;
}

static void put_prev_task_rt(struct rq *rq, struct task_struct *p)
{
	update_curr_rt(rq);

}

static void task_tick_rt(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_rt_entity *rt_se = &p->rt;

	update_curr_rt(rq);


	if (p->policy != SCHED_RR)
		return;

	if (--p->rt.time_slice)
		return;

	p->rt.time_slice = sched_rr_timeslice;

	/*
	 * Requeue to the end of queue if we (and all of our ancestors) are not
	 * the only element on the queue
	 */
	for_each_sched_rt_entity(rt_se) {
		if (rt_se->run_list.prev != rt_se->run_list.next) {
			requeue_task_rt(rq, p, 0);
			resched_curr(rq);
			return;
		}
	}
}

static unsigned int get_rr_interval_rt(struct rq *rq, struct task_struct *task)
{
	/*
	 * Time slice is 0 for SCHED_FIFO tasks
	 */
	if (task->policy == SCHED_RR)
		return sched_rr_timeslice;
	else
		return 0;
}

/****  start dummy functions  ****/
static void yield_task_rt(struct rq *rq){requeue_task_rt(rq, rq->curr, 0);}
static int select_task_rq_rt(struct task_struct *p, int cpu, int sd_flag, int flags){return 0;}
static void rq_online_rt(struct rq *rq){}
static void rq_offline_rt(struct rq *rq){}
static void task_woken_rt(struct rq *rq, struct task_struct *p){}
static void switched_from_rt(struct rq *rq, struct task_struct *p){}
static void set_curr_task_rt(struct rq *rq) {}
static void prio_changed_rt(struct rq *rq, struct task_struct *p, int oldprio){}
static void switched_to_rt(struct rq *rq, struct task_struct *p){}
/****  end dummy functions  ****/


const struct sched_class rt_sched_class = {
	.next			= &fair_sched_class,
	.enqueue_task		= enqueue_task_rt,
	.dequeue_task		= dequeue_task_rt,
	.check_preempt_curr	= check_preempt_curr_rt,
	.pick_next_task		= pick_next_task_rt,
	.put_prev_task		= put_prev_task_rt,
	.update_curr		= update_curr_rt,
	.task_tick		= task_tick_rt,
	.get_rr_interval	= get_rr_interval_rt,

	/****  dummy functions  ****/
	.yield_task		= yield_task_rt,
#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_rt,

	.set_cpus_allowed       = set_cpus_allowed_common,
	.rq_online              = rq_online_rt,
	.rq_offline             = rq_offline_rt,
	.task_woken		= task_woken_rt,
	.switched_from		= switched_from_rt,
#endif
	.set_curr_task          = set_curr_task_rt,
	.prio_changed		= prio_changed_rt,
	.switched_to		= switched_to_rt,
	/****  dummy functions  ****/

};
