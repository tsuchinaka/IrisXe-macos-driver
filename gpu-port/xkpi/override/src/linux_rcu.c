/*
 * LinuxKPI RCU / SRCU for xnu (replaces FreeBSD's linux_rcu.c).
 *
 * Why it is replaced (10-17 freeze hunt): FreeBSD's version keeps one Concurrency Kit epoch record per CPU and pins
 * each reader to its CPU with sched_pin() so rcu_read_unlock() finds the same record. On xnu a kext cannot pin a thread;
 * the port's sched_pin() only disabled interrupts on the current CPU. SRCU readers are allowed to sleep, and i915 does
 * sleep inside them (e.g. waiting on a request under gt->reset.backoff_srcu). A reader that slept woke on another CPU,
 * ended its section on that CPU's record and decremented that CPU's interrupt-nesting count: the first CPU's record
 * stayed "active" forever, so every synchronize_rcu() - the call_rcu cleaner on linuxkpi_irq_wq, drm file close, GEM
 * and syncobj creation - spun for ever (spindump: four threads in linux_synchronize_rcu -> ck_epoch_synchronize_wait),
 * and the corrupted interrupt state froze the whole machine.
 *
 * This version does not depend on the CPU at all. Each RCU type has two reader counters and a phase bit:
 *   read_lock    take the current phase p, count the reader in cnt[p], re-check the phase (retry if it flipped
 *                in between) and remember p in the task (rcu_section[type]); nested sections only count once
 *   read_unlock  uncount from the remembered cnt[p], on whatever CPU the thread now runs
 *   synchronize  flip the phase, then wait until the old phase's counter drains to zero
 * Readers that began before the flip are counted in the old phase and waited for; readers that begin after it use the
 * new phase. Readers may sleep and migrate (as SRCU requires and preemptible RCU allows). Interrupts stay enabled.
 * call_rcu / kfree_rcu / rcu_barrier keep FreeBSD's design: callbacks are batched on linuxkpi_irq_wq behind one
 * grace period.
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/kdb.h>
#include <machine/cpu.h>

#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/srcu.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/compat.h>
#include <linux/llist.h>
#include <linux/irq_work.h>

#ifdef CONFIG_NO_RCU_SKIP
#define	RCU_SKIP(void) 0
#else
#define	RCU_SKIP(void)	unlikely(SCHEDULER_STOPPED() || kdb_active)
#endif

struct callback_head {
	union {
		STAILQ_ENTRY(callback_head) entry;
		struct llist_node node;
	};
	rcu_callback_t func;
};
CTASSERT(sizeof(struct rcu_head) == sizeof(struct callback_head));
CTASSERT(TS_RCU_TYPE_MAX == RCU_TYPE_MAX);

struct linux_epoch_head {
	struct llist_head cb_head;
	struct task task;
} __aligned(CACHE_LINE_SIZE);

struct nmx_rcu_gp {
	volatile unsigned phase;          /* bit 0: the phase new readers join */
	volatile long readers[2];         /* readers counted in each phase */
	volatile int sync_busy;           /* one grace period at a time per type */
	volatile unsigned long grace_periods;
} __aligned(CACHE_LINE_SIZE);

static struct linux_epoch_head linux_epoch_head[RCU_TYPE_MAX];
static struct nmx_rcu_gp nmx_gp[RCU_TYPE_MAX];

static void linux_rcu_cleaner_func(void *, int);

static void
linux_rcu_runtime_init(void *arg __unused)
{
	for (int j = 0; j != RCU_TYPE_MAX; j++) {
		struct linux_epoch_head *head = &linux_epoch_head[j];
		TASK_INIT(&head->task, 0, linux_rcu_cleaner_func, head);
		init_llist_head(&head->cb_head);
		nmx_gp[j].phase = 0;
		nmx_gp[j].readers[0] = nmx_gp[j].readers[1] = 0;
		nmx_gp[j].sync_busy = 0;
	}
}
SYSINIT(linux_rcu_runtime, SI_SUB_CPU, SI_ORDER_ANY, linux_rcu_runtime_init, NULL);

static void
linux_rcu_cleaner_func(void *context, int pending __unused)
{
	struct linux_epoch_head *head = context;
	struct callback_head *rcu;
	STAILQ_HEAD(, callback_head) tmp_head;
	struct llist_node *node, *next;
	uintptr_t offset;

	/* move current callbacks into own queue */
	STAILQ_INIT(&tmp_head);
	llist_for_each_safe(node, next, llist_del_all(&head->cb_head)) {
		rcu = container_of(node, struct callback_head, node);
		/* re-reverse list to restore chronological order */
		STAILQ_INSERT_HEAD(&tmp_head, rcu, entry);
	}

	/* one grace period for the whole batch */
	linux_synchronize_rcu(head - linux_epoch_head);

	/* dispatch all callbacks, if any */
	while ((rcu = STAILQ_FIRST(&tmp_head)) != NULL) {
		STAILQ_REMOVE_HEAD(&tmp_head, entry);

		offset = (uintptr_t)rcu->func;

		if (offset < LINUX_KFREE_RCU_OFFSET_MAX)
			kfree((char *)rcu - offset);
		else
			rcu->func((struct rcu_head *)rcu);
	}
}

