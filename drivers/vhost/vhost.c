/* Copyright (C) 2009 Red Hat, Inc.
 * Copyright (C) 2006 Rusty Russell IBM Corporation
 *
 * Author: Michael S. Tsirkin <mst@redhat.com>
 *
 * Inspiration, some code, and most witty comments come from
 * Documentation/virtual/lguest/lguest.c, by Rusty Russell
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 *
 * Generic code for virtio server in host kernel.
 */

#include <linux/eventfd.h>
#include <linux/vhost.h>
#include <linux/uio.h>
#include <linux/mm.h>
#include <linux/mmu_context.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/cgroup.h>
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/module.h>

#include "vhost.h"
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/cpuset.h>
#include <linux/moduleparam.h>
#define MODULE_NAME "vhost"

static int virtual_queue_default_min_poll_rate = 0;
module_param(virtual_queue_default_min_poll_rate, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(virtual_queue_default_min_poll_rate,
		"The minimum rate in which a polled queue can be polled (disable = 0) "
		"Note: This is designed for non-latency sensitive queues, when we want "
		"to avoid sending a lot of virtual interrupts to the guest.");

static int worker_work_list_default_max_stuck_cycles = -1;
module_param(worker_work_list_default_max_stuck_cycles, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(worker_work_list_default_max_stuck_cycles, "How many cycles "
		"need to elapse to consider the worker work_list stuck "
		"(-1 = disabled)");

static int virtual_queue_default_max_stuck_cycles = -1;
module_param(virtual_queue_default_max_stuck_cycles, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(virtual_queue_default_max_stuck_cycles, "The default number "
		"of cycles need to elapse in order to consider a queue as stuck "
		"(-1 = disabled)");

static int virtual_queue_default_max_stuck_pending_items = 0;
module_param(virtual_queue_default_max_stuck_pending_items, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(virtual_queue_default_max_pending_items, "The default "
		"maximum number of items pending in the queue in order to consider a "
		"queue as stuck (0 = disabled)");

static int worker_default_disable_soft_interrupts_cycles = 0;
module_param(worker_default_disable_soft_interrupts_cycles, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(worker_default_max_disabled_soft_interrupts_cycles ,
		"Number of cycles that a worker can disable software interrupts while "
		"processing virtio queues.");


static struct class *vhost_fs_class;
static struct device *vhost_fs_workers;
static struct device *vhost_fs_devices;
static struct device *vhost_fs_queues;

#define VHOST_FS_DIRECTORY_WORKER "worker"
#define VHOST_FS_DIRECTORY_DEVICE "dev"
#define VHOST_FS_DIRECTORY_VIRTUAL_QUEUE "vq"


#define ARRAY_LENGTH(x)  (sizeof(x) / sizeof(x[0]))

#define VHOST_FS_SHOW(func, value_type, str_format)							\
static ssize_t func(struct device *dev,	struct device_attribute *attr, 		\
		char *buf){															\
	struct dev_ext_attribute *ea;											\
	size_t offset;															\
	value_type val;															\
	long length;															\
	vhost_printk("VHOST_FS_SHOW: %s %s\n", dev_name(dev->parent), 			\
			dev_name(dev));													\
	ea = container_of(attr, struct dev_ext_attribute, attr);				\
	offset = (size_t)ea->var;												\
	val = *(value_type *)((char *)dev_get_drvdata(dev) + offset);			\
	length = snprintf(buf, PAGE_SIZE, str_format, val);						\
	vhost_printk("VHOST_FS_SHOW: done!\n");									\
	vhost_printk("page = %s length = %ld\n", buf, length);					\
	return length;															\
}

#define VHOST_FS_STORE(func, value_type, kstrto)							\
static ssize_t func(struct device *dev, 									\
		struct device_attribute *attr, const char *buf, size_t size){		\
	struct dev_ext_attribute *ea;											\
	size_t offset;															\
	value_type out;															\
	int err;																\
	vhost_printk("VHOST_FS_STORE: %s %s\n", dev_name(dev->parent), 			\
			dev_name(dev));													\
	ea = container_of(attr, struct dev_ext_attribute, attr);				\
	offset = (size_t)ea->var;												\
	if ((err = kstrto(buf, 0, &out)) != 0){									\
		return err;															\
	}																		\
	*(value_type *)((char *)dev_get_drvdata(dev) + offset) = out;			\
	vhost_printk("VHOST_FS_STORE done: return value is %lu\n", size);		\
	return size;															\
}

VHOST_FS_SHOW(vhost_fs_show_u64, u64, "%llu\n");
VHOST_FS_STORE(vhost_fs_store_u64, u64, kstrtoull);

#define VHOST_FS_QUEUE_STAT(x) (offsetof(struct vhost_virtqueue, x))
#define VHOST_FS_DEVICE_STAT(x) (offsetof(struct vhost_dev, x))
#define VHOST_FS_WORKER_STAT(x) (offsetof(struct vhost_worker, x))

#define VHOST_FS_WORKER_STAT_ATTR(name, field) \
{__ATTR(name, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP | S_IROTH,	\
		vhost_fs_show_u64, vhost_fs_store_u64), 							\
		(void *)(VHOST_FS_WORKER_STAT(field)) }
#define VHOST_FS_WORKER_STAT_READONLY_ATTR(name, field) \
{__ATTR(name, S_IRUSR | S_IRGRP | S_IROTH, vhost_fs_show_u64, NULL), 		\
		(void *)(VHOST_FS_WORKER_STAT(field)) }
#define VHOST_FS_DEVICE_STAT_ATTR(name, field) \
{__ATTR(name, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP | S_IROTH, 	\
		vhost_fs_show_u64, vhost_fs_store_u64), 							\
		(void *)(VHOST_FS_DEVICE_STAT(field)) }
#define VHOST_FS_DEVICE_STAT_READONLY_ATTR(name, field) \
{__ATTR(name, S_IRUSR | S_IRGRP | S_IROTH, vhost_fs_show_u64, NULL,		\
		(void *)(VHOST_FS_DEVICE_STAT(field)) }
#define VHOST_FS_QUEUE_STAT_ATTR(name, field) \
{__ATTR(name, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP | S_IROTH, 	\
		vhost_fs_show_u64, vhost_fs_store_u64), 							\
		(void *)(VHOST_FS_QUEUE_STAT(field)) }
#define VHOST_FS_QUEUE_STAT_READONLY_ATTR(name, field) \
{__ATTR(name, S_IRUSR | S_IRGRP | S_IROTH, vhost_fs_show_u64, NULL), 		\
		(void *)(VHOST_FS_QUEUE_STAT(field)) }

#define DECLARE_VHOST_FS_SHOW(func) \
	static ssize_t func(struct device *dev, struct device_attribute *attr, \
			char *buf)

#define DECLARE_VHOST_FS_STORE(func) \
	static ssize_t func(struct device *dev, struct device_attribute *attr, \
		 const char *buf, size_t count)

static ssize_t vhost_fs_get_epoch(struct class *class,
		struct class_attribute *attr, char *buf);
static ssize_t vhost_fs_inc_epoch(struct class *class,
		struct class_attribute *attr, const char *buf, size_t count);
static ssize_t vhost_fs_get_cycles(struct class *class,
		struct class_attribute *attr, char *buf);
//static ssize_t vhost_fs_status(struct class *class,
//		struct class_attribute *attr, char *buf);

/* global attributes */
static struct class_attribute vhost_fs_global_attrs[] = {
	/* Writing advances the epoch counter by 1.
	 * Reading returns the current epoch. */
	__ATTR(epoch, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP | S_IROTH,
			vhost_fs_get_epoch, vhost_fs_inc_epoch),
	/* Reading returns the number of cycles elapsed during the last epoch of
	 * vhost. */
	__ATTR(cycles, S_IRUSR | S_IRGRP| S_IROTH,
			vhost_fs_get_cycles, NULL),
//	/* Reading returns the status of vhost. */
//	__ATTR(status, S_IRUSR | S_IRGRP| S_IROTH, vhost_fs_status, NULL)
};

DECLARE_VHOST_FS_SHOW(vhost_fs_get_recent_new_workers);
DECLARE_VHOST_FS_STORE(vhost_fs_create_new_worker);
DECLARE_VHOST_FS_STORE(vhost_fs_remove_worker);
DECLARE_VHOST_FS_SHOW(vhost_fs_get_default_worker);
DECLARE_VHOST_FS_STORE(vhost_fs_set_default_worker);

/* global worker attributes */
static struct dev_ext_attribute vhost_fs_global_worker_attrs[] = {
	/* Writing a cpu mask creates a new worker with affinity to the requested
	 * cpu mask, Write 0 to specify no affinity.
	 * Reading returns the array of newly created workers id since the
	 * last time this file was read (upto VHOST_WORKERS_POOL_NEW_WORKERS_SIZE
	 * workers). */
	{__ATTR(create, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP | S_IROTH,
			vhost_fs_get_recent_new_workers, vhost_fs_create_new_worker), NULL},
	/* Writing a worker id removes a worker with the same id. The worker is
	 * removed only if locked and have no queues assigned to it. */
	{__ATTR(remove, S_IWUSR | S_IWGRP, NULL, vhost_fs_remove_worker),
			NULL},
	/* Writing a worker id set the worker with the same id as the designated
	 * worker new devices are assigned to. Worker must be unlocked.
	 * Reading returns the current value. */
	{__ATTR(default, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP | S_IROTH,
			vhost_fs_get_default_worker, vhost_fs_set_default_worker), NULL}
};

DECLARE_VHOST_FS_SHOW(vhost_fs_worker_get_locked);
DECLARE_VHOST_FS_STORE(vhost_fs_worker_set_locked);
DECLARE_VHOST_FS_SHOW(vhost_fs_worker_get_cpu);
DECLARE_VHOST_FS_SHOW(vhost_fs_worker_get_pid);
DECLARE_VHOST_FS_SHOW(vhost_fs_worker_get_dev_list);

/* per worker attributes */
static struct dev_ext_attribute vhost_fs_per_worker_attrs[] = {
	/* Reading returns the number of loops performed by the worker. */
	VHOST_FS_WORKER_STAT_READONLY_ATTR(loops, stats.loops),
	/* Reading returns the number of times interrupts were re-enabled by
	 * the worker. */
	VHOST_FS_WORKER_STAT_READONLY_ATTR(enabled_interrupts,
			stats.enabled_interrupts),
	/* Reading returns the cycles spent in the worker, excluding cycles
	 * doing queue work. */
	VHOST_FS_WORKER_STAT_READONLY_ATTR(cycles, stats.cycles),
	/* Reading returns the number of times the mm was switched. */
	VHOST_FS_WORKER_STAT_READONLY_ATTR(mm_switches, stats.mm_switches),
	/* number of cycles the worker thread was not running after schedule. */
	VHOST_FS_WORKER_STAT_READONLY_ATTR(wait, stats.wait),
	/* Reading returns the number of times there were no works in the
	 * queue -- ignoring poll kicks. */
	VHOST_FS_WORKER_STAT_READONLY_ATTR(empty_works, stats.empty_works),
	/* Reading returns the number of times there were no queues to poll
	 * and the polling queue was not empty. */
	VHOST_FS_WORKER_STAT_READONLY_ATTR(empty_polls, stats.empty_polls),
	/* Reading returns the number of times were detected stuck and limited
	 * queues. */
	VHOST_FS_WORKER_STAT_READONLY_ATTR(stuck_works, stats.stuck_works),
	/* Reading returns the number of works which have no queue related to
	 * them (e.g. vhost-net rx). */
	VHOST_FS_WORKER_STAT_READONLY_ATTR(pending_works, stats.pending_works),
	/* Reading returns the number of pending works. */
	VHOST_FS_WORKER_STAT_READONLY_ATTR(noqueue_works, stats.noqueue_works),
	/* Writing a bool sets worker as locked/unlocked, when a worker is locked
	 * it cannot be assigned queues.
	 * Reading returns 1 if device is locked, 0 otherwise.
	 * Note: once a device has been locked it cannot be unlocked. */
	{__ATTR(locked, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP | S_IROTH,
			vhost_fs_worker_get_locked, vhost_fs_worker_set_locked), NULL},
	/* Writing an int the maximum number of cycles need to elapse to consider
	 * the work_list stuck (disabled = -1).
	 * Reading return the current value. */
	VHOST_FS_WORKER_STAT_READONLY_ATTR(work_list_max_stuck_cycles,
			work_list_max_stuck_cycles),
	/* Writing an int the maximum number of cycles a worker is allowed
	 * to turn off soft interrupts while servicing poll queues.
	 * Reading return the current value. */
	VHOST_FS_WORKER_STAT_READONLY_ATTR(max_disabled_soft_interrupts_cycles,
			max_disabled_soft_interrupts_cycles),
	/* Reading returns the core affinity of the worker. */
	{__ATTR(cpu, S_IRUSR| S_IRGRP | S_IROTH,
			vhost_fs_worker_get_cpu, NULL), NULL},
	/* Reading returns the process id of the worker. */
	{__ATTR(pid, S_IRUSR| S_IRGRP | S_IROTH, vhost_fs_worker_get_pid, NULL), NULL},
	 /* Reading returns a '\n' separated list of ids of devices assigned to
	  * this worker. */
	{__ATTR(dev_list, S_IRUSR | S_IRGRP | S_IROTH, vhost_fs_worker_get_dev_list, NULL), NULL}
};

DECLARE_VHOST_FS_SHOW(vhost_fs_device_get_worker);
DECLARE_VHOST_FS_STORE(vhost_fs_device_set_worker);
DECLARE_VHOST_FS_SHOW(vhost_fs_device_get_owner);
DECLARE_VHOST_FS_SHOW(vhost_fs_device_get_vq_list);

/* device attributes */
static struct dev_ext_attribute vhost_fs_device_attrs[] = {
	/* Writing a worker id transfers a device from its current worker to the
	 * worker with the written id. The receiving worker cannot be a locked worker.
	 * Reading returns the current value. */
	{__ATTR(worker, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP | S_IROTH,
			vhost_fs_device_get_worker, vhost_fs_device_set_worker), NULL},
	 /* Reading returns the pid of the owner thread. */
	{__ATTR(owner, S_IRUSR | S_IRGRP | S_IROTH, vhost_fs_device_get_owner, NULL), NULL},
	 /* Reading returns a '\n' separated list of ids of queues contained in the
	  * device. */
	{__ATTR(vq_list, S_IRUSR | S_IRGRP | S_IROTH, vhost_fs_device_get_vq_list, NULL), NULL}
};

DECLARE_VHOST_FS_SHOW(vhost_fs_queue_get_poll);
DECLARE_VHOST_FS_STORE(vhost_fs_queue_set_poll);
DECLARE_VHOST_FS_SHOW(vhost_fs_queue_get_device);

/* queue attributes */
static struct dev_ext_attribute vhost_fs_queue_attrs[] = {
	/* Reading returns the number of cycles spent handling kicks in poll mode. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(poll_cycles, stats.poll_cycles),
	/* Reading returns the number of bytes sent/received by kicks in poll mode. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(poll_bytes, stats.poll_bytes),
	/* Reading returns the number of cycles elapsed between poll kicks. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(poll_wait, stats.poll_wait),
	/* Reading returns the number of times the queue was empty during poll. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(poll_empty, stats.poll_empty),
	/* Reading returns the number of cycles elapsed while the queue was empty. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(poll_empty_cycles, stats.poll_empty_cycles),
	/* Reading returns the number of times this queue was coalesced. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(poll_coalesced, stats.poll_coalesced),
	/* Reading returns the number of times the queue was limited by netweight
	 * during poll kicks. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(poll_limited, stats.poll_limited),
	/* Reading returns the total amount of cycles all the work items in the ring
	 * buffer waited for service. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(poll_aggregated_wait_cycles,
									  stats.poll_aggregated_wait_cycles),


	/* Reading returns the number of works in notif mode. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(notif_works, stats.notif_works),
	/* Reading returns the number of cycles spent handling works in notif mode. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(notif_cycles, stats.notif_cycles),
	/* Reading returns the number of bytes sent/received by works in notif mode. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(notif_bytes, stats.notif_bytes),
	/* Reading returns the number of cycles elapsed between works in notif mode. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(notif_wait, stats.notif_wait),
	/* Reading returns the number of times the queue was limited by netweight in
	 * notif mode */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(notif_limited, stats.notif_limited),

	/* Reading returns the number of times the ring was full */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(ring_full, stats.ring_full),

	/* Reading returns the number of times this queue was stuck and limited
	 * other queues */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(stuck_times, stats.stuck_times),
	/* Reading returns the number of amount of cycles the queue was stuck */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(stuck_cycles, stats.stuck_cycles),

	/* Reading returns the number of bytes handled by this queue in the last
	 * poll/notif. Must be updated by the concrete vhost implementations
	 * (vhost-net)*/
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(handled_bytes, stats.handled_bytes),

#if 1 /* patchouli vhost-pollmode */
	/* Writing starts/stops polling of virtual queue.
	 * Reading returns the current value. */
	{__ATTR(poll, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP | S_IROTH,
			vhost_fs_queue_get_poll, vhost_fs_queue_set_poll), NULL},
	/* Writing sets the minimum amount of bytes to be processed in a single
	 * service cycle of virtual queue (disable = 0).
	 * Reading returns the current value. */
	VHOST_FS_QUEUE_STAT_ATTR(min_processed_data_limit, min_processed_data_limit),
	/* Writing sets the maximum amount of bytes to be processed in a single
	 * service cycle of virtual queue (disable = -1).
	 * Reading returns the current value. */
	VHOST_FS_QUEUE_STAT_ATTR(max_processed_data_limit, max_processed_data_limit),
	/* Writing sets the maximum number of pending items in the queue to
	 * consider the queue stuck (disable = -1).
	 * Reading returns the current value. */
	VHOST_FS_QUEUE_STAT_ATTR(max_stuck_pending_items, vqpoll.max_stuck_pending_items),
	/* Writing sets the number of need to elapse without service to consider
	 * the queue stuck (disable = -1).
	 * Reading returns the current value. */
	VHOST_FS_QUEUE_STAT_ATTR(max_stuck_cycles, vqpoll.max_stuck_cycles),
	/* Reading returns the end of the last polling in cycles. */
	VHOST_FS_QUEUE_STAT_ATTR(last_poll_cycles, stats.last_poll_tsc_end),

	/* Writing sets the minimum rate in which a polled queue can be polled
	 * (disable = 0).
	 * Reading returns the current value. */
	VHOST_FS_QUEUE_STAT_READONLY_ATTR(min_poll_rate, vqpoll.min_poll_rate),
#endif

	/* Reading returns the id of the device this queue is contained in. */
	{__ATTR(dev, S_IRUSR | S_IRGRP | S_IROTH, vhost_fs_queue_get_device, NULL),
	NULL},
};


/* The worker states */
enum {
	/* The worker can be assigned new devices, and the system can transfer
	 * devices to it. */
	VHOST_WORKER_STATE_NORMAL,
	/* The worker cannot be assigned new unassigned devices, and devices cannot
	 * be transfered to it. The aim of this state is to allow for a user-space
	 * program to remove existing workers by first marking them as a LOCKED
	 * worker, then transferring all the devices assigned to it to the other
	 * workers. The last step in removing the worker, after all devices were
	 * transfered away is to is to send it a remove control.
	 */
	VHOST_WORKER_STATE_LOCKED,
	/* The worker is about to be shutdown. In order to shutdown a worker it
	 * must first be locked, and has no devices assigned to it. The worker
	 * thread will shutdown once all the works in its work_list are done.
	 */
	VHOST_WORKER_STATE_SHUTDOWN
};

/* The device operation mode */
enum {
	/* The device can receive new work. and the worker assigned to it can
	 * process them. At this state the device CANNOT be transfered safely.
	 */
	VHOST_DEVICE_OPERATION_MODE_NORMAL,
	/* Device operation mode during transfer. Arriving new works are attached to
	 * the device's suspended_work_list to be later processed by the new worker.
	 * The old worker processes any existing work in its work_list and keeps
	 * polling the devices virtual queues until it comes across a "detach device
	 * work" that removes the virtual queues of the device from the old worker's
	 * vqpoll_list. The last step in the transfer work is to queue an "attach
	 * device work" in the new worker. The new worker then changes the worker
	 * pointer of the device. Adds all polled queues to its vqpoll_list. Added
	 * all suspended work to its work_list. finally the new worker set the
	 * operation mode of the device back to normal.
	 */
	VHOST_DEVICE_OPERATION_MODE_TRANSFERRING
};

enum {
	VHOST_MEMORY_MAX_NREGIONS = 64,
	VHOST_MEMORY_F_LOG = 0x1,
};

static struct vhost_workers_pool workers_pool;

static struct vhost_dev_table dev_table;

static atomic_t epoch;
static u64 cycles_start_tsc;

#define vhost_used_event(vq) ((u16 __user *)&vq->avail->ring[vq->num])
#define vhost_avail_event(vq) ((u16 __user *)&vq->used->ring[vq->num])

static void vhost_poll_func(struct file *file, wait_queue_head_t *wqh,
			    poll_table *pt)
{
	struct vhost_poll *poll;

	poll = container_of(pt, struct vhost_poll, table);
	poll->wqh = wqh;
	add_wait_queue(wqh, &poll->wait);
}

static int vhost_poll_wakeup(wait_queue_t *wait, unsigned mode, int sync,
			     void *key)
{
	struct vhost_poll *poll = container_of(wait, struct vhost_poll, wait);

	if (!((unsigned long)key & poll->mask))
		return 0;

	vhost_poll_queue(poll);
	return 0;
}

/*
 * Initialize a vhost work with @fn as the work function.
 * @work [out] the initialized work.
 * @vq [in] the virtual device that ownes this work item.
 * @fn [in] the work item function.
 */
void vhost_work_init(struct vhost_work *work, struct vhost_virtqueue *vq, vhost_work_fn_t fn)
{
	INIT_LIST_HEAD(&work->node);
	work->fn = fn;
	init_waitqueue_head(&work->done);
	work->flushing = 0;
	work->queue_seq = work->done_seq = 0;
	work->vq = vq;
	spin_lock_init(&work->lock);
}
EXPORT_SYMBOL_GPL(vhost_work_init);

/* Init poll structure */
void vhost_poll_init(struct vhost_poll *poll, vhost_work_fn_t fn,
		     unsigned long mask, struct vhost_virtqueue *vq)
{
	init_waitqueue_func_entry(&poll->wait, vhost_poll_wakeup);
	init_poll_funcptr(&poll->table, vhost_poll_func);
	poll->mask = mask;
	poll->dev = vq->dev;

	vhost_work_init(&poll->work, vq, fn);
}
EXPORT_SYMBOL_GPL(vhost_poll_init);

/* Start polling a file. We add ourselves to file's wait queue. The caller must
 * keep a reference to a file until after vhost_poll_stop is called. */
int vhost_poll_start(struct vhost_poll *poll, struct file *file)
{
	unsigned long mask;
	int ret = 0;

	if (poll->wqh)
		return 0;

	mask = file->f_op->poll(file, &poll->table);
	if (mask)
		vhost_poll_wakeup(&poll->wait, 0, 0, (void *)mask);
	if (mask & POLLERR) {
		if (poll->wqh)
			remove_wait_queue(poll->wqh, &poll->wait);
		ret = -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(vhost_poll_start);

/* Stop polling a file. After this function returns, it becomes safe to drop the
 * file reference. You must also flush afterwards. */
void vhost_poll_stop(struct vhost_poll *poll)
{
	if (poll->wqh) {
		remove_wait_queue(poll->wqh, &poll->wait);
		poll->wqh = NULL;
	}
}
EXPORT_SYMBOL_GPL(vhost_poll_stop);

/*
 * @dev [in] the device holding the work.
 * @work [in] the work that is checked.
 * @return 1 if the work is no queued or it is done, 0 otherwise.
 */
static bool vhost_work_seq_done(struct vhost_work *work, unsigned seq){
	int left;
	spin_lock_irq(&work->lock);
	left = seq - work->done_seq;
	spin_unlock_irq(&work->lock);
	return left <= 0;
}

/*
 * Wait until @work is done. The function returns after the work has been done.
 * @worker [in] the worker that performs the work.
 * @work [in] the work that is checked.
 */
static void vhost_worker_flush_work(struct vhost_work *work){
	unsigned seq;
	int flushing;

	spin_lock_irq(&work->lock);
	seq = work->queue_seq;
	work->flushing++;
	spin_unlock_irq(&work->lock);

	wait_event(work->done, vhost_work_seq_done(work, seq));

	spin_lock_irq(&work->lock);
	flushing = --work->flushing;
	spin_unlock_irq(&work->lock);
	BUG_ON(flushing < 0);
}
/*
 * Wait until @work is done. The function returns after the work has been done.
 * @dev [in] the device holding the work.
 * @work [in] the work that is checked.
 */
void vhost_work_flush(struct vhost_dev *dev, struct vhost_work *work)
{
	vhost_worker_flush_work(work);
}
EXPORT_SYMBOL_GPL(vhost_work_flush);

/* Flush any work that has been scheduled. When calling this, don't hold any
 * locks that are also used by the callback. */
void vhost_poll_flush(struct vhost_poll *poll)
{
	vhost_work_flush(poll->dev, &poll->work);
}
EXPORT_SYMBOL_GPL(vhost_poll_flush);
/*
 * Add @work to the work list of the @worker thread.
 * @worker [in] the worker which the work is assigned to.
 * @work [in] the work to be enqueued.
 *
 * Note: This function circumvent the checks regarding the device state. If the
 * device this originates from can be transfered DO NOT use this function.
 */
static void vhost_work_force_enqueue(struct vhost_worker* worker,
		struct vhost_work *work){
	unsigned long flags;
	spin_lock_irqsave(&worker->work_lock, flags);
	spin_lock(&work->lock);
	/* check that the work was not added to any work list of any worker thread. */
	if (list_empty(&work->node)) {
		list_add_tail(&work->node, &worker->work_list);
		worker->stats.pending_works++;
		work->queue_seq++;
		wake_up_process(worker->worker_thread);
	}
	spin_unlock(&work->lock);
	spin_unlock_irqrestore(&worker->work_lock, flags);
}
/*
 * Add @work to the work list of the worker thread in-charge of the device @dev.
 * @dev [in] the device to which the work is assigned to.
 * @work [in] the work to be enqueued. *
 */
void vhost_work_queue(struct vhost_dev *dev, struct vhost_work *work)
{
	unsigned long flags;
//	vhost_printk("attempting to add work %p into device %d.", work, dev->id);

	if (unlikely(atomic_read(&dev->transfer.operation_mode) ==
			VHOST_DEVICE_OPERATION_MODE_TRANSFERRING)){
		spin_lock_irqsave(&dev->transfer.suspended_work_lock, flags);
		spin_lock(&work->lock);
		/* the device is in transfer, add work to device's suspended list. */
		if (list_empty(&work->node)) {
			/* At this point we should make sure that the device is still
			 * Transferring before we can add the work to the suspended work
			 * list */
			if (unlikely(atomic_read(&dev->transfer.operation_mode) ==
						VHOST_DEVICE_OPERATION_MODE_NORMAL)){
				spin_unlock(&work->lock);
				spin_unlock_irqrestore(&dev->transfer.suspended_work_lock, flags);
				vhost_work_force_enqueue(dev->worker, work);
				return;
			}

			list_add_tail(&work->node, &dev->transfer.suspended_work_list);
			++dev->transfer.suspended_works;
			++work->queue_seq;
		}
		spin_unlock(&work->lock);
		spin_unlock_irqrestore(&dev->transfer.suspended_work_lock, flags);
//		vhost_printk("work %p for device %d was added to suspended list.",
//				work, dev->id);
		return;
	}
//	vhost_printk("trying to add work %p for device %d to worker %d work_list.",
//					  work, dev->id, dev->worker->id);
	vhost_work_force_enqueue(dev->worker, work);
//	vhost_printk("work %p for device %d was successfully added to work list of worker %d.",
//				work, dev->id, dev->worker->id);
}
EXPORT_SYMBOL_GPL(vhost_work_queue);

/*
 * Enqueue the work contained in @poll
 * @poll a vhost poll request.
 */
void vhost_poll_queue(struct vhost_poll *poll)
{
	vhost_work_queue(poll->dev, &poll->work);
}
EXPORT_SYMBOL_GPL(vhost_poll_queue);

static atomic_t last_vqid = ATOMIC_INIT(0);
#define  printk_vq(s, vq) 									\
	do { 													\
		 int devid = vq->dev->id; 							\
		 int vqid = vq->id;									\
		 int workerid = vq->dev->worker->id;				\
		 vhost_printk("%s on virtqueue %d/%d (worker %d)\n", \
				     s, devid, vqid, workerid); 			\
	}while(0);

/* Enable or disable virtqueue polling (vqpoll.enabled) for a virtqueue.
 *
 * Enabling this mode it tells the guest not to notify ("kick") us when its
 * has made more work available on this virtqueue; Rather, we will continuously
 * poll this virtqueue in the worker thread. If multiple virtqueues are polled,
 * the worker thread polls them all, e.g., in a round-robin fashion.
 * Note that vqpoll.enabled doesn't always mean that this virtqueue is
 * actually being polled: The backend (e.g., net.c) may temporarily disable it
 * using vhost_disable/enable_notify(), while vqpoll.enabled is unchanged.
 *
 * It is assumed that these functions are called relatively rarely, when vhost
 * notices that this virtqueue's usage pattern significantly changed in a way
 * that makes polling more efficient than notification, or vice versa.
 * Also, we assume that vhost_vq_disable_vqpoll() is always called on vq
 * cleanup, so any allocations done by vhost_vq_enable_vqpoll() can be
 * reclaimed.
 *
 * Note: Only call this function from the context of the worker handling the vq
 * because this call is not atomic or synchronized or checks that the correct mm
 * is loaded. use vhost_vq_enable_vqpoll instead.
 */
static void vhost_vq_enable_vqpoll_unsafe(struct vhost_virtqueue *vq){
	printk_vq("Enabling vqpoll - START", vq);
	if (vq->vqpoll.enabled)
		return; /* already enabled, nothing to do */
	if (!vq->handle_kick)
		return; /* polling will be a waste of time if no callback! */
	if (!(vq->used_flags & VRING_USED_F_NO_NOTIFY)) {
		/* vq has guest notifications enabled. Disable them,
		   and instead add vq to the polling list */
		vhost_disable_notify(vq->dev, vq);
		list_add_tail(&vq->vqpoll.link, &vq->dev->worker->vqpoll_list);
	}
	__get_user(vq->avail_idx, &vq->avail->idx); // TODO: do we need to do this? also,  if we do, why not just use the mapped below?

	vq->vqpoll.enabled = true;

	/* Map userspace's vq->avail to the kernel's memory space, so it
	 * can be polled without slow switching of page tables. This is
	 * important so that inactive guests still on the polling list
	 * do not slow down an active guest.
	 */
	if (get_user_pages_fast((unsigned long)vq->avail, 1, 0,
		&vq->vqpoll.avail_page) != 1) {
		// TODO: I think this can't happen because we check access
		// to vq->avail in advance, right??
		BUG();
	}
	vq->vqpoll.avail_mapped = (struct vring_avail *) (
		(unsigned long)kmap(vq->vqpoll.avail_page) |
		((unsigned long)vq->avail & ~PAGE_MASK));

	printk_vq("Enabling vqpoll - DONE.", vq);
}

/*
 * This function doesn't always succeed in changing the mode. Sometimes
 * a temporary race condition prevents turning on guest notifications, so
 * vq should be polled next time again.
 *
 * Note: only call this function from the context of the worker handling the vq
 * because this call is not atomic or synchronized or checks that the correct mm
 * is loaded. use vhost_vq_disable_vqpoll instead.
 */
static void vhost_vq_disable_vqpoll_unsafe(struct vhost_virtqueue *vq){
	printk_vq("Disabling vqpoll", vq);
	if (!vq->vqpoll.enabled) {
		return; /* already disabled, nothing to do */
	}
	vq->vqpoll.enabled = false;
	/* stop measuring cycles the queue was empty while polling */
	vq->stats.last_poll_empty_tsc = 0;

	if (!list_empty(&vq->vqpoll.link)) {
		/* vq is on the polling list, remove it from this list and
		 * instead enable guest notifications. */
		list_del_init(&vq->vqpoll.link);
		if (unlikely(vhost_enable_notify(vq->dev,vq))
			&& !vq->vqpoll.shutdown) {
			/* Race condition: guest wrote before we enabled
			 * notification, so we'll never get a notification for
			 * this work - so continue polling mode for a while. */
			vhost_disable_notify(vq->dev, vq);
			vq->vqpoll.enabled = true;
			vhost_enable_notify(vq->dev, vq);
			return;
		}
	}

	if (vq->vqpoll.avail_mapped) {
		kunmap(vq->vqpoll.avail_page);
		put_page(vq->vqpoll.avail_page);
		vq->vqpoll.avail_mapped = 0;
	}
}

static void vhost_vq_reset(struct vhost_dev *dev,
			   struct vhost_virtqueue *vq)
{
	vq->num = 1;
	vq->desc = NULL;
	vq->avail = NULL;
	vq->used = NULL;
	vq->last_avail_idx = 0;
	vq->avail_idx = 0;
	vq->last_used_idx = 0;
	vq->signalled_used = 0;
	vq->signalled_used_valid = false;
	vq->used_flags = 0;
	vq->log_used = false;
	vq->log_addr = -1ull;
	vq->private_data = NULL;
	vq->acked_features = 0;
	vq->log_base = NULL;
	vq->error_ctx = NULL;
	vq->error = NULL;
	vq->kick = NULL;
	vq->call_ctx = NULL;
	vq->call = NULL;
	vq->log_ctx = NULL;
	vq->memory = NULL;
	INIT_LIST_HEAD(&vq->vqpoll.link);
	vq->vqpoll.enabled = false;
	vq->vqpoll.shutdown = false;
	vq->vqpoll.avail_mapped = NULL;
	vq->vqpoll.max_stuck_cycles = virtual_queue_default_max_stuck_cycles;
	vq->vqpoll.max_stuck_pending_items = virtual_queue_default_max_stuck_pending_items;
	vq->min_processed_data_limit = 1500;
	vq->max_processed_data_limit = 524288;
	vq->vqpoll.min_poll_rate = virtual_queue_default_min_poll_rate;
}

/* Switch the current kernel thread's mm context (page tables) to the given
 * one, if necessary (if it's not already using this mm context).
 */
static inline void set_mm(struct vhost_virtqueue *vq) {
	struct mm_struct *mm = vq->dev->mm;
	if (current->mm != mm) {
		use_mm(mm);
		vq->dev->worker->stats.mm_switches++;
	}
}

/* roundrobin_poll() takes worker->vqpoll_list, and returns one of the
 * virtqueues which the caller should kick, or NULL in case none should be
 * kicked. roundrobin_poll() also disables polling on a virtqueue which has
 * been polled for too long without success.
 *
 * This current implementation (the "round-robin" implementation) only
 * polls the first vq in the list, returning it or NULL as appropriate, and
 * moves this vq to the end of the list, so next time a different one is
 * polled.
 */
static struct vhost_virtqueue *roundrobin_poll(struct list_head *list) {
	struct vhost_virtqueue *vq;
	u16 avail_idx;

	if (list_empty(list)){
		return NULL;
	}

	vq = list_first_entry(list, struct vhost_virtqueue, vqpoll.link);
	WARN_ON(!vq->vqpoll.enabled);
	list_move_tail(&vq->vqpoll.link, list);
	WARN_ON(list_empty(list));

	/* If poll_coalescing_rate is set, avoid kicking the same vq too often */
	if (vq->vqpoll.min_poll_rate > 0) {
		u64 tsc;
		rdtscll(tsc);
		if (vq->vqpoll.min_poll_rate < tsc - vq->stats.last_poll_tsc_end){
			vq->stats.poll_coalesced++;
			return NULL;
		}
	}
	/* See if there is any new work available from the guest. */
	/* TODO: need to check the optional idx feature, and if we haven't
	 * reached that idx yet, don't kick... */
	avail_idx = vq->vqpoll.avail_mapped->idx;
	if (avail_idx != vq->last_avail_idx) {
		/* if the list was empty in previous polls calculated
		   the cycles elapsed until we detected work */
		if (vq->stats.last_poll_empty_tsc != 0) {
			u64 tsc;
			rdtscll(tsc);
			vq->stats.poll_empty_cycles+=(tsc-vq->stats.last_poll_empty_tsc);
			vq->stats.last_poll_empty_tsc = 0;
		}
		return vq;
	}
	vq->stats.poll_empty++;
	/* Remember the first time the queue was empty to measure the amount
	   of cycles elapsed until we detect work */
	if (vq->stats.last_poll_empty_tsc == 0)
		rdtscll(vq->stats.last_poll_empty_tsc);

	return NULL;    
}

/*
 * Check if any of the queues has pending works and no new items has been
 * added for specific amount of time. This probably means that the queue is stuck
 * and the pending items need to be processed ASAP to release it so the VM can continue
 * processing data. If no stuck queues were found, continue as far as the amount of data
 * processed is less than the specified limit.
 */
bool vhost_can_continue(struct vhost_virtqueue  *vq,
		size_t processed_data) {
	struct vhost_virtqueue *vq_iterator, *next = NULL;
	struct vhost_worker *worker = vq->dev->worker;
	struct list_head *list = &vq->dev->worker->vqpoll_list;
	u64 elapsed_cycles;
	u64 cycles;

	// if we didn't process the minimum amount of data we can always continue
	if (processed_data < vq->min_processed_data_limit)
			vq->stats.was_limited = 1;
		return true;

	// If we processed more than the maximum we can not continue
	if (processed_data > vq->max_processed_data_limit)
		goto cannot_continue;

	rdtscll(cycles);
	elapsed_cycles = cycles - vq->dev->worker->last_work_tsc;
	// if there are work items pending for too long we can not continue
	if (worker->work_list_max_stuck_cycles >= 0 &&
			elapsed_cycles > worker->work_list_max_stuck_cycles &&
			!list_empty(&vq->dev->worker->work_list)) {
		goto cannot_continue;
	}

	if (vq->vqpoll.max_stuck_cycles < 0) {
		// stuck queues option is turned off
		return true;
	}
	rdtscll(cycles);
	// check if there are stuck queues
	list_for_each_entry_safe(vq_iterator, next, list, vqpoll.link) {
		u16 pending_items;

		// ignore the queue that is currently being processed
		if (vq_iterator == vq) {
			vq_iterator->vqpoll.last_pending_items = 0;
			continue;
		}

		// ignore queues that has no pending data
		pending_items = vq_iterator->vqpoll.avail_mapped->idx - vq_iterator->last_avail_idx;
		if (pending_items == 0) {
			vq_iterator->vqpoll.last_pending_items = 0;
			continue;
		}

		// check if the queue stuck with pending data since the last check (?)
		if (pending_items != vq_iterator->vqpoll.last_pending_items) {
			// the queue is not stuck, reset stuck
			vq_iterator->vqpoll.last_pending_items = pending_items;
			vq_iterator->vqpoll.stuck_cycles = cycles;
			continue;
		}

		// stuck sizes is used to avoid detecting a bursty queue as a stuck queue
		// (don't consider a queue stuck if it holds too many items = max_queue_stuck_size)
		if (vq_iterator->vqpoll.max_stuck_pending_items >  0 &&
				pending_items > vq_iterator->vqpoll.max_stuck_pending_items)
			continue;

		elapsed_cycles = cycles - vq_iterator->vqpoll.stuck_cycles;
		// is the queue stuck for too long ?
		if (elapsed_cycles >= vq_iterator->vqpoll.max_stuck_cycles) {
			// put current queue in the 2nd place if it didn't send more than half of the max
			// and it's being polled
			if (vq->vqpoll.enabled && processed_data < vq->max_processed_data_limit / 2)
				list_move(&vq->vqpoll.link, list);

			// put stuck queue in the 1st place if it's being polled
			list_move(&vq_iterator->vqpoll.link, list);
			vq_iterator->stats.stuck_times++;
			vq_iterator->stats.stuck_cycles+=elapsed_cycles;
			goto cannot_continue;
		}
	}

	// no stuck queues, no works, no maximum  => we can continue
	return true;

cannot_continue:
	vq->stats.was_limited = 1;
	return false;
}
EXPORT_SYMBOL_GPL(vhost_can_continue);

static int vhost_worker_thread(void *data)
{
	u64 bh_disabled_tsc = 0;
	struct vhost_worker *worker = data;
//	int has_poll = !list_empty(&worker->vqpoll_list);
	struct vhost_work *work = NULL;
	unsigned uninitialized_var(seq);
	mm_segment_t oldfs = get_fs();

	set_fs(USER_DS);
	if (worker->max_disabled_soft_interrupts_cycles) {
		rdtscll(bh_disabled_tsc);
		local_bh_disable();
	}

	for (;;) {
		u64 loop_start_tsc, loop_end_tsc,
		poll_start_tsc = 0, poll_end_tsc = 0,
		work_start_tsc = 0, work_end_tsc = 0;
		rdtscll(loop_start_tsc);
		/* mb paired w/ kthread_stop */
		set_current_state(TASK_INTERRUPTIBLE);

		if (work) {
			spin_lock_irq(&work->lock);
			work->done_seq = seq;
			if (work->flushing)
				wake_up_all(&work->done);
			spin_unlock_irq(&work->lock);
		}               

//		if (kthread_should_stop()) {
//			spin_unlock_irq(&worker->work_lock);
//			__set_current_state(TASK_RUNNING);
//			break;
//		}
		spin_lock_bh(&worker->work_lock);
		if (!list_empty(&worker->work_list)) {
			work = list_first_entry(&worker->work_list, struct vhost_work, node);
			spin_lock_irq(&work->lock);
			list_del_init(&work->node);
			worker->stats.pending_works--;
			seq = work->queue_seq;
			spin_unlock_irq(&work->lock);
		} else if (unlikely(atomic_read(&worker->state) ==
				VHOST_WORKER_STATE_SHUTDOWN)) {
			/* The work list is empty and worker state is shutdown so we shut
			 * the thread. We shutdown here because it is after all the threads
			 * waiting on the shutdown work where informed that the shutdown
			 * work ended. */
			spin_unlock_bh(&worker->work_lock);
			vhost_printk("worker %d stopping\n.", worker->id);
			return 1;
		} else {
			work = NULL;
		}
		spin_unlock_bh(&worker->work_lock);
		if (work) {
			struct vhost_virtqueue *vq = work->vq;
			__set_current_state(TASK_RUNNING);
			if (vq) {
				set_mm(vq);
				if (vq->avail && (vq->last_avail_idx == vq->avail->idx+1))
					vq->stats.ring_full++;
				vq->stats.handled_bytes = 0;
				vq->stats.was_limited = 0;
				rdtscll(work_start_tsc);
				work->fn(work);
				rdtscll(work_end_tsc);
				vq->stats.notif_cycles += (work_end_tsc - work_start_tsc);
				if (likely(vq->stats.notif_works++ > 0))
					vq->stats.notif_wait+=(work_start_tsc-vq->stats.last_notif_tsc_end);
				vq->stats.last_notif_tsc_end = work_end_tsc;
				vq->stats.notif_bytes+=vq->stats.handled_bytes;
				vq->stats.notif_limited+=vq->stats.was_limited;
			} else {
				vhost_printk("performing noqueue work = %p\n.", work);
				work->fn(work);
				worker->stats.noqueue_works++;
			}
			rdtscll(worker->last_work_tsc);
			/* If vq is in the round-robin list of virtqueues being
			 * constantly checked by this thread, move vq the end
			 * of the queue, because it had its fair chance now.
			 */
			if (vq && !list_empty(&vq->vqpoll.link)) {
				list_move_tail(&vq->vqpoll.link, &worker->vqpoll_list);
			}
		} else
			worker->stats.empty_works++;

	/* Check one virtqueue from the round-robin list */
	if (!list_empty(&worker->vqpoll_list)) {
		struct vhost_virtqueue *vq = NULL;
//		vhost_printk("worker %d has a non-empty poll list!", worker->id);
		vq = roundrobin_poll(&worker->vqpoll_list);
		if (vq) {
			set_mm(vq);
			if (vq->avail && vq->last_avail_idx == vq->avail->idx +1 )
				vq->stats.ring_full++;
			vq->stats.handled_bytes = 0;
			vq->stats.was_limited = 0;
			rdtscll(poll_start_tsc);
			vq->handle_kick(&vq->poll.work);
			rdtscll(poll_end_tsc);
			vq->stats.poll_cycles+=(poll_end_tsc-poll_start_tsc);
			if (likely(vq->stats.poll_kicks++ > 0))
				vq->stats.poll_wait+=(poll_start_tsc-vq->stats.last_poll_tsc_end);
			vq->stats.last_poll_tsc_end = poll_end_tsc;
			vq->stats.poll_bytes+=vq->stats.handled_bytes;
			vq->stats.poll_limited+=vq->stats.was_limited;
		}
		else {
			worker->stats.empty_polls++;
		}
		/* If our polling list isn't empty, ask to continue
		 * running this thread, don't yield.
		 */
		__set_current_state(TASK_RUNNING);
	}

	rdtscll(loop_end_tsc);
	worker->stats.cycles+=(loop_end_tsc-loop_start_tsc)
							-(work_end_tsc-work_start_tsc)
							-(poll_end_tsc-poll_start_tsc);
	if (likely(worker->stats.loops++ > 0))
		worker->stats.wait+=(loop_start_tsc-worker->stats.last_loop_tsc_end);
	worker->stats.last_loop_tsc_end = loop_end_tsc;
	if (bh_disabled_tsc) {
		// interrupts were disabled
		if (loop_end_tsc - bh_disabled_tsc > worker->max_disabled_soft_interrupts_cycles) {
			local_bh_enable();
			worker->stats.enabled_interrupts++;
			if (need_resched())
				schedule();
			// check if we should continue disabling
			// interrupts or not (mod param value set to 0)
			if (worker->max_disabled_soft_interrupts_cycles) {
				bh_disabled_tsc = loop_end_tsc;
				local_bh_disable();
			} else {
				bh_disabled_tsc = 0;
				printk("abelg: disable_soft_interrupts_cycles=0\n");
			}
		}
	} else {
		// interrupts were not disabled
		if (need_resched())
			schedule();
		// check if now we should start disabling them (mod param value set to >0 )
		if (worker->max_disabled_soft_interrupts_cycles) {
			printk("abelg: disable_soft_interrupts_cycles=%d\n", worker->max_disabled_soft_interrupts_cycles);
			bh_disabled_tsc = loop_end_tsc;
			local_bh_disable();
		}
	}
	}
// local_irq_enable();  // check this!!
	set_fs(oldfs);
	return 0;
}

static void vhost_vq_free_iovecs(struct vhost_virtqueue *vq)
{
	kfree(vq->indirect);
	vq->indirect = NULL;
	kfree(vq->log);
	vq->log = NULL;
	kfree(vq->heads);
	vq->heads = NULL;
}

static void vhost_fs_dir_init(void *owner, struct device **vhost_fs_dev,
		struct device *parent,
		struct dev_ext_attribute *attrs,
		size_t attrs_count,
		const char *fmt, ...) {
	int i = 0;
	va_list vargs;
	vhost_printk("START.");

	va_start(vargs, fmt);
	*vhost_fs_dev = device_create_vargs(vhost_fs_class, parent, (dev_t)0,
			owner, fmt, vargs);
	va_end(vargs);
	if (IS_ERR(*vhost_fs_dev)) {
		long ret = PTR_ERR(*vhost_fs_dev);
		char *id = NULL;
		va_start(vargs, fmt);
		id = kvasprintf(GFP_KERNEL, fmt, vargs);
		va_end(vargs);
		WARN(IS_ERR_VALUE(ret), "couldn't create directory for id %s, "
			"err = %ld.\n", id, ret);
		kfree(id);
		vhost_printk("DONE - ERROR.");
		return;
	}
	vhost_printk("creating files for %s.\n", dev_name(*vhost_fs_dev));
	for (; i < attrs_count; ++i){
		int res;
		vhost_printk("creating file %s.\n", attrs[i].attr.attr.name);
		res = device_create_file(*vhost_fs_dev, &attrs[i].attr);
		if (res < 0){
			WARN(res < 0, "couldn't create class file %s, err = %d.\n",
					attrs[i].attr.attr.name, res);
			vhost_printk("DONE - ERROR.");
			return;
		}
	}
	vhost_printk("DONE.");
}

#define vhost_fs_init_per_worker_dir(w) \
		vhost_fs_dir_init(w, &w->vhost_fs_dev, vhost_fs_workers,			  \
						  vhost_fs_per_worker_attrs,						  \
						  ARRAY_LENGTH(vhost_fs_per_worker_attrs), "w.%d", w->id)
#define vhost_fs_init_device_dir(d) \
		vhost_fs_dir_init(d, &d->vhost_fs_dev, vhost_fs_devices,			  \
						  vhost_fs_device_attrs,							  \
						  ARRAY_LENGTH(vhost_fs_device_attrs), "d.%d", d->id)
#define vhost_fs_init_virtual_queue_dir(vq) \
	do{																		  \
		typeof(vq) __temp_vq_ptr = vq;										  \
		vhost_fs_dir_init(__temp_vq_ptr, &__temp_vq_ptr->vhost_fs_dev, 		  \
				vhost_fs_queues, 	 										  \
				vhost_fs_queue_attrs, ARRAY_LENGTH(vhost_fs_queue_attrs), 	  \
				"vq.%d.%d", __temp_vq_ptr->dev->id,							  \
				__temp_vq_ptr->id);											  \
	}while(0)

static void vhost_fs_dir_exit(struct device *vhost_fs_dev,
		struct dev_ext_attribute *attrs, size_t attrs_count) {
	int i=0;
	vhost_printk("START.");
	for (; i < attrs_count; ++i){
		vhost_printk("removing file %s.", attrs[i].attr.attr.name);
		device_remove_file(vhost_fs_dev, &attrs[i].attr);
	}

	// Releasing the device directory
	vhost_printk("Releasing the device directory.");
	// vhost_printk("Decrement reference count to device object.");
	// put_device(vhost_fs_dev);
	vhost_printk("Unregistering the device.");
	device_unregister(vhost_fs_dev);
	vhost_printk("DONE.");
}

#define vhost_fs_exit_per_worker_dir(w) \
	vhost_fs_dir_exit(w->vhost_fs_dev, vhost_fs_per_worker_attrs,		  \
			ARRAY_LENGTH(vhost_fs_per_worker_attrs))
#define vhost_fs_exit_device_dir(d) \
	vhost_fs_dir_exit(d->vhost_fs_dev, vhost_fs_device_attrs,			  \
			ARRAY_LENGTH(vhost_fs_device_attrs))
#define vhost_fs_exit_virtual_queue_dir(vq)  \
	do{																		  \
		typeof(vq) __temp_vq_ptr = vq;										  \
		vhost_fs_dir_exit(__temp_vq_ptr->vhost_fs_dev, 						  \
				vhost_fs_queue_attrs, ARRAY_LENGTH(vhost_fs_queue_attrs));	  \
	}while(0)

static void vhost_fs_init(void) {
	int i = 0;
	vhost_printk("START.");

	// create the vhost class
	vhost_printk("create the vhost class.");
	vhost_fs_class = class_create(THIS_MODULE, MODULE_NAME);
	if (IS_ERR(vhost_fs_class)){
		WARN(IS_ERR(vhost_fs_class), "couldn't create class, err = %ld\n",
				PTR_ERR(vhost_fs_class));
		return;
	}
	// add global files
	vhost_printk("add global files.");
	for (; i < ARRAY_LENGTH(vhost_fs_global_attrs); ++i){
		int res;
		vhost_printk("creating file %s.\n", vhost_fs_global_attrs[i].attr.name);
		res = class_create_file(vhost_fs_class, &vhost_fs_global_attrs[i]);
		if (res < 0){
			WARN(res < 0, "couldn't create class file %s, err = %d.\n",
				vhost_fs_global_attrs[i].attr.name, res);
			return;
		}
	}

	// add workers directory and global files
	vhost_printk("add workers directory and global files.");
	vhost_fs_dir_init(NULL, &vhost_fs_workers, NULL,
			vhost_fs_global_worker_attrs,
			ARRAY_LENGTH(vhost_fs_global_worker_attrs),
			"%s", VHOST_FS_DIRECTORY_WORKER);

	// add device directory and global files
	vhost_printk("add workers directory and global files.");
	vhost_fs_dir_init(NULL, &vhost_fs_devices, NULL, NULL, 0,
			"%s", VHOST_FS_DIRECTORY_DEVICE);

	// add device directory and global files
	vhost_printk("add device directory and global files.");
	vhost_fs_dir_init(NULL, &vhost_fs_queues, NULL, NULL, 0,
				"%s", VHOST_FS_DIRECTORY_VIRTUAL_QUEUE);
	vhost_printk("DONE.");
}

static void vhost_fs_exit(void) {
	int i = 0;
	vhost_printk("START.");
	// Remove files from the class directory
	vhost_printk("Remove files from the class directory.");
	for (i = 0; i < ARRAY_LENGTH(vhost_fs_global_attrs); ++i){
		vhost_printk("removing file %s.\n", vhost_fs_global_attrs[i].attr.name);
		class_remove_file(vhost_fs_class, &vhost_fs_global_attrs[i]);
	}

	// Remove workers directory and global files
	vhost_printk("Remove workers directory and global files.");
	vhost_fs_dir_exit(vhost_fs_workers, vhost_fs_global_worker_attrs,
			ARRAY_LENGTH(vhost_fs_global_worker_attrs));

	// Remove devices directory and global files
	vhost_printk("Remove devices directory and global files.");
	vhost_fs_dir_exit(vhost_fs_devices, NULL, 0);

	// Remove queues directory and global files
	vhost_printk("Remove queues directory and global files.");
	vhost_fs_dir_exit(vhost_fs_queues, NULL, 0);

	vhost_printk("Destroy the vhost fs class.");
	class_destroy(vhost_fs_class);
	vhost_printk("DONE.");
}

struct vhost_dev_transfer_struct {
	struct vhost_work work;
	struct vhost_dev *device;
	struct vhost_worker *src_worker;
	struct vhost_worker *dst_worker;
};

static void vhost_attach_device_to_worker(struct vhost_work *transfer_work){
	unsigned long flags;
	struct vhost_dev_transfer_struct *t;
	struct vhost_dev *dev = NULL;
	struct vhost_worker* worker = NULL;
	int i;

	t = container_of(transfer_work, struct vhost_dev_transfer_struct, work);
	dev = t->device;
	worker = t->dst_worker;

	for (i = 0; i < dev->nvqs; ++i) {
		if (dev->vqs[i]->vqpoll.enabled && list_empty(&dev->vqs[i]->vqpoll.link)) {
			list_add_tail(&dev->vqs[i]->vqpoll.link, &worker->vqpoll_list);
		}
	}

	spin_lock_irqsave(&worker->work_lock, flags);
	vhost_printk("increase device number of dst worker.");
	atomic_inc(&worker->num_devices);

	vhost_printk("Switch to normal operation mode.");
	/* Switch to normal operation mode */
	atomic_set(&dev->transfer.operation_mode, VHOST_DEVICE_OPERATION_MODE_NORMAL);

	spin_lock(&dev->transfer.suspended_work_lock);
	vhost_printk("Adding all suspended work items to dst worker work_list.");
	list_splice_init(&dev->transfer.suspended_work_list, &worker->work_list);
	dev->worker->stats.pending_works += dev->transfer.suspended_works;
	spin_unlock(&dev->transfer.suspended_work_lock);
	spin_unlock_irqrestore(&worker->work_lock, flags);
	vhost_printk("DONE.");
}

static void vhost_detach_device_from_worker(struct vhost_work *transfer_work){
	struct vhost_dev_transfer_struct *t;
	struct vhost_dev *dev = NULL;
	struct vhost_worker* worker = NULL;
	struct vhost_virtqueue *cur, *next;
	vhost_printk("START");
	t = container_of(transfer_work, struct vhost_dev_transfer_struct, work);
	dev = t->device;
	worker = t->src_worker;

	vhost_printk("device %d from worker %d to worker %d.\n", dev->id,
			t->src_worker->id, t->dst_worker->id);

	vhost_printk("remove all polled queues of the device from src_worker polled queues list.");
	/* remove all polled queues of the device from src_worker polled queues list */
	list_for_each_entry_safe(cur, next, &worker->vqpoll_list, vqpoll.link) {
		if (cur->dev == dev){
			vhost_printk("deleting vq %p from worker vqpoll_list", cur);
			list_del_init(&cur->vqpoll.link);
		}
	}
	vhost_printk("Reduce the number of devices.");
	/* Reduce the number of devices  */
	atomic_dec(&worker->num_devices);
	vhost_printk("Set dst worker as worker assigned to device.");
	/* Set dst worker as worker assigned to device. */
	dev->worker = t->dst_worker;
	vhost_printk("DONE.");
}

static int vhost_dev_transfer_to_worker(struct vhost_dev *d,
		struct vhost_worker* w){
	struct vhost_dev_transfer_struct transfer;

	vhost_printk("START\n");
	BUG_ON(d == NULL);
	BUG_ON(d->worker == NULL);
	BUG_ON(w == NULL);

	vhost_printk("Starting to transfer device %d from worker %d to"
			   " worker %d\n", d->id, d->worker->id, w->id);

	if (atomic_cmpxchg(&d->transfer.operation_mode, VHOST_DEVICE_OPERATION_MODE_NORMAL,
			VHOST_DEVICE_OPERATION_MODE_TRANSFERRING) ==
					VHOST_DEVICE_OPERATION_MODE_TRANSFERRING){
		vhost_printk("Error device %d already in transfer\n", d->id);
		return 0;
	}
	transfer.device = d;
	transfer.src_worker = d->worker;
	transfer.dst_worker = w;

	vhost_printk("adding work %p to queue.\n", &transfer);
	vhost_work_init(&transfer.work, NULL, vhost_detach_device_from_worker);
	vhost_work_force_enqueue(d->worker, &transfer.work);
	vhost_printk("enqueued detach device %d\n", d->id);
	vhost_work_flush(d, &transfer.work);
	vhost_printk("detached device %d\n", d->id);

	BUG_ON(d->worker != w);
	BUG_ON(atomic_read(&d->transfer.operation_mode) !=
			VHOST_DEVICE_OPERATION_MODE_TRANSFERRING);

	/* Place the vhost_attach_device_to_worker in the dest worker work_list */
	vhost_work_init(&transfer.work, NULL, vhost_attach_device_to_worker);
	vhost_work_force_enqueue(w, &transfer.work);
	vhost_printk("enqueued attach device %d\n", d->id);
	vhost_work_flush(d, &transfer.work);
	vhost_printk("attached device %d\n", d->id);
	return 1;
}

static void vhost_idle_work(struct vhost_work *w){
	vhost_printk("\n");
	unuse_mm(current->mm);
}

struct vhost_worker_shutdown_struct {
	struct vhost_work work;
	struct vhost_worker *worker;
};

static void vhost_worker_shutdown_work(struct vhost_work *w){
	struct vhost_worker_shutdown_struct *sd;
	int old_state;

	sd = container_of(w, struct vhost_worker_shutdown_struct, work);
	old_state = atomic_cmpxchg(&sd->worker->state, VHOST_WORKER_STATE_LOCKED,
			VHOST_WORKER_STATE_SHUTDOWN);
	vhost_printk("\n");
	if (old_state != VHOST_WORKER_STATE_LOCKED) {
		vhost_printk("worker wasn't locked was %d (0 = normal, 1 = locked, "
				"2 = shutdown). DONE.\n", old_state);
		return;
	}
	vhost_printk("\n");
	unuse_mm(current->mm);
}

/**
 * Force remove @worker from worker poll.
 *
 * Note: use vhost_worker_remove instead.
 */
static int vhost_worker_remove_unsafe(struct vhost_worker *worker){
	struct vhost_worker_shutdown_struct shutdown;

	vhost_printk("\n");
	vhost_fs_exit_per_worker_dir(worker);
	vhost_printk("\n");

	BUG_ON(atomic_read(&worker->num_devices) != 0);

	vhost_printk("releasing worker %d (list_empty = %d, will print warning if "
				"list is not empty)\n", worker->id, list_empty(&worker->work_list));

	/* The add a shutdown work that will be the last work in the worker work
	 * list. */
	vhost_printk("Add a shutdown work that will be the last work in the worker "
			"work list.");
	shutdown.worker = worker;
	vhost_work_init(&shutdown.work, NULL, vhost_worker_shutdown_work);
	vhost_printk("\n");
	vhost_work_force_enqueue(worker, &shutdown.work);
	/*wait until the final work is done.*/
	vhost_printk("wait until the final work is done.");
	vhost_worker_flush_work(&shutdown.work);
	vhost_printk("\n");

	/* cleanup worker */
	spin_lock_irq(&workers_pool.workers_lock);
	vhost_printk("\n");
	worker->worker_thread = NULL;
	vhost_printk("\n");
	list_del(&worker->node);
	vhost_printk("\n");
	spin_unlock_irq(&workers_pool.workers_lock);
	vhost_printk("\n");
	kfree(worker);
	vhost_printk("DONE!\n");
	worker = NULL;
	return 1;
}

/**
 * Remove @worker from worker poll. A worker will be removed only if it is
 * locked and no devices are assigned to it (other cases will cause the function
 * to bug).
 */
static int vhost_worker_remove(struct vhost_worker *worker){
	int state;

	vhost_printk("START\n");
	BUG_ON(worker == NULL);
	vhost_printk("\n");
	state = atomic_read(&worker->state);
	vhost_printk("\n");
	if (state != VHOST_WORKER_STATE_LOCKED) {
		vhost_printk("worker is already during shutdown or is normal. DONE.");
		return state != VHOST_WORKER_STATE_NORMAL;
	}
	vhost_worker_remove_unsafe(worker);
	return 1;
}

int vhost_dev_table_add(struct vhost_dev *dev){
	spin_lock_irq(&dev_table.entries_lock);
	list_add_tail(&dev->vhost_dev_table_entry, &dev_table.entries);
	spin_unlock_irq(&dev_table.entries_lock);
	return 1;
}

int vhost_dev_table_remove(struct vhost_dev *dev){
	vhost_printk("START removing device %d\n", dev->id);
	spin_lock_irq(&dev_table.entries_lock);
	if (!list_empty(&dev->vhost_dev_table_entry))
		list_del_init(&dev->vhost_dev_table_entry);
	spin_unlock_irq(&dev_table.entries_lock);
	vhost_printk("DONE removing device %d\n", dev->id);
	return 1;
}
struct vhost_dev *vhost_dev_table_get(int device_id){
	struct vhost_dev *pos;
	vhost_printk("START searching for device %d\n", device_id);
	spin_lock_irq(&dev_table.entries_lock);
	list_for_each_entry(pos, &dev_table.entries, vhost_dev_table_entry) {
		if (pos->id == device_id){
			spin_unlock_irq(&dev_table.entries_lock);
			vhost_printk("device found. DONE\n");
			return pos;
		}
	}
	spin_unlock_irq(&dev_table.entries_lock);
	/* device wasn't found. */
	vhost_printk("device wasn't found. DONE\n");
	return NULL;
}

static int vhsot_worker_pool_init(void){
	int i;
	workers_pool.default_worker = NULL;

	for (i=0; i<VHOST_WORKERS_POOL_NEW_WORKERS_SIZE; ++i){
		workers_pool.new_workers[i] = VHOST_WORKERS_POOL_NEW_WORKERS_VACANT;
	}
	spin_lock_init(&workers_pool.workers_lock);
	INIT_LIST_HEAD(&workers_pool.workers_list);

	return 1;
}


static int __init vhost_init(void)
{
	vhost_printk("START");
	vhsot_worker_pool_init();
	spin_lock_init(&dev_table.entries_lock);
	INIT_LIST_HEAD(&dev_table.entries);
	atomic_set(&epoch, 0);
	rdtscll(cycles_start_tsc);

	vhost_printk("calling vhost_fs_init");
	vhost_fs_init();
	vhost_printk("DONE - elvis v2.9.15.");
	return 0;
}

static int vhost_worker_set_locked(struct vhost_worker *worker);
static inline int vhost_worker_set_locked_unsafe(struct vhost_worker *worker);

static void vhost_exit(void)
{
	int i=0;
	struct vhost_worker *pos;

	while (!list_empty(&dev_table.entries)){
		struct vhost_dev *d = list_first_entry(&dev_table.entries,
									struct vhost_dev, vhost_dev_table_entry);
		vhost_dev_cleanup(d, false);
	}

	spin_lock_irq(&workers_pool.workers_lock);
	list_for_each_entry(pos, &workers_pool.workers_list, node){
		++i;
	}
	vhost_printk("There are %d worker(s) in the pool.\n", i);
	++i;
	while (!list_empty(&workers_pool.workers_list) && i>0){
		struct vhost_worker *w = list_first_entry(&workers_pool.workers_list,
					struct vhost_worker, node);
		--i;
		spin_unlock_irq(&workers_pool.workers_lock);
		vhost_worker_set_locked_unsafe(w);
		vhost_worker_remove_unsafe(w);
		spin_lock_irq(&workers_pool.workers_lock);
	}
	spin_unlock_irq(&workers_pool.workers_lock);

	WARN_ON(i == 0);

	vhost_fs_exit();
}

/* Helper to allocate iovec buffers for all vqs. */
static long vhost_dev_alloc_iovecs(struct vhost_dev *dev)
{
	struct vhost_virtqueue *vq;
	int i;

	for (i = 0; i < dev->nvqs; ++i) {
		vq = dev->vqs[i];
		vq->indirect = kmalloc(sizeof *vq->indirect * UIO_MAXIOV,
				       GFP_KERNEL);
		vq->log = kmalloc(sizeof *vq->log * UIO_MAXIOV, GFP_KERNEL);
		vq->heads = kmalloc(sizeof *vq->heads * UIO_MAXIOV, GFP_KERNEL);
		if (!vq->indirect || !vq->log || !vq->heads)
			goto err_nomem;
	}
	return 0;

err_nomem:
	for (; i >= 0; --i)
		vhost_vq_free_iovecs(dev->vqs[i]);
	return -ENOMEM;
}

static void vhost_dev_free_iovecs(struct vhost_dev *dev)
{
	int i;

	for (i = 0; i < dev->nvqs; ++i)
		vhost_vq_free_iovecs(dev->vqs[i]);
}

static int workers_pool_set_default_worker(struct vhost_worker *worker){
	if (atomic_read(&worker->state) == VHOST_WORKER_STATE_LOCKED)
		return 0;

	workers_pool.default_worker = worker;
	return 1;
}

/* Acquires locks before trying to set @worker as default worker. */
static int workers_pool_set_default_worker_safe(struct vhost_worker *worker){
	int res;
	vhost_printk("worker = %d START.", worker->id);
	spin_lock_irq(&workers_pool.workers_lock);
	res = workers_pool_set_default_worker(worker);
	spin_unlock_irq(&workers_pool.workers_lock);
	vhost_printk("worker = %d DONE.", worker->id);
	return res;
}

static int create_new_worker(int cpu, bool default_worker){
	struct vhost_worker *worker;
	int i;

	vhost_printk("START.");

	worker = kmalloc(sizeof *worker, GFP_KERNEL);
	memset(worker, 0, sizeof *worker);
	atomic_set(&worker->num_devices, 0);
	atomic_set(&worker->state, VHOST_WORKER_STATE_NORMAL);
	spin_lock_init(&worker->work_lock);
	INIT_LIST_HEAD(&worker->work_list);

	worker->worker_thread = kthread_create(vhost_worker_thread, worker, "vhost-%d", worker->id);
	if (cpu != -1){
		struct cpumask mask;
		char str_mask[64] = {0};
		int length = 0;
		int i;
		int ret;

		cpumask_clear(&mask);
		cpumask_set_cpu(cpu, &mask);
		ret = set_cpus_allowed_ptr(worker->worker_thread, &mask);
		BUG_ON(ret < 0);

		for_each_cpu(i, &mask){
			length += sprintf(str_mask + length, "%d ", i);
		}
		vhost_printk("mask = %s", str_mask);
//		kthread_bind(worker->worker_thread, cpu);
	}

	/*
	 * vqpoll_list starts out empty, so we don't continuously
	 * poll on any virtqueue, and rather just wait for guest
	 * notifications. Later when we notice work on some virtqueue,
	 * we will switch it to polling mode, i.e., add it to
	 * vqpoll_list and disable guest notifications.
	 */
	INIT_LIST_HEAD(&worker->vqpoll_list);
	worker->work_list_max_stuck_cycles =
			worker_work_list_default_max_stuck_cycles;
	worker->max_disabled_soft_interrupts_cycles =
			worker_default_disable_soft_interrupts_cycles;
	spin_lock_irq(&workers_pool.workers_lock);
	worker->id = ++workers_pool.last_worker_id;
	if (default_worker){
		workers_pool_set_default_worker(worker);
	}
	list_add(&worker->node, &workers_pool.workers_list);
	spin_unlock_irq(&workers_pool.workers_lock);

	vhost_fs_init_per_worker_dir(worker);
	vhost_printk("New worker %d was created.\n", worker->id);

	for (i=0; i<VHOST_WORKERS_POOL_NEW_WORKERS_SIZE; ++i){
		if (workers_pool.new_workers[i] != VHOST_WORKERS_POOL_NEW_WORKERS_VACANT){
			continue;
		}
		workers_pool.new_workers[i] = worker->id;
		break;
	}

	vhost_printk("END.");
	return worker->id;
}

/**
 * Search the worker pool for the first worker which in NORMAL operation mode,
 * and sets it as the default worker.
 * If no NORMAL workers are found the function creates a new worker and sets it
 * as the default worker.
 */
static void workers_pool_locate_new_default_worker(void){
	struct vhost_worker *cur;
	vhost_printk("START.");
	/* searching for a NORMAL worker to be the default worker */
	spin_lock_irq(&workers_pool.workers_lock);
	list_for_each_entry(cur, &workers_pool.workers_list, node) {
		if (workers_pool_set_default_worker(cur)){
			/* we successfully found and set a worker to be the default worker */
			spin_unlock_irq(&workers_pool.workers_lock);
			vhost_printk("DONE worker = %d is now the default worker.\n", cur->id);
			return;
		}
	}
	spin_unlock_irq(&workers_pool.workers_lock);
	/* at this point we haven't found any NORMAL workers to be the default. */
	create_new_worker(-1, 1);
	vhost_printk("DONE worker = %d is now the default worker.\n",
			workers_pool.default_worker->id);
}

static int worker_get_cpu(struct vhost_worker *worker){
	struct cpumask mask;
	char str_mask[64] = {0};
	int length = 0;
	int cpu;
	int i;
	unsigned long flags;
	struct task_struct *p = worker->worker_thread;

	raw_spin_lock_irqsave(&p->pi_lock, flags);
	cpumask_and(&mask, &p->cpus_allowed, cpu_online_mask);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

//	cpuset_cpus_allowed(worker->worker_thread, &mask);
	for_each_cpu(i, &mask){
		length += sprintf(str_mask + length, "%d ", i);
	}
	vhost_printk("mask = %s", str_mask);

	if ((cpu = cpumask_next(-1, &mask)) >= nr_cpu_ids){
		// there is no allowed cpus.
		BUG();
	}
	if (cpumask_next(cpu, &mask) < nr_cpu_ids){
		// there is more then one cpu allowed
		return -1;
	}
	return cpu;
}

static int vhost_worker_set_unlocked(struct vhost_worker *worker){
	int old_state;

	vhost_printk("worker = %d START.\n", worker->id);
	BUG_ON(worker == NULL);
	old_state = atomic_cmpxchg(&worker->state, VHOST_WORKER_STATE_LOCKED,
			VHOST_WORKER_STATE_NORMAL);
	if (old_state == VHOST_WORKER_STATE_NORMAL){
		vhost_printk("worker = %d is already unlocked. DONE.", worker->id);
		return 1;
	}
	vhost_printk("worker = %d cannot be unlocked. DONE.", worker->id);
	return 0;
}

/* Sets a worker as locked. meaning no new devices can be assigned to it.
 * @return 1 if the worker was already locked, 0 otherwise.
 *
 * Note: this function is unsafe because it can leave vhost with a locked
 * default worker, use vhost_worker_set_locked() instead.
 */
static inline int vhost_worker_set_locked_unsafe(struct vhost_worker *worker){
	// if worker is in normal set as locked.
	int old_state = atomic_cmpxchg(&worker->state, VHOST_WORKER_STATE_NORMAL,
			VHOST_WORKER_STATE_LOCKED);
	if (old_state != VHOST_WORKER_STATE_NORMAL){
		BUG_ON(old_state == VHOST_WORKER_STATE_SHUTDOWN);
		vhost_printk("worker = %d is already locked. DONE.", worker->id);
		return 1;
	}
	return 0;
}

/* Sets a worker as locked. meaning no new devices can be assigned to it. if the
 * worker was the default worker then the function finds another unlocked worker
 * and sets it as the default worker. If there are no unlocked workers then the
 * function creates one.
 *
 * @return 1 if the worker is now locked, 0 otherwise.
 */
static int vhost_worker_set_locked(struct vhost_worker *worker){
	vhost_printk("worker = %d START.\n", worker->id);
	BUG_ON(worker == NULL);
	if (vhost_worker_set_locked_unsafe(worker)){
		// the worker was already locked, no need to do additional checks.
		return 1;
	}
	// check if we locked the default worker, if so locate a new default worker.
	spin_lock_irq(&workers_pool.workers_lock);
	if (likely(workers_pool.default_worker != worker)){
		spin_unlock_irq(&workers_pool.workers_lock);
		vhost_printk("worker = %d is not the default worker and is now locked. "
				"DONE.\n", worker->id);
		return 1;
	}
	spin_unlock_irq(&workers_pool.workers_lock);
	workers_pool_locate_new_default_worker();
	vhost_printk("worker = %d was the default worker and is now locked. DONE.\n", worker->id);
	return 1;
}

/* assign a the default worker for the device */
static void vhost_dev_assign_worker(struct vhost_dev *dev){
	vhost_printk("START dev = %d.", dev->id);
	atomic_inc(&workers_pool.default_worker->num_devices);

	if (unlikely(workers_pool.default_worker == NULL)){
		workers_pool_locate_new_default_worker();
	}

	dev->worker = workers_pool.default_worker;
	vhost_printk("END.");
}

void vhost_dev_init(struct vhost_dev *dev,
		    struct vhost_virtqueue **vqs, int nvqs)
{
	int i;

	vhost_printk("START.");
	memset(dev, 0, sizeof *dev);

	dev->vqs = vqs;
	dev->nvqs = nvqs;
	mutex_init(&dev->mutex);
	dev->log_ctx = NULL;
	dev->log_file = NULL;
	dev->memory = NULL;
	dev->mm = NULL;
	dev->owner = NULL;
	dev->id = atomic_inc_return(&last_vqid);

	if (workers_pool.default_worker == NULL){
		create_new_worker(-1, 1);
		/* create new worker implicitly sets the default worker if there is no
		 * default worker already present. */
	}
	vhost_dev_assign_worker(dev);
	spin_lock_init(&dev->transfer.suspended_work_lock);
	INIT_LIST_HEAD(&dev->transfer.suspended_work_list);
	atomic_set(&dev->transfer.operation_mode, VHOST_DEVICE_OPERATION_MODE_NORMAL);

	vhost_printk("initializing device %d in worker %d.\n", dev->id,
			dev->worker->id);
	vhost_fs_init_device_dir(dev);

	for (i = 0; i < dev->nvqs; ++i) {
		dev->vqs[i]->log = NULL;
		dev->vqs[i]->indirect = NULL;
		dev->vqs[i]->heads = NULL;
		dev->vqs[i]->dev = dev;
		dev->vqs[i]->id = i;
		memset(&dev->vqs[i]->stats, 0, sizeof dev->vqs[i]->stats);
		memset(&dev->vqs[i]->vqpoll, 0, sizeof dev->vqs[i]->vqpoll);
		vhost_fs_init_virtual_queue_dir(dev->vqs[i]);
		mutex_init(&dev->vqs[i]->mutex);
		vhost_vq_reset(dev, dev->vqs[i]);
		if (dev->vqs[i]->handle_kick)
			vhost_poll_init(&dev->vqs[i]->poll,
					dev->vqs[i]->handle_kick, POLLIN, dev->vqs[i]);
	}

	INIT_LIST_HEAD(&dev->vhost_dev_table_entry);
	vhost_dev_table_add(dev);
	vhost_printk("END.");
}
EXPORT_SYMBOL_GPL(vhost_dev_init);

/* Caller should have device mutex */
long vhost_dev_check_owner(struct vhost_dev *dev)
{
	/* Are you the owner? If not, I don't think you mean to do that */
	return dev->mm == current->mm ? 0 : -EPERM;
}
EXPORT_SYMBOL_GPL(vhost_dev_check_owner);

struct vhost_attach_cgroups_struct {
	struct vhost_work work;
	struct task_struct *owner;
	int ret;
};

static void vhost_attach_cgroups_work(struct vhost_work *work)
{
	struct vhost_attach_cgroups_struct *s;

	s = container_of(work, struct vhost_attach_cgroups_struct, work);
	s->ret = cgroup_attach_task_all(s->owner, current);
}

static int vhost_attach_cgroups(struct vhost_dev *dev)
{
	struct vhost_attach_cgroups_struct attach;

	attach.owner = current;
	vhost_work_init(&attach.work, NULL, vhost_attach_cgroups_work);
	vhost_work_queue(dev, &attach.work);
	vhost_work_flush(dev, &attach.work);
	return attach.ret;
}

/* Caller should have device mutex */
bool vhost_dev_has_owner(struct vhost_dev *dev)
{
	return dev->mm;
}
EXPORT_SYMBOL_GPL(vhost_dev_has_owner);

/* Caller should have device mutex */
long vhost_dev_set_owner(struct vhost_dev *dev)
{
	struct task_struct *worker;
	int err;

	/* Is there an owner already? */
	if (vhost_dev_has_owner(dev)) {
		err = -EBUSY;
		goto err_mm;
	}

	/* No owner, become one */
	dev->mm = get_task_mm(current);
	dev->owner = current;
	if (!dev->worker)
		vhost_printk("vhost_dev_set_owner: worker not assigned!\n");
	else
		vhost_printk("vhost_dev_set_owner: using worker %d\n",
		       dev->worker->id);

	wake_up_process(dev->worker->worker_thread);  /* avoid contributing to loadavg */

	err = vhost_attach_cgroups(dev);
	if (err)
		goto err_cgroup;

	err = vhost_dev_alloc_iovecs(dev);
	if (err)
		goto err_cgroup;

	return 0;
err_cgroup:
	kthread_stop(worker);
	dev->worker = NULL;
err_mm:
	return err;
}
EXPORT_SYMBOL_GPL(vhost_dev_set_owner);

struct vhost_memory *vhost_dev_reset_owner_prepare(void)
{
	return kmalloc(offsetof(struct vhost_memory, regions), GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(vhost_dev_reset_owner_prepare);

/* Caller should have device mutex */
void vhost_dev_reset_owner(struct vhost_dev *dev, struct vhost_memory *memory)
{
	int i;

	vhost_dev_cleanup(dev, true);

	/* Restore memory to default empty mapping. */
	memory->nregions = 0;
	dev->memory = memory;
	/* We don't need VQ locks below since vhost_dev_cleanup makes sure
	 * VQs aren't running.
	 */
	for (i = 0; i < dev->nvqs; ++i)
		dev->vqs[i]->memory = memory;
}
EXPORT_SYMBOL_GPL(vhost_dev_reset_owner);

void vhost_dev_stop(struct vhost_dev *dev)
{
	int i;

	for (i = 0; i < dev->nvqs; ++i) {
		if (dev->vqs[i]->kick && dev->vqs[i]->handle_kick) {
			vhost_poll_stop(&dev->vqs[i]->poll);
			vhost_poll_flush(&dev->vqs[i]->poll);
		}
	}
}
EXPORT_SYMBOL_GPL(vhost_dev_stop);

/* vhost_vq_disable_vqpoll() asks the worker thread to disables virtqueue polling
 * mode for a given virtqueue. We ask the worker thread to do this rather than
 * doing it directly, so that we don't race with the worker thread's use of the
 * queue.
 */
static inline void vhost_vq_disable_vqpoll_work(struct vhost_work *work)
{
	vhost_vq_disable_vqpoll_unsafe(work->vq);
	WARN_ON(work->vq->vqpoll.avail_mapped);
}

/* vhost_vq_disable_vqpoll() asks the worker thread to shut down virtqueue polling
 * mode for a given virtqueue which is itself being shut down. We ask the
 * worker thread to do this rather than doing it directly, so that we don't
 * race with the worker thread's use of the queue.
 */
static inline void vhost_vq_shutdown_vqpoll_work(struct vhost_work *work)
{
	work->vq->vqpoll.shutdown = true;
	vhost_vq_disable_vqpoll_work(work);
}

static inline void vhost_vq_disable_vqpoll(struct vhost_virtqueue *vq, bool shutdown)
{
	struct vhost_work work;
	if (shutdown){
		vhost_work_init(&work, vq, vhost_vq_shutdown_vqpoll_work);
	}else{
		vhost_work_init(&work, vq, vhost_vq_disable_vqpoll_work);
	}

	vhost_work_queue(vq->dev, &work);
	vhost_work_flush(vq->dev, &work);
	printk_vq("NYH: Done vhost_vq_disable_vqpoll", vq);
}

/* vhost_vq_enable_vqpoll() asks the worker thread to start virtqueue polling
 * mode for a given virtqueue. We ask the worker thread to do this rather than
 * doing it directly, so that we don't race with the worker thread's use of the
 * queue.
 */
static inline void vhost_vq_enable_vqpoll_work(struct vhost_work *work)
{
	vhost_vq_enable_vqpoll_unsafe(work->vq);
//	WARN_ON(!work->vq->vqpoll.avail_mapped);
}

static inline void vhost_vq_enable_vqpoll(struct vhost_virtqueue *vq)
{
	struct vhost_work work;
	vhost_work_init(&work, vq, vhost_vq_enable_vqpoll_work);
	vhost_work_queue(vq->dev, &work);
	vhost_work_flush(vq->dev, &work);
	printk_vq("NYH: Done vhost_vq_enable_vqpoll", vq);
}

/* Caller should have device mutex if and only if locked is set */
void vhost_dev_cleanup(struct vhost_dev *dev, bool locked)
{
	struct vhost_work idle;
	int i;
	vhost_printk("START dev %d ptr %p locked %d.\n", dev->id, dev, locked);
	for (i = 0; i < dev->nvqs; ++i) {
		if (dev->vqs[i]->error_ctx)
			eventfd_ctx_put(dev->vqs[i]->error_ctx);
		if (dev->vqs[i]->error)
			fput(dev->vqs[i]->error);
		if (dev->vqs[i]->kick)
			fput(dev->vqs[i]->kick);
		if (dev->vqs[i]->call_ctx)
			eventfd_ctx_put(dev->vqs[i]->call_ctx);
		if (dev->vqs[i]->call)
			fput(dev->vqs[i]->call);

		vhost_vq_disable_vqpoll(dev->vqs[i], true);

		vhost_printk("\n");

		vhost_work_init(&idle, NULL, vhost_idle_work);
		vhost_printk("\n");
		vhost_work_queue(dev, &idle);
		/*wait until the final work is done.*/
		vhost_printk("wait until the final work is done.");
		vhost_work_flush(dev, &idle);
		vhost_printk("\n");

		// TODO: Think: When a device goes down, the worker might
		// still have its mm as its current mm. We will never use it
		// again (before the next work, we switch mm again) but maybe
		// holding it wastes memory?
		vhost_fs_exit_virtual_queue_dir(dev->vqs[i]);
		vhost_dev_table_remove(dev);
		vhost_vq_reset(dev, dev->vqs[i]);
	}
	vhost_printk("\n");
	vhost_dev_free_iovecs(dev);
	vhost_printk("\n");
	if (dev->log_ctx)
		eventfd_ctx_put(dev->log_ctx);
	dev->log_ctx = NULL;
	vhost_printk("\n");
	if (dev->log_file)
		fput(dev->log_file);
	vhost_printk("\n");
	dev->log_file = NULL;
	/* No one will access memory at this point */
	vhost_printk("\n");
	kfree(dev->memory);
	vhost_printk("\n");
	dev->memory = NULL;
	vhost_printk("cleaning up device %d in worker %d.\n", dev->id, dev->worker->id);
	vhost_fs_exit_device_dir(dev);
	vhost_printk("\n");
	if (dev->worker) {
		vhost_printk("\n");
		// decrease number of devices
		atomic_dec_if_positive(&dev->worker->num_devices);
	}
	vhost_printk("\n");
	// TODO: maybe we need to copy dev->mm, zero it, and only then mmput,
	// to ensure we don't mm_use it again?
	vhost_printk("\n");
	if (dev->mm)
		mmput(dev->mm);
	vhost_printk("\n");
	dev->mm = NULL;
	dev->owner = NULL;
	vhost_printk("DONE dev-ptr %p locked %d.\n", dev, locked);
}
EXPORT_SYMBOL_GPL(vhost_dev_cleanup);

static int log_access_ok(void __user *log_base, u64 addr, unsigned long sz)
{
	u64 a = addr / VHOST_PAGE_SIZE / 8;

	/* Make sure 64 bit math will not overflow. */
	if (a > ULONG_MAX - (unsigned long)log_base ||
	    a + (unsigned long)log_base > ULONG_MAX)
		return 0;

	return access_ok(VERIFY_WRITE, log_base + a,
			 (sz + VHOST_PAGE_SIZE * 8 - 1) / VHOST_PAGE_SIZE / 8);
}

/* Caller should have vq mutex and device mutex. */
static int vq_memory_access_ok(void __user *log_base, struct vhost_memory *mem,
			       int log_all)
{
	int i;

	if (!mem)
		return 0;

	for (i = 0; i < mem->nregions; ++i) {
		struct vhost_memory_region *m = mem->regions + i;
		unsigned long a = m->userspace_addr;
		if (m->memory_size > ULONG_MAX)
			return 0;
		else if (!access_ok(VERIFY_WRITE, (void __user *)a,
				    m->memory_size))
			return 0;
		else if (log_all && !log_access_ok(log_base,
						   m->guest_phys_addr,
						   m->memory_size))
			return 0;
	}
	return 1;
}

/* Can we switch to this memory table? */
/* Caller should have device mutex but not vq mutex */
static int memory_access_ok(struct vhost_dev *d, struct vhost_memory *mem,
			    int log_all)
{
	int i;

	for (i = 0; i < d->nvqs; ++i) {
		int ok;
		bool log;

		mutex_lock(&d->vqs[i]->mutex);
		log = log_all || vhost_has_feature(d->vqs[i], VHOST_F_LOG_ALL);
		/* If ring is inactive, will check when it's enabled. */
		if (d->vqs[i]->private_data)
			ok = vq_memory_access_ok(d->vqs[i]->log_base, mem, log);
		else
			ok = 1;
		mutex_unlock(&d->vqs[i]->mutex);
		if (!ok)
			return 0;
	}
	return 1;
}

static int vq_access_ok(struct vhost_virtqueue *vq, unsigned int num,
			struct vring_desc __user *desc,
			struct vring_avail __user *avail,
			struct vring_used __user *used)
{
	size_t s = vhost_has_feature(vq, VIRTIO_RING_F_EVENT_IDX) ? 2 : 0;
	return access_ok(VERIFY_READ, desc, num * sizeof *desc) &&
	       access_ok(VERIFY_READ, avail,
			 sizeof *avail + num * sizeof *avail->ring + s) &&
	       access_ok(VERIFY_WRITE, used,
			sizeof *used + num * sizeof *used->ring + s);
}

/* Can we log writes? */
/* Caller should have device mutex but not vq mutex */
int vhost_log_access_ok(struct vhost_dev *dev)
{
	return memory_access_ok(dev, dev->memory, 1);
}
EXPORT_SYMBOL_GPL(vhost_log_access_ok);

/* Verify access for write logging. */
/* Caller should have vq mutex and device mutex */
static int vq_log_access_ok(struct vhost_virtqueue *vq,
			    void __user *log_base)
{
	size_t s = vhost_has_feature(vq, VIRTIO_RING_F_EVENT_IDX) ? 2 : 0;

	return vq_memory_access_ok(log_base, vq->memory,
				   vhost_has_feature(vq, VHOST_F_LOG_ALL)) &&
		(!vq->log_used || log_access_ok(log_base, vq->log_addr,
					sizeof *vq->used +
					vq->num * sizeof *vq->used->ring + s));
}

/* Can we start vq? */
/* Caller should have vq mutex and device mutex */
int vhost_vq_access_ok(struct vhost_virtqueue *vq)
{
	return vq_access_ok(vq, vq->num, vq->desc, vq->avail, vq->used) &&
		vq_log_access_ok(vq, vq->log_base);
}
EXPORT_SYMBOL_GPL(vhost_vq_access_ok);

static long vhost_set_memory(struct vhost_dev *d, struct vhost_memory __user *m)
{
	struct vhost_memory mem, *newmem, *oldmem;
	unsigned long size = offsetof(struct vhost_memory, regions);
	int i;

	if (copy_from_user(&mem, m, size))
		return -EFAULT;
	if (mem.padding)
		return -EOPNOTSUPP;
	if (mem.nregions > VHOST_MEMORY_MAX_NREGIONS)
		return -E2BIG;
	newmem = kmalloc(size + mem.nregions * sizeof *m->regions, GFP_KERNEL);
	if (!newmem)
		return -ENOMEM;

	memcpy(newmem, &mem, size);
	if (copy_from_user(newmem->regions, m->regions,
			   mem.nregions * sizeof *m->regions)) {
		kfree(newmem);
		return -EFAULT;
	}

	if (!memory_access_ok(d, newmem, 0)) {
		kfree(newmem);
		return -EFAULT;
	}
	oldmem = d->memory;
	d->memory = newmem;

	/* All memory accesses are done under some VQ mutex. */
	for (i = 0; i < d->nvqs; ++i) {
		mutex_lock(&d->vqs[i]->mutex);
		d->vqs[i]->memory = newmem;
		mutex_unlock(&d->vqs[i]->mutex);
	}
	kfree(oldmem);
	return 0;
}

long vhost_vring_ioctl(struct vhost_dev *d, int ioctl, void __user *argp)
{
	struct file *eventfp, *filep = NULL;
	bool pollstart = false, pollstop = false;
	struct eventfd_ctx *ctx = NULL;
	u32 __user *idxp = argp;
	struct vhost_virtqueue *vq;
	struct vhost_vring_state s;
	struct vhost_vring_file f;
	struct vhost_vring_addr a;
	u32 idx;
	long r;

	vhost_printk("START device = %d, worker = %d, ioctl = %d, argp = %p\n",
			d->id, d->worker->id, ioctl, argp);

	r = get_user(idx, idxp);
	if (r < 0)
		return r;
	if (idx >= d->nvqs)
		return -ENOBUFS;

	vq = d->vqs[idx];

	mutex_lock(&vq->mutex);

	switch (ioctl) {
	case VHOST_SET_VRING_NUM:
		vhost_printk("VHOST_SET_VRING_NUM\n");
		/* Resizing ring with an active backend?
		 * You don't want to do that. */
		if (vq->private_data) {
			r = -EBUSY;
			break;
		}
		if (copy_from_user(&s, argp, sizeof s)) {
			r = -EFAULT;
			break;
		}
		if (!s.num || s.num > 0xffff || (s.num & (s.num - 1))) {
			r = -EINVAL;
			break;
		}
		vq->num = s.num;
		break;
	case VHOST_SET_VRING_BASE:
		vhost_printk("VHOST_SET_VRING_NUM\n");
		/* Moving base with an active backend?
		 * You don't want to do that. */
		if (vq->private_data) {
			r = -EBUSY;
			break;
		}
		if (copy_from_user(&s, argp, sizeof s)) {
			r = -EFAULT;
			break;
		}
		if (s.num > 0xffff) {
			r = -EINVAL;
			break;
		}
		vq->last_avail_idx = s.num;
		/* Forget the cached index value. */
		vq->avail_idx = vq->last_avail_idx;
		break;
	case VHOST_GET_VRING_BASE:
		vhost_printk("VHOST_GET_VRING_BASE\n");
		s.index = idx;
		s.num = vq->last_avail_idx;
		if (copy_to_user(argp, &s, sizeof s))
			r = -EFAULT;
		break;
	case VHOST_SET_VRING_ADDR:
		vhost_printk("VHOST_SET_VRING_ADDR\n");
		if (copy_from_user(&a, argp, sizeof a)) {
			r = -EFAULT;
			break;
		}
		if (a.flags & ~(0x1 << VHOST_VRING_F_LOG)) {
			r = -EOPNOTSUPP;
			break;
		}
		/* For 32bit, verify that the top 32bits of the user
		   data are set to zero. */
		if ((u64)(unsigned long)a.desc_user_addr != a.desc_user_addr ||
		    (u64)(unsigned long)a.used_user_addr != a.used_user_addr ||
		    (u64)(unsigned long)a.avail_user_addr != a.avail_user_addr) {
			r = -EFAULT;
			break;
		}
		if ((a.avail_user_addr & (sizeof *vq->avail->ring - 1)) ||
		    (a.used_user_addr & (sizeof *vq->used->ring - 1)) ||
		    (a.log_guest_addr & (sizeof *vq->used->ring - 1))) {
			r = -EINVAL;
			break;
		}

		/* We only verify access here if backend is configured.
		 * If it is not, we don't as size might not have been setup.
		 * We will verify when backend is configured. */
		if (vq->private_data) {
			if (!vq_access_ok(vq, vq->num,
				(void __user *)(unsigned long)a.desc_user_addr,
				(void __user *)(unsigned long)a.avail_user_addr,
				(void __user *)(unsigned long)a.used_user_addr)) {
				r = -EINVAL;
				break;
			}

			/* Also validate log access for used ring if enabled. */
			if ((a.flags & (0x1 << VHOST_VRING_F_LOG)) &&
			    !log_access_ok(vq->log_base, a.log_guest_addr,
					   sizeof *vq->used +
					   vq->num * sizeof *vq->used->ring)) {
				r = -EINVAL;
				break;
			}
		}

		vq->log_used = !!(a.flags & (0x1 << VHOST_VRING_F_LOG));
		vq->desc = (void __user *)(unsigned long)a.desc_user_addr;
		vq->avail = (void __user *)(unsigned long)a.avail_user_addr;
		vq->log_addr = a.log_guest_addr;
		vq->used = (void __user *)(unsigned long)a.used_user_addr;
		break;
	case VHOST_SET_VRING_KICK:
		vhost_printk("VHOST_SET_VRING_KICK\n");
		if (copy_from_user(&f, argp, sizeof f)) {
			r = -EFAULT;
			break;
		}
		eventfp = f.fd == -1 ? NULL : eventfd_fget(f.fd);
		if (IS_ERR(eventfp)) {
			r = PTR_ERR(eventfp);
			break;
		}
		if (eventfp != vq->kick) {
			pollstop = (filep = vq->kick) != NULL;
			pollstart = (vq->kick = eventfp) != NULL;
		} else
			filep = eventfp;
		break;
	case VHOST_SET_VRING_CALL:
		vhost_printk("VHOST_SET_VRING_CALL\n");
		if (copy_from_user(&f, argp, sizeof f)) {
			r = -EFAULT;
			break;
		}
		eventfp = f.fd == -1 ? NULL : eventfd_fget(f.fd);
		if (IS_ERR(eventfp)) {
			r = PTR_ERR(eventfp);
			break;
		}
		if (eventfp != vq->call) {
			filep = vq->call;
			ctx = vq->call_ctx;
			vq->call = eventfp;
			vq->call_ctx = eventfp ?
				eventfd_ctx_fileget(eventfp) : NULL;
		} else
			filep = eventfp;
		break;
	case VHOST_SET_VRING_ERR:
		vhost_printk("VHOST_SET_VRING_ERR\n");
		if (copy_from_user(&f, argp, sizeof f)) {
			r = -EFAULT;
			break;
		}
		eventfp = f.fd == -1 ? NULL : eventfd_fget(f.fd);
		if (IS_ERR(eventfp)) {
			r = PTR_ERR(eventfp);
			break;
		}
		if (eventfp != vq->error) {
			filep = vq->error;
			vq->error = eventfp;
			ctx = vq->error_ctx;
			vq->error_ctx = eventfp ?
				eventfd_ctx_fileget(eventfp) : NULL;
		} else
			filep = eventfp;
		break;
	default:
		vhost_printk("Wrong option\n");
		r = -ENOIOCTLCMD;
	}

	if (pollstop && vq->handle_kick)
		vhost_poll_stop(&vq->poll);

	if (ctx)
		eventfd_ctx_put(ctx);
	if (filep)
		fput(filep);

	if (pollstart && vq->handle_kick)
		r = vhost_poll_start(&vq->poll, vq->kick);

	mutex_unlock(&vq->mutex);

	if (pollstop && vq->handle_kick)
		vhost_poll_flush(&vq->poll);

	vhost_printk("END device = %d, worker = %d, ioctl = %d, argp = %p\n",
				d->id, d->worker->id, ioctl, argp);
	return r;
}
EXPORT_SYMBOL_GPL(vhost_vring_ioctl);

/* Caller must have device mutex */
long vhost_dev_ioctl(struct vhost_dev *d, unsigned int ioctl, void __user *argp)
{
	struct file *eventfp, *filep = NULL;
	struct eventfd_ctx *ctx = NULL;
	u64 p;
	long r;
	int i, fd;

	vhost_printk("START device = %d, worker = %d, ioctl = %d, argp = %p\n",
				d->id, d->worker->id, ioctl, argp);

	/* If you are not the owner, you can become one */
	if (ioctl == VHOST_SET_OWNER) {
		vhost_printk("VHOST_SET_OWNER\n");
		r = vhost_dev_set_owner(d);
		goto done;
	}

	/* You must be the owner to do anything else */
	r = vhost_dev_check_owner(d);
	if (r)
		goto done;

	switch (ioctl) {
	case VHOST_SET_MEM_TABLE:
		vhost_printk("VHOST_SET_MEM_TABLE\n");
		r = vhost_set_memory(d, argp);
		break;
	case VHOST_SET_LOG_BASE:
		vhost_printk("VHOST_SET_LOG_BASE\n");
		if (copy_from_user(&p, argp, sizeof p)) {
			r = -EFAULT;
			break;
		}
		if ((u64)(unsigned long)p != p) {
			r = -EFAULT;
			break;
		}
		for (i = 0; i < d->nvqs; ++i) {
			struct vhost_virtqueue *vq;
			void __user *base = (void __user *)(unsigned long)p;
			vq = d->vqs[i];
			mutex_lock(&vq->mutex);
			/* If ring is inactive, will check when it's enabled. */
			if (vq->private_data && !vq_log_access_ok(vq, base))
				r = -EFAULT;
			else
				vq->log_base = base;
			mutex_unlock(&vq->mutex);
		}
		break;
	case VHOST_SET_LOG_FD:
		vhost_printk("VHOST_SET_LOG_FD\n");
		r = get_user(fd, (int __user *)argp);
		if (r < 0)
			break;
		eventfp = fd == -1 ? NULL : eventfd_fget(fd);
		if (IS_ERR(eventfp)) {
			r = PTR_ERR(eventfp);
			break;
		}
		if (eventfp != d->log_file) {
			filep = d->log_file;
			d->log_file = eventfp;
			ctx = d->log_ctx;
			d->log_ctx = eventfp ?
				eventfd_ctx_fileget(eventfp) : NULL;
		} else
			filep = eventfp;
		for (i = 0; i < d->nvqs; ++i) {
			mutex_lock(&d->vqs[i]->mutex);
			d->vqs[i]->log_ctx = d->log_ctx;
			mutex_unlock(&d->vqs[i]->mutex);
		}
		if (ctx)
			eventfd_ctx_put(ctx);
		if (filep)
			fput(filep);
		break;
	default:
		vhost_printk("Wrong option\n");
		r = -ENOIOCTLCMD;
		break;
	}
done:
	vhost_printk("END device = %d, worker = %d, ioctl = %d, argp = %p\n",
			d->id, d->worker->id, ioctl, argp);
	return r;
}
EXPORT_SYMBOL_GPL(vhost_dev_ioctl);

static const struct vhost_memory_region *find_region(struct vhost_memory *mem,
						     __u64 addr, __u32 len)
{
	struct vhost_memory_region *reg;
	int i;

	/* linear search is not brilliant, but we really have on the order of 6
	 * regions in practice */
	for (i = 0; i < mem->nregions; ++i) {
		reg = mem->regions + i;
		if (reg->guest_phys_addr <= addr &&
		    reg->guest_phys_addr + reg->memory_size - 1 >= addr)
			return reg;
	}
	return NULL;
}

/* TODO: This is really inefficient.  We need something like get_user()
 * (instruction directly accesses the data, with an exception table entry
 * returning -EFAULT). See Documentation/x86/exception-tables.txt.
 */
static int set_bit_to_user(int nr, void __user *addr)
{
	unsigned long log = (unsigned long)addr;
	struct page *page;
	void *base;
	int bit = nr + (log % PAGE_SIZE) * 8;
	int r;

	r = get_user_pages_fast(log, 1, 1, &page);
	if (r < 0)
		return r;
	BUG_ON(r != 1);
	base = kmap_atomic(page);
	set_bit(bit, base);
	kunmap_atomic(base);
	set_page_dirty_lock(page);
	put_page(page);
	return 0;
}

static int log_write(void __user *log_base,
		     u64 write_address, u64 write_length)
{
	u64 write_page = write_address / VHOST_PAGE_SIZE;
	int r;

	if (!write_length)
		return 0;
	write_length += write_address % VHOST_PAGE_SIZE;
	for (;;) {
		u64 base = (u64)(unsigned long)log_base;
		u64 log = base + write_page / 8;
		int bit = write_page % 8;
		if ((u64)(unsigned long)log != log)
			return -EFAULT;
		r = set_bit_to_user(bit, (void __user *)(unsigned long)log);
		if (r < 0)
			return r;
		if (write_length <= VHOST_PAGE_SIZE)
			break;
		write_length -= VHOST_PAGE_SIZE;
		write_page += 1;
	}
	return r;
}

int vhost_log_write(struct vhost_virtqueue *vq, struct vhost_log *log,
		    unsigned int log_num, u64 len)
{
	int i, r;

	/* Make sure data written is seen before log. */
	smp_wmb();
	for (i = 0; i < log_num; ++i) {
		u64 l = min(log[i].len, len);
		r = log_write(vq->log_base, log[i].addr, l);
		if (r < 0)
			return r;
		len -= l;
		if (!len) {
			if (vq->log_ctx)
				eventfd_signal(vq->log_ctx, 1);
			return 0;
		}
	}
	/* Length written exceeds what we have stored. This is a bug. */
	BUG();
	return 0;
}
EXPORT_SYMBOL_GPL(vhost_log_write);

static int vhost_update_used_flags(struct vhost_virtqueue *vq)
{
	void __user *used;
	if (__put_user(vq->used_flags, &vq->used->flags) < 0)
		return -EFAULT;
	if (unlikely(vq->log_used)) {
		/* Make sure the flag is seen before log. */
		smp_wmb();
		/* Log used flag write. */
		used = &vq->used->flags;
		log_write(vq->log_base, vq->log_addr +
			  (used - (void __user *)vq->used),
			  sizeof vq->used->flags);
		if (vq->log_ctx)
			eventfd_signal(vq->log_ctx, 1);
	}
	return 0;
}

static int vhost_update_avail_event(struct vhost_virtqueue *vq, u16 avail_event)
{
	if (__put_user(vq->avail_idx, vhost_avail_event(vq)))
		return -EFAULT;
	if (unlikely(vq->log_used)) {
		void __user *used;
		/* Make sure the event is seen before log. */
		smp_wmb();
		/* Log avail event write */
		used = vhost_avail_event(vq);
		log_write(vq->log_base, vq->log_addr +
			  (used - (void __user *)vq->used),
			  sizeof *vhost_avail_event(vq));
		if (vq->log_ctx)
			eventfd_signal(vq->log_ctx, 1);
	}
	return 0;
}

int vhost_init_used(struct vhost_virtqueue *vq)
{
	int r;
	if (!vq->private_data)
		return 0;

	r = vhost_update_used_flags(vq);
	if (r)
		return r;
	vq->signalled_used_valid = false;
	return get_user(vq->last_used_idx, &vq->used->idx);
}
EXPORT_SYMBOL_GPL(vhost_init_used);

static int translate_desc(struct vhost_virtqueue *vq, u64 addr, u32 len,
			  struct iovec iov[], int iov_size)
{
	const struct vhost_memory_region *reg;
	struct vhost_memory *mem;
	struct iovec *_iov;
	u64 s = 0;
	int ret = 0;

	mem = vq->memory;
	while ((u64)len > s) {
		u64 size;
		if (unlikely(ret >= iov_size)) {
			ret = -ENOBUFS;
			break;
		}
		reg = find_region(mem, addr, len);
		if (unlikely(!reg)) {
			ret = -EFAULT;
			break;
		}
		_iov = iov + ret;
		size = reg->memory_size - addr + reg->guest_phys_addr;
		_iov->iov_len = min((u64)len - s, size);
		_iov->iov_base = (void __user *)(unsigned long)
			(reg->userspace_addr + addr - reg->guest_phys_addr);
		s += size;
		addr += size;
		++ret;
	}

	return ret;
}

/* Each buffer in the virtqueues is actually a chain of descriptors.  This
 * function returns the next descriptor in the chain,
 * or -1U if we're at the end. */
static unsigned next_desc(struct vring_desc *desc)
{
	unsigned int next;

	/* If this descriptor says it doesn't chain, we're done. */
	if (!(desc->flags & VRING_DESC_F_NEXT))
		return -1U;

	/* Check they're not leading us off end of descriptors. */
	next = desc->next;
	/* Make sure compiler knows to grab that: we don't want it changing! */
	/* We will use the result as an index in an array, so most
	 * architectures only need a compiler barrier here. */
	read_barrier_depends();

	return next;
}

static int get_indirect(struct vhost_virtqueue *vq,
			struct iovec iov[], unsigned int iov_size,
			unsigned int *out_num, unsigned int *in_num,
			struct vhost_log *log, unsigned int *log_num,
			struct vring_desc *indirect)
{
	struct vring_desc desc;
	unsigned int i = 0, count, found = 0;
	int ret;

	/* Sanity check */
	if (unlikely(indirect->len % sizeof desc)) {
		vq_err(vq, "Invalid length in indirect descriptor: "
		       "len 0x%llx not multiple of 0x%zx\n",
		       (unsigned long long)indirect->len,
		       sizeof desc);
		return -EINVAL;
	}

	ret = translate_desc(vq, indirect->addr, indirect->len, vq->indirect,
			     UIO_MAXIOV);
	if (unlikely(ret < 0)) {
		vq_err(vq, "Translation failure %d in indirect.\n", ret);
		return ret;
	}

	/* We will use the result as an address to read from, so most
	 * architectures only need a compiler barrier here. */
	read_barrier_depends();

	count = indirect->len / sizeof desc;
	/* Buffers are chained via a 16 bit next field, so
	 * we can have at most 2^16 of these. */
	if (unlikely(count > USHRT_MAX + 1)) {
		vq_err(vq, "Indirect buffer length too big: %d\n",
		       indirect->len);
		return -E2BIG;
	}

	do {
		unsigned iov_count = *in_num + *out_num;
		if (unlikely(++found > count)) {
			vq_err(vq, "Loop detected: last one at %u "
			       "indirect size %u\n",
			       i, count);
			return -EINVAL;
		}
		if (unlikely(memcpy_fromiovec((unsigned char *)&desc,
					      vq->indirect, sizeof desc))) {
			vq_err(vq, "Failed indirect descriptor: idx %d, %zx\n",
			       i, (size_t)indirect->addr + i * sizeof desc);
			return -EINVAL;
		}
		if (unlikely(desc.flags & VRING_DESC_F_INDIRECT)) {
			vq_err(vq, "Nested indirect descriptor: idx %d, %zx\n",
			       i, (size_t)indirect->addr + i * sizeof desc);
			return -EINVAL;
		}

		ret = translate_desc(vq, desc.addr, desc.len, iov + iov_count,
				     iov_size - iov_count);
		if (unlikely(ret < 0)) {
			vq_err(vq, "Translation failure %d indirect idx %d\n",
			       ret, i);
			return ret;
		}
		/* If this is an input descriptor, increment that count. */
		if (desc.flags & VRING_DESC_F_WRITE) {
			*in_num += ret;
			if (unlikely(log)) {
				log[*log_num].addr = desc.addr;
				log[*log_num].len = desc.len;
				++*log_num;
			}
		} else {
			/* If it's an output descriptor, they're all supposed
			 * to come before any input descriptors. */
			if (unlikely(*in_num)) {
				vq_err(vq, "Indirect descriptor "
				       "has out after in: idx %d\n", i);
				return -EINVAL;
			}
			*out_num += ret;
		}
	} while ((i = next_desc(&desc)) != -1);
	return 0;
}

/* This looks in the virtqueue and for the first available buffer, and converts
 * it to an iovec for convenient access.  Since descriptors consist of some
 * number of output then some number of input descriptors, it's actually two
 * iovecs, but we pack them into one and note how many of each there were.
 *
 * This function returns the descriptor number found, or vq->num (which is
 * never a valid descriptor number) if none was found.  A negative code is
 * returned on error. */
int vhost_get_vq_desc(struct vhost_virtqueue *vq,
		      struct iovec iov[], unsigned int iov_size,
		      unsigned int *out_num, unsigned int *in_num,
		      struct vhost_log *log, unsigned int *log_num)
{
	struct vring_desc desc;
	unsigned int i, head, found = 0;
	u16 last_avail_idx;
	int ret;

	/* Check it isn't doing very strange things with descriptor numbers. */
	last_avail_idx = vq->last_avail_idx;
	if (unlikely(__get_user(vq->avail_idx, &vq->avail->idx))) {
		vq_err(vq, "Failed to access avail idx at %p\n",
		       &vq->avail->idx);
		return -EFAULT;
	}

	if (unlikely((u16)(vq->avail_idx - last_avail_idx) > vq->num)) {
		vq_err(vq, "Guest moved used index from %u to %u",
		       last_avail_idx, vq->avail_idx);
		return -EFAULT;
	}

	/* If there's nothing new since last we looked, return invalid. */
	if (vq->avail_idx == last_avail_idx)
		return vq->num;

	/* Only get avail ring entries after they have been exposed by guest. */
	smp_rmb();

	/* Grab the next descriptor number they're advertising, and increment
	 * the index we've seen. */
	if (unlikely(__get_user(head,
				&vq->avail->ring[last_avail_idx % vq->num]))) {
		vq_err(vq, "Failed to read head: idx %d address %p\n",
		       last_avail_idx,
		       &vq->avail->ring[last_avail_idx % vq->num]);
		return -EFAULT;
	}

	/* If their number is silly, that's an error. */
	if (unlikely(head >= vq->num)) {
		vq_err(vq, "Guest says index %u > %u is available",
		       head, vq->num);
		return -EINVAL;
	}

	/* When we start there are none of either input nor output. */
	*out_num = *in_num = 0;
	if (unlikely(log))
		*log_num = 0;

	i = head;
	do {
		unsigned iov_count = *in_num + *out_num;
		if (unlikely(i >= vq->num)) {
			vq_err(vq, "Desc index is %u > %u, head = %u",
			       i, vq->num, head);
			return -EINVAL;
		}
		if (unlikely(++found > vq->num)) {
			vq_err(vq, "Loop detected: last one at %u "
			       "vq size %u head %u\n",
			       i, vq->num, head);
			return -EINVAL;
		}
		ret = __copy_from_user(&desc, vq->desc + i, sizeof desc);
		if (unlikely(ret)) {
			vq_err(vq, "Failed to get descriptor: idx %d addr %p\n",
			       i, vq->desc + i);
			return -EFAULT;
		}
		if (desc.flags & VRING_DESC_F_INDIRECT) {
			ret = get_indirect(vq, iov, iov_size,
					   out_num, in_num,
					   log, log_num, &desc);
			if (unlikely(ret < 0)) {
				vq_err(vq, "Failure detected "
				       "in indirect descriptor at idx %d\n", i);
				return ret;
			}
			continue;
		}

		ret = translate_desc(vq, desc.addr, desc.len, iov + iov_count,
				     iov_size - iov_count);
		if (unlikely(ret < 0)) {
			vq_err(vq, "Translation failure %d descriptor idx %d\n",
			       ret, i);
			return ret;
		}
		if (desc.flags & VRING_DESC_F_WRITE) {
			/* If this is an input descriptor,
			 * increment that count. */
			*in_num += ret;
			if (unlikely(log)) {
				log[*log_num].addr = desc.addr;
				log[*log_num].len = desc.len;
				++*log_num;
			}
		} else {
			/* If it's an output descriptor, they're all supposed
			 * to come before any input descriptors. */
			if (unlikely(*in_num)) {
				vq_err(vq, "Descriptor has out after in: "
				       "idx %d\n", i);
				return -EINVAL;
			}
			*out_num += ret;
		}
	} while ((i = next_desc(&desc)) != -1);

	/* On success, increment avail index. */
	vq->last_avail_idx++;

	/* Assume notifications from guest are disabled at this point,
	 * if they aren't we would need to update avail_event index. */
	BUG_ON(!(vq->used_flags & VRING_USED_F_NO_NOTIFY));
	return head;
}
EXPORT_SYMBOL_GPL(vhost_get_vq_desc);

/* Reverse the effect of vhost_get_vq_desc. Useful for error handling. */
void vhost_discard_vq_desc(struct vhost_virtqueue *vq, int n)
{
	vq->last_avail_idx -= n;
}
EXPORT_SYMBOL_GPL(vhost_discard_vq_desc);

/* After we've used one of their buffers, we tell them about it.  We'll then
 * want to notify the guest, using eventfd. */
int vhost_add_used(struct vhost_virtqueue *vq, unsigned int head, int len)
{
	struct vring_used_elem heads = { head, len };

	return vhost_add_used_n(vq, &heads, 1);
}
EXPORT_SYMBOL_GPL(vhost_add_used);

static int __vhost_add_used_n(struct vhost_virtqueue *vq,
			    struct vring_used_elem *heads,
			    unsigned count)
{
	struct vring_used_elem __user *used;
	u16 old, new;
	int start;

	start = vq->last_used_idx % vq->num;
	used = vq->used->ring + start;
	if (count == 1) {
		if (__put_user(heads[0].id, &used->id)) {
			vq_err(vq, "Failed to write used id");
			return -EFAULT;
		}
		if (__put_user(heads[0].len, &used->len)) {
			vq_err(vq, "Failed to write used len");
			return -EFAULT;
		}
	} else if (__copy_to_user(used, heads, count * sizeof *used)) {
		vq_err(vq, "Failed to write used");
		return -EFAULT;
	}
	if (unlikely(vq->log_used)) {
		/* Make sure data is seen before log. */
		smp_wmb();
		/* Log used ring entry write. */
		log_write(vq->log_base,
			  vq->log_addr +
			   ((void __user *)used - (void __user *)vq->used),
			  count * sizeof *used);
	}
	old = vq->last_used_idx;
	new = (vq->last_used_idx += count);
	/* If the driver never bothers to signal in a very long while,
	 * used index might wrap around. If that happens, invalidate
	 * signalled_used index we stored. TODO: make sure driver
	 * signals at least once in 2^16 and remove this. */
	if (unlikely((u16)(new - vq->signalled_used) < (u16)(new - old)))
		vq->signalled_used_valid = false;
	return 0;
}

/* After we've used one of their buffers, we tell them about it.  We'll then
 * want to notify the guest, using eventfd. */
int vhost_add_used_n(struct vhost_virtqueue *vq, struct vring_used_elem *heads,
		     unsigned count)
{
	int start, n, r;

	start = vq->last_used_idx % vq->num;
	n = vq->num - start;
	if (n < count) {
		r = __vhost_add_used_n(vq, heads, n);
		if (r < 0)
			return r;
		heads += n;
		count -= n;
	}
	r = __vhost_add_used_n(vq, heads, count);

	/* Make sure buffer is written before we update index. */
	smp_wmb();
	if (put_user(vq->last_used_idx, &vq->used->idx)) {
		vq_err(vq, "Failed to increment used idx");
		return -EFAULT;
	}
	if (unlikely(vq->log_used)) {
		/* Log used index update. */
		log_write(vq->log_base,
			  vq->log_addr + offsetof(struct vring_used, idx),
			  sizeof vq->used->idx);
		if (vq->log_ctx)
			eventfd_signal(vq->log_ctx, 1);
	}
	return r;
}
EXPORT_SYMBOL_GPL(vhost_add_used_n);

static bool vhost_notify(struct vhost_dev *dev, struct vhost_virtqueue *vq)
{
	__u16 old, new, event;
	bool v;
	/* Flush out used index updates. This is paired
	 * with the barrier that the Guest executes when enabling
	 * interrupts. */
	smp_mb();

	if (vhost_has_feature(vq, VIRTIO_F_NOTIFY_ON_EMPTY) &&
	    unlikely(vq->avail_idx == vq->last_avail_idx))
		return true;

	if (!vhost_has_feature(vq, VIRTIO_RING_F_EVENT_IDX)) {
		__u16 flags;
		if (__get_user(flags, &vq->avail->flags)) {
			vq_err(vq, "Failed to get flags");
			return true;
		}
		return !(flags & VRING_AVAIL_F_NO_INTERRUPT);
	}
	old = vq->signalled_used;
	v = vq->signalled_used_valid;
	new = vq->signalled_used = vq->last_used_idx;
	vq->signalled_used_valid = true;

	if (unlikely(!v))
		return true;

	if (get_user(event, vhost_used_event(vq))) {
		vq_err(vq, "Failed to get used event idx");
		return true;
	}
	return vring_need_event(event, new, old);
}

/* This actually signals the guest, using eventfd. */
void vhost_signal(struct vhost_dev *dev, struct vhost_virtqueue *vq)
{
	/* Signal the Guest tell them we used something up. */
	if (vq->call_ctx && vhost_notify(dev, vq))
		eventfd_signal(vq->call_ctx, 1);
}
EXPORT_SYMBOL_GPL(vhost_signal);

/* And here's the combo meal deal.  Supersize me! */
void vhost_add_used_and_signal(struct vhost_dev *dev,
			       struct vhost_virtqueue *vq,
			       unsigned int head, int len)
{
	vhost_add_used(vq, head, len);
	vhost_signal(dev, vq);
}
EXPORT_SYMBOL_GPL(vhost_add_used_and_signal);

/* multi-buffer version of vhost_add_used_and_signal */
void vhost_add_used_and_signal_n(struct vhost_dev *dev,
				 struct vhost_virtqueue *vq,
				 struct vring_used_elem *heads, unsigned count)
{
	vhost_add_used_n(vq, heads, count);
	vhost_signal(dev, vq);
}
EXPORT_SYMBOL_GPL(vhost_add_used_and_signal_n);

/* OK, now we need to know about added descriptors. */
bool vhost_enable_notify(struct vhost_dev *dev, struct vhost_virtqueue *vq)
{
	u16 avail_idx;
	int r;
//	vhost_printk("START - worker %d, dev %d, vq %d", dev->worker->id, dev->id, dev->vqs - vq);
	/* In polling mode, when the backend (e.g., net.c) asks to enable
	 * notifications, we don't enable guest notifications. Instead, start
	 * polling on this vq by adding it to the round-robin list.
	 */
	if (vq->vqpoll.enabled) {
		if (list_empty(&vq->vqpoll.link)) {
			list_add_tail(&vq->vqpoll.link,
				&vq->dev->worker->vqpoll_list);
		}
//		vhost_printk("END - worker %d, dev %d, vq %d", dev->worker->id, dev->id, dev->vqs - vq);
		return false;
	}

	if (!(vq->used_flags & VRING_USED_F_NO_NOTIFY)){
//		vhost_printk("END - worker %d, dev %d, vq %d", dev->worker->id, dev->id, dev->vqs - vq);
		return false;
	}
	vq->used_flags &= ~VRING_USED_F_NO_NOTIFY;
	if (!vhost_has_feature(vq, VIRTIO_RING_F_EVENT_IDX)) {
		r = vhost_update_used_flags(vq);
		if (r) {
			vq_err(vq, "Failed to enable notification at %p: %d\n",
			       &vq->used->flags, r);
//			vhost_printk("END - worker %d, dev %d, vq %d", dev->worker->id, dev->id, dev->vqs - vq);
			return false;
		}
	} else {
		r = vhost_update_avail_event(vq, vq->avail_idx);
		if (r) {
			vq_err(vq, "Failed to update avail event index at %p: %d\n",
			       vhost_avail_event(vq), r);
//			vhost_printk("END - worker %d, dev %d, vq %d", dev->worker->id, dev->id, dev->vqs - vq);
			return false;
		}
	}
	/* They could have slipped one in as we were doing that: make
	 * sure it's written, then check again. */
	smp_mb();
	r = __get_user(avail_idx, &vq->avail->idx);
	if (r) {
		vq_err(vq, "Failed to check avail idx at %p: %d\n",
		       &vq->avail->idx, r);
//		vhost_printk("END - worker %d, dev %d, vq %d", dev->worker->id, dev->id, dev->vqs - vq);
		return false;
	}

//	vhost_printk("END - worker %d, dev %d, vq %d", dev->worker->id, dev->id, dev->vqs - vq);
	return avail_idx != vq->avail_idx;
}
EXPORT_SYMBOL_GPL(vhost_enable_notify);

/* We don't need to be notified again. */
void vhost_disable_notify(struct vhost_dev *dev, struct vhost_virtqueue *vq)
{
	int r;
//	vhost_printk("START - worker %d, dev %d, vq %d", dev->worker->id, dev->id, dev->vqs - vq);
	/* If this virtqueue is vqpoll.enabled, and on the polling list, it
	 * will generate notifications even if the guest is asked not to send
	 * them. So we must remove it from the round-robin polling list.
	 * Note that vqpoll.enabled remains set.
	 */
	if (vq->vqpoll.enabled) {
		if(!list_empty(&vq->vqpoll.link))
			list_del_init(&vq->vqpoll.link);
//		vhost_printk("END - worker %d, dev %d, vq %d", dev->worker->id, dev->id, dev->vqs - vq);
		return;
	}

	if (vq->used_flags & VRING_USED_F_NO_NOTIFY){
//		vhost_printk("END - worker %d, dev %d, vq %d", dev->worker->id, dev->id, dev->vqs - vq);
		return;
	}
	vq->used_flags |= VRING_USED_F_NO_NOTIFY;
	if (!vhost_has_feature(vq, VIRTIO_RING_F_EVENT_IDX)) {
		r = vhost_update_used_flags(vq);
		if (r)
			vq_err(vq, "Failed to enable notification at %p: %d\n",
			       &vq->used->flags, r);
	}
//	vhost_printk("END - worker %d, dev %d, vq %d", dev->worker->id, dev->id, dev->vqs - vq);
}
EXPORT_SYMBOL_GPL(vhost_disable_notify);

//static int vhost_fs_get_bool_from_user(const char __user *buffer,
//		unsigned long count, bool *out){
//	int err, length;
//	char kbuf[16];
//
//	if (count > (sizeof(kbuf) - 1))
//		return -EINVAL;
//
//	vhost_printk("buffer = %p, count = %lu.\n", buffer, count);
//	if ((err = copy_from_user(kbuf, buffer, count)) > 0){
//		vhost_printk("copy from user wasn't able to copy %d bytes.\n", err);
//		return -EINVAL;
//	}
//	kbuf[count] = '\0';
//	kbuf[count] = '\0';
//	length = strlen(kbuf);
//	if (kbuf[length - 1] == '\n')
//		kbuf[--length] = '\0';
//
//	if ((err = strtobool(kbuf, out))){
//		vhost_printk("strtobool falied with %d.\n", err);
//		return err;
//	}
//	return 0;
//}

static struct vhost_worker *vhost_get_worker(int worker_id){
	struct vhost_worker *worker = NULL;
	int found = 0;
	spin_lock_irq(&workers_pool.workers_lock);
	list_for_each_entry(worker, &workers_pool.workers_list, node) {
		if (worker->id == worker_id) {
			found = 1;
			vhost_printk("found a worker with id = w.%d, worker = %p", worker_id, worker);
			break;
		}
	}
	spin_unlock_irq(&workers_pool.workers_lock);
	vhost_printk("worker_id = %d, return worker id %p", worker_id, worker);
	return found == 1? worker: NULL;
}

static int vhost_fs_get_worker_from_string(const char *buffer,
		unsigned long count, struct vhost_worker **out){
	long worker_id;
	int err;
	vhost_printk("START\n");
	if (count < 3){
		vhost_warn("Error: count was too short, only %lu\n", count);
		return -EINVAL;
	}
	if (buffer[0] != 'w' || buffer[1] != '.'){
		vhost_warn("Error: buffer doesn't start with 'w.' but with: '%c%c'\n",
				buffer[0], buffer[1]);
		return -EINVAL;
	}
	if ((err = kstrtol(buffer + 2, 0, &worker_id)) != 0){
		vhost_warn("Error: could not convert buffer: %s to number\n",
				buffer + 2);
		return err;
	}
	if ((*out = vhost_get_worker(worker_id)) == NULL){
		vhost_warn("Error: could not find worker w.%ld\n", worker_id);
		return -EINVAL;
	}
	vhost_printk("END\n");
	return 0;
}

static ssize_t vhost_fs_get_epoch(struct class *class,
		struct class_attribute *attr, char *buf){
	ssize_t length;
	vhost_printk("START\n");
	length = sprintf(buf, "%d\n", atomic_read(&epoch));
	vhost_printk("page = %s length = %ld\n", buf, length);
	vhost_printk("DONE!\npage = %s length = %ld\n",
			buf, length);
	return length;
}

static ssize_t vhost_fs_inc_epoch(struct class *class,
		struct class_attribute *attr,
		const char *buffer, size_t count){
	long value;
	int err;
	vhost_printk("START\n");
	if ((err = kstrtol(buffer, 0, &value)) != 0){
		vhost_warn("Error: %d.\n", err);
		return err;
	}
	if (value != 1){
		vhost_warn("Error: %d.\n", -EINVAL);
		return -EINVAL;
	}
	atomic_inc(&epoch);
	vhost_printk("DONE!\n");
	return count;
}

static ssize_t vhost_fs_get_cycles(struct class *class,
		struct class_attribute *attr, char *buf){
	ssize_t length;
	u64 end, cycles;
	vhost_printk("START\n");
	rdtscll(end);
	cycles = end - cycles_start_tsc;
	length = sprintf(buf, "%llu\n", cycles);
	vhost_printk("page = %s length = %ld\n", buf, length);
	vhost_printk("DONE!\npage = %s length = %ld\n",
			buf, length);
	return length;
}

//static ssize_t vhost_fs_status(struct class *class, struct class_attribute *attr,
//		char *buf);
//
//static ssize_t vhost_fs_show_all_class_attributes(struct class *class,
//		struct class_attribute *attrs, size_t attrs_count, char *buf){
//	int i = 0;
//	int error;
//	ssize_t length = 0;
//	length = sprintf(buf, "%s:\n", class->name);
//	for (; i < attrs_count; ++i){
//		if (attrs[i].show == NULL || attrs[i].show == vhost_fs_status)
//			continue;
//		length += sprintf(buf + length, "%s:", attrs[i].attr.name);
//		if (IS_ERR_VALUE(error = attrs[i].show(class, &attrs[i], buf + length)))
//			return error;
//
//		length += error;
////		length += sprintf(buf + length, "\n");
//	}
//	return length;
//}
//
//struct vhost_fs_status_struct {
//	char *buf;
//	ssize_t length;
//
//	struct dev_ext_attribute *attrs;
//	size_t attrs_count;
//};
//
//static int vhost_fs_show_all_dir_attributes(struct device *dev, void *data){
//	struct vhost_fs_status_struct *status = (struct vhost_fs_status_struct *)data;
//	int i = 0;
//	int res;
//	status->length += sprintf(status->buf + status->length, "%s:\n", dev_name(dev));
//	for (; i < status->attrs_count; ++i){
//		if (status->attrs[i].attr.show == NULL)
//			continue;
//		status->length += sprintf(status->buf + status->length, "%s:",
//				status->attrs[i].attr.attr.name);
//		if (IS_ERR_VALUE(res = status->attrs[i].attr.show(dev, &status->attrs[i].attr,
//				status->buf + status->length))){
//			return res;
//		}
//		status->length += res;
//	}
//	return 0;
//}
//
//static ssize_t vhost_fs_status(struct class *class, struct class_attribute *attr,
//		char *buf){
//	ssize_t length = 0;
//	int res;
//	struct vhost_fs_status_struct status;
//
//	vhost_printk("START\n");
//	// print global attributes
//	if (IS_ERR_VALUE(res = vhost_fs_show_all_class_attributes(class,
//			vhost_fs_global_attrs,
//			ARRAY_LENGTH(vhost_fs_global_attrs), buf + length))){
//		return res;
//	}
//	length += res;
//	vhost_printk("length = %ld\n", length);
//	// print global workers attributes
//	status.buf = buf + length;
//	status.length = 0;
//	status.attrs = vhost_fs_global_worker_attrs;
//	status.attrs_count = ARRAY_LENGTH(vhost_fs_global_worker_attrs);
//	if (IS_ERR_VALUE(res = vhost_fs_show_all_dir_attributes(vhost_fs_workers,
//			&status))){
//		return res;
//	}
//	length += status.length;
//	vhost_printk("length = %ld\n", length);
//	// print each worker attributes
//	status.buf = buf + length;
//	status.length = 0;
//	status.attrs = vhost_fs_per_worker_attrs;
//	status.attrs_count = ARRAY_LENGTH(vhost_fs_per_worker_attrs);
//	if (IS_ERR_VALUE(res = device_for_each_child(vhost_fs_workers, &status,
//			vhost_fs_show_all_dir_attributes))){
//		return res;
//	}
//	length += status.length;
//	vhost_printk("length = %ld\n", length);
//	// print each device attributes
//	status.buf = buf + length;
//	status.length = 0;
//	status.attrs = vhost_fs_device_attrs;
//	status.attrs_count = ARRAY_LENGTH(vhost_fs_device_attrs);
//	if (IS_ERR_VALUE(res = device_for_each_child(vhost_fs_devices, &status,
//			vhost_fs_show_all_dir_attributes))){
//		return res;
//	}
//	length += status.length;
//	vhost_printk("length = %ld\n", length);
//	// print each queue attributes
//	status.buf = buf+length;
//	status.length = 0;
//	status.attrs = vhost_fs_queue_attrs;
//	status.attrs_count = ARRAY_LENGTH(vhost_fs_queue_attrs);
//	if (IS_ERR_VALUE(res = device_for_each_child(vhost_fs_queues, &status,
//			vhost_fs_show_all_dir_attributes))){
//		return res;
//	}
//	length += status.length;
//
//	vhost_printk("DONE! length = %ld\n", length);
//	return length;
//}

ssize_t vhost_fs_get_recent_new_workers(struct device *dev,
		struct device_attribute *attr, char *buf){
	ssize_t length = 0;
	int i = 0;
	vhost_printk("START\n");
	for (i=0; i<VHOST_WORKERS_POOL_NEW_WORKERS_SIZE; ++i){
		if (workers_pool.new_workers[i] == VHOST_WORKERS_POOL_NEW_WORKERS_VACANT){
			continue;
		}
		length += sprintf(buf + length, "w.%d\t", workers_pool.new_workers[i]);
		workers_pool.new_workers[i] = VHOST_WORKERS_POOL_NEW_WORKERS_VACANT;
		if (length >= PAGE_SIZE){
			break;
		}
	}
	if (length > 0)
		buf[length - 1] = '\n';
	vhost_printk("page = %s length = %ld\n", buf, length);
	vhost_printk("DONE!\npage = %s length = %ld\n", buf, length);
	return length;
}

ssize_t vhost_fs_create_new_worker(struct device *dev,
		struct device_attribute *attr, const char *buffer, size_t count){
	int res;
	int err;
	vhost_printk("START\n");
	if ((err = kstrtoint(buffer, 0, &res)) != 0){
		vhost_warn("Error: getting int: %d.\n", err);
		return err;
	}
	if (res < -1 || res >= num_possible_cpus()){
		/* invalid cpu number */
		vhost_warn("Error: invalid cpu number, %d.\n", res);
		return -EINVAL;
	}
	create_new_worker(res, workers_pool.default_worker == NULL);
	vhost_printk("DONE: return value is %lu\n", count);
	return count;
}

ssize_t vhost_fs_remove_worker(struct device *dev, struct device_attribute *attr,
		 const char *buffer, size_t count){
	struct vhost_worker *worker;
	int num_devices;
	int worker_id;
	int err;
	vhost_printk("START\n");
	if ((err = vhost_fs_get_worker_from_string(buffer, count, &worker)) != 0){
		vhost_warn("Error: getting worker: %d.\n", err);
		return err;
	}
	worker_id = worker->id;
	if (atomic_read(&worker->state) != VHOST_WORKER_STATE_LOCKED){
		/* worker still is not set as locked. */
		vhost_warn("Error: worker w.%d is not set as locked.\n", worker_id);
		return -EINVAL;
	}
	num_devices = atomic_read(&worker->num_devices);
	if (num_devices != 0){
		vhost_warn("Error: worker w.%d is still assigned to %d device(s)\n",
				worker_id, num_devices);
		/* worker still is still assigned to devices */
		return -EINVAL;
	}
	if (vhost_worker_remove(worker) == 0){
		/* worker still has some work to process in the work_list */
		vhost_warn("Error: worker w.%d still has some work to process in the "
				"work_list - BUG.\n", worker_id);
		return -EINVAL;
	}
	vhost_printk("DONE: return value is %lu\n", count);
	return count;
}

ssize_t vhost_fs_get_default_worker(struct device *dev,
		struct device_attribute *attr, char *buf){
	ssize_t length;
	struct vhost_worker *worker = NULL;
	int worker_id;
	vhost_printk("START\n");
	worker = workers_pool.default_worker;
	if (worker == NULL){
		length = 0;
	}else{
		worker_id = worker->id;
		length = sprintf(buf, "w.%d\n", worker_id);
	}
	vhost_printk("DONE!\npage = %s length = %ld\n", buf, length);
	return length;
}

ssize_t vhost_fs_set_default_worker(struct device *dev,
		struct device_attribute *attr, const char *buffer, size_t count){
	struct vhost_worker *worker;
	int err;
	vhost_printk("START\n");
	if ((err = vhost_fs_get_worker_from_string(buffer, count, &worker)) != 0){
		vhost_printk("VHOST_FS_FILE_GLOBAL_WORKER_DEFAULT: error getting "
				"worker: %d.\n", err);
		return err;
	}
	if (!workers_pool_set_default_worker_safe(worker)){
		/* worker is locked */
		vhost_warn("Error: worker w.%d locked.\n", worker->id);
		return -EINVAL;
	}
	vhost_printk("DONE: return value is %lu\n", count);
	return count;
}

ssize_t vhost_fs_worker_get_locked(struct device *dev,
		struct device_attribute *attr, char *buf){
	ssize_t length = 0;
	struct vhost_worker *worker = (struct vhost_worker *)dev_get_drvdata(dev);
	vhost_printk("START: worker %d\n", worker->id);
	length = sprintf(buf, "%d\n",
			atomic_read(&worker->state) != VHOST_WORKER_STATE_NORMAL);
	vhost_printk("DONE!\npage = %s length = %ld\n", buf, length);
	return length;
}

ssize_t vhost_fs_worker_set_locked(struct device *dev,
		 struct device_attribute *attr, const char *buffer, size_t count){
	struct vhost_worker *worker = (struct vhost_worker *)dev_get_drvdata(dev);
	int err;
	bool lock;
	vhost_printk("START: worker %d\n", worker->id);
	if ((err = strtobool(buffer, &lock))){
		vhost_warn("Error: getting bool: %d.\n", err);
		return err;
	}
	if (lock){
		if (!vhost_worker_set_locked(worker)){
			vhost_warn("Error: worker w.%d cannot set locked.\n", worker->id);
			return -EINVAL;
		}
	}else{
		if (!vhost_worker_set_unlocked(worker)){
			vhost_warn("Error: worker w.%d cannot be unlocked.\n", worker->id);
			return -EINVAL;
		}
	}
	vhost_printk("DONE: return value is %lu\n", count);
	return count;
}

ssize_t vhost_fs_worker_get_cpu(struct device *dev,
		struct device_attribute *attr, char *buf){
	ssize_t length = 0;
	struct vhost_worker *worker = (struct vhost_worker *)dev_get_drvdata(dev);
	vhost_printk("START: worker %d\n", worker->id);
	length = sprintf(buf, "%d\n", worker_get_cpu(worker));
	vhost_printk("DONE!\npage = %s length = %ld\n", buf, length);
	return length;
}

ssize_t vhost_fs_worker_get_pid(struct device *dev,
		struct device_attribute *attr, char *buf){
	ssize_t length = 0;
	struct vhost_worker *worker = (struct vhost_worker *)dev_get_drvdata(dev);
	vhost_printk("START: worker %d\n", worker->id);
	length = sprintf(buf, "%d\n", worker->worker_thread->pid);
	vhost_printk("DONE!\npage = %s length = %ld\n", buf, length);
	return length;
}

ssize_t vhost_fs_worker_get_dev_list(struct device *dev,
		struct device_attribute *attr, char *buf){
	ssize_t length = 0;
	struct vhost_worker *worker = (struct vhost_worker *)dev_get_drvdata(dev);
	struct vhost_dev *device = NULL;
	vhost_printk("START: worker w.%d\n", worker->id);
	spin_lock_irq(&dev_table.entries_lock);
	list_for_each_entry(device, &dev_table.entries, vhost_dev_table_entry) {
		if (device->worker == worker){
			length += sprintf(buf + length, "d.%d\t", device->id);
			if (length >= PAGE_SIZE){
				break;
			}
		}
	}
	spin_unlock_irq(&dev_table.entries_lock);
	if (length > 0)
		buf[length - 1] = '\n';
	vhost_printk("DONE!\npage = %s length = %ld\n", buf, length);
	return length;
}

ssize_t vhost_fs_device_get_worker(struct device *dir,
		struct device_attribute *attr, char *buf){
	ssize_t length = 0;
	struct vhost_dev *dev = (struct vhost_dev *)dev_get_drvdata(dir);
	vhost_printk("START: dev d.%d\n", dev->id);
	length = sprintf(buf, "w.%d\n", dev->worker->id);
	vhost_printk("DONE!\npage = %s length = %ld\n", buf, length);
	return length;
	return 0;
}

ssize_t vhost_fs_device_set_worker(struct device *dir,
		struct device_attribute *attr, const char *buffer, size_t count){
	struct vhost_dev *device = (struct vhost_dev *)dev_get_drvdata(dir);
	struct vhost_worker *worker = NULL;
	int err;
	vhost_printk("START: device d.%d\n", device->id);
	if ((err = vhost_fs_get_worker_from_string(buffer, count, &worker)) != 0){
		vhost_warn("Error: getting worker: "
				"%d.\n", err);
		return err;
	}
	if (device->worker == worker){
		vhost_printk("device d.%d the device is already assigned to the "
				"designated worker d.%d.\n", device->id, worker->id);
		return count;
	}
	if(vhost_dev_transfer_to_worker(device, worker) == 0){
		vhost_warn("Error: device %d is already in transfer.\n", device->id);
		return -EINVAL;
	}
	vhost_printk("DONE: return value is %lu\n",	count);
	return count;
}

ssize_t vhost_fs_device_get_owner(struct device *dir,
		struct device_attribute *attr, char *buf){
	ssize_t length = 0;
	struct vhost_dev *dev = (struct vhost_dev *)dev_get_drvdata(dir);
	vhost_printk("START: dev %d\n", dev->id);
	length = sprintf(buf, "%d\n", dev->owner? dev->owner->pid : -1);
	vhost_printk("DONE!\npage = %s length = %ld\n", buf, length);
	return length;
	return 0;
}

ssize_t vhost_fs_device_get_vq_list(struct device *dir,
		struct device_attribute *attr, char *buf){
	ssize_t length = 0;
	int i;
	struct vhost_dev *dev = (struct vhost_dev *)dev_get_drvdata(dir);
	vhost_printk("device %d\n",dev->id);
	for (i=0; i<dev->nvqs; ++i){
		length += sprintf(buf + length, "vq.%d.%d\t", dev->id, i);
		if (length >= PAGE_SIZE){
			break;
		}
	}
	if (length > 0)
		buf[length - 1] = '\n';
	vhost_printk("DONE!\npage = %s length = %ld\n", buf, length);
	return length;
}

ssize_t vhost_fs_queue_get_poll(struct device *dir,
		struct device_attribute *attr, char *buf){
	ssize_t length = 0;
	struct vhost_virtqueue *vq = (struct vhost_virtqueue *)dev_get_drvdata(dir);
	vhost_printk("START: vq.%d.%d\n", vq->dev->id, vq->id);
	length = sprintf(buf, "%d\n", vq->vqpoll.enabled);
	vhost_printk("DONE!\npage = %s length = %ld\n", buf, length);
	return length;
	return 0;
}

ssize_t vhost_fs_queue_set_poll(struct device *dir,
		struct device_attribute *attr, const char *buffer, size_t count){
	struct vhost_virtqueue *vq = (struct vhost_virtqueue *)dev_get_drvdata(dir);
	int err;
	bool poll;
	vhost_printk("START: %d.%d\n", vq->dev->id, vq->id);
	if ((err = strtobool(buffer, &poll))){
		vhost_warn("Error: getting bool: %d.\n", err);
		return err;
	}
	if (poll){
		vhost_printk("enable");
		vhost_vq_enable_vqpoll(vq);
	} else {
		vhost_printk("disable");
		vhost_vq_disable_vqpoll(vq, false);
	}
	vhost_printk("DONE: return value is %lu\n", count);
	return count;
}

ssize_t vhost_fs_queue_get_device(struct device *dir,
		struct device_attribute *attr, char *buf){
	ssize_t length = 0;
	struct vhost_virtqueue *vq = (struct vhost_virtqueue *)dev_get_drvdata(dir);
	vhost_printk("START: vq.%d.%d\n", vq->dev->id, vq->id);
	length = sprintf(buf, "d.%d\n", vq->dev->id);
	vhost_printk("DONE!\npage = %s length = %ld\n", buf, length);
	return length;
	return 0;
}

module_init(vhost_init);
module_exit(vhost_exit);

MODULE_VERSION("0.0.1");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Michael S. Tsirkin");
MODULE_DESCRIPTION("Host kernel accelerator for virtio");