void
linux_rcu_read_lock(unsigned type)
{
	struct nmx_rcu_gp *gp;
	struct task_struct *ts;
	unsigned p;

	MPASS(type < RCU_TYPE_MAX);

	if (RCU_SKIP())
		return;

	ts = current;
	MPASS(ts->rcu_recurse[type] != INT_MAX);
	if (++(ts->rcu_recurse[type]) != 1)
		return;

	gp = &nmx_gp[type];
	for (;;) {
		p = __atomic_load_n(&gp->phase, __ATOMIC_ACQUIRE) & 1;
		__atomic_fetch_add(&gp->readers[p], 1, __ATOMIC_SEQ_CST);
		/* a synchronize may have flipped the phase between the load and the count: then this reader would be
		   counted in a phase nobody waits on any more, so take it back and join the new phase instead */
		if ((__atomic_load_n(&gp->phase, __ATOMIC_SEQ_CST) & 1) == p)
			break;
		__atomic_fetch_sub(&gp->readers[p], 1, __ATOMIC_SEQ_CST);
	}
	ts->rcu_section[type] = p;
}

void
linux_rcu_read_unlock(unsigned type)
{
	struct task_struct *ts;

	MPASS(type < RCU_TYPE_MAX);

	if (RCU_SKIP())
		return;

	ts = current;
	MPASS(ts->rcu_recurse[type] > 0);
	if (--(ts->rcu_recurse[type]) != 0)
		return;

	/* the phase this section was counted in - the CPU it ends on does not matter */
	__atomic_fetch_sub(&nmx_gp[type].readers[ts->rcu_section[type] & 1], 1, __ATOMIC_SEQ_CST);
}

bool
linux_rcu_read_lock_held(unsigned type)
{
	MPASS(type < RCU_TYPE_MAX);

	if (RCU_SKIP())
		return (false);
	if (__current_unallocated(curthread))
		return (false);
	return (current->rcu_recurse[type] > 0);
}

void
linux_synchronize_rcu(unsigned type)
{
	struct nmx_rcu_gp *gp;
	unsigned old;
	unsigned long spins = 0;

	MPASS(type < RCU_TYPE_MAX);

	if (RCU_SKIP())
		return;

	WITNESS_WARN(WARN_GIANTOK | WARN_SLEEPOK, NULL,
	    "linux_synchronize_rcu() can sleep");

	gp = &nmx_gp[type];

	/* one grace period at a time: a second caller waits for the first, then runs its own */
	while (__atomic_exchange_n(&gp->sync_busy, 1, __ATOMIC_ACQUIRE) != 0)
		pause("rcusync", 1);

	old = __atomic_load_n(&gp->phase, __ATOMIC_SEQ_CST) & 1;
	__atomic_store_n(&gp->phase, old ^ 1, __ATOMIC_SEQ_CST);

	/* readers are short: spin briefly, then sleep a tick at a time */
	while (__atomic_load_n(&gp->readers[old], __ATOMIC_SEQ_CST) != 0) {
		if (++spins < 200)
			cpu_spinwait();
		else
			pause("rcugp", 1);
	}

	gp->grace_periods++;
	__atomic_store_n(&gp->sync_busy, 0, __ATOMIC_RELEASE);
}

void
linux_rcu_barrier(unsigned type)
{
	struct linux_epoch_head *head;

	MPASS(type < RCU_TYPE_MAX);

	/*
	 * This function is not obligated to wait for a grace period.
	 * It only waits for RCU callbacks that have already been posted.
	 */
	head = &linux_epoch_head[type];
	taskqueue_drain(linux_irq_work_tq, &head->task);
}

void
linux_call_rcu(unsigned type, struct rcu_head *context, rcu_callback_t func)
{
	struct callback_head *rcu;
	struct linux_epoch_head *head;

	MPASS(type < RCU_TYPE_MAX);

	rcu = (struct callback_head *)context;
	head = &linux_epoch_head[type];

	rcu->func = func;
	llist_add(&rcu->node, &head->cb_head);
	taskqueue_enqueue(linux_irq_work_tq, &head->task);
}

int
init_srcu_struct(struct srcu_struct *srcu)
{
	return (0);
}

void
cleanup_srcu_struct(struct srcu_struct *srcu)
{
}

int
srcu_read_lock(struct srcu_struct *srcu)
{
	linux_rcu_read_lock(RCU_TYPE_SLEEPABLE);
	return (0);
}

void
srcu_read_unlock(struct srcu_struct *srcu, int key __unused)
{
	linux_rcu_read_unlock(RCU_TYPE_SLEEPABLE);
}

void
synchronize_srcu(struct srcu_struct *srcu)
{
	linux_synchronize_rcu(RCU_TYPE_SLEEPABLE);
}

void
srcu_barrier(struct srcu_struct *srcu)
{
	linux_rcu_barrier(RCU_TYPE_SLEEPABLE);
}
