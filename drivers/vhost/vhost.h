#ifndef _VHOST_H
#define _VHOST_H

#include <linux/eventfd.h>
#include <linux/vhost.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>
#include <linux/virtio_net.h>
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/delay.h>

#if 0
#define vhost_warn(msg, ...) \
	WARN(1, "vhost-debug: %s:%d: "msg"\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)

#define vhost_printk(msg, ...) \
	printk("vhost-debug: %s:%d: "msg"\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)

#define read_tsc(x) rdtscll(x)
#else
#define vhost_warn(msg, ...)
#define vhost_printk(msg, ...)
#define read_tsc(x)
#endif

struct vhost_device;
struct vhost_work;

/*
 * A function pointer to a function performing the work
 */
typedef void (*vhost_work_fn_t)(struct vhost_work *work);


/*
 * struct vhost_work - A vhost piece of work.
 *	@node: worker thread list of works 
 *	@fn: the work function
 *	@done: processes waiting for the work completion
 *	@flushing; 1 if the waiting processes are notified once on work completion
 *	@queue_seq: 1 if the work has been assigned to a worker
 *	@done_seq: 1 if the work is done but not yet cleared
 */

struct vhost_work {
	struct list_head	  node;
	vhost_work_fn_t		  fn;
	wait_queue_head_t	  done;
	int			  flushing;
	unsigned		  queue_seq;
	unsigned		  done_seq;
	/* a worker might handle work items from different devices thus now we need
	   to know the owner of each work item.
	 */
	struct vhost_virtqueue    *vq;
	spinlock_t lock;
	u64 arrival_cycles;
};

/* Poll a file (eventfd or socket) */
/* Note: there's nothing vhost specific about this structure. */
struct vhost_poll {
	poll_table                table;
	wait_queue_head_t        *wqh;
	wait_queue_t              wait;
	struct vhost_work	  work;
	unsigned long		  mask;
	struct vhost_dev	 *dev;
};

void vhost_work_init(struct vhost_work *work, struct vhost_virtqueue *vq, vhost_work_fn_t fn);
void vhost_work_flush(struct vhost_dev *dev, struct vhost_work *work);
void vhost_work_queue(struct vhost_dev *dev, struct vhost_work *work);

void vhost_poll_init(struct vhost_poll *poll, vhost_work_fn_t fn,
		unsigned long mask, struct vhost_virtqueue  *vq);
int vhost_poll_start(struct vhost_poll *poll, struct file *file);
void vhost_poll_stop(struct vhost_poll *poll);
void vhost_poll_flush(struct vhost_poll *poll);
void vhost_poll_queue(struct vhost_poll *poll);
void vhost_work_flush(struct vhost_dev *dev, struct vhost_work *work);
long vhost_vring_ioctl(struct vhost_dev *d, int ioctl, void __user *argp);
bool vhost_can_continue(struct vhost_virtqueue  *vq, size_t processed_data);

struct vhost_log {
	u64 addr;
	u64 len;
};

struct ubuf_info;

/* The virtqueue structure describes a queue attached to a device. */
struct vhost_virtqueue {
	struct vhost_dev *dev;

	/* The actual ring of buffers. */
	struct mutex mutex;
	unsigned int num;
	struct vring_desc __user *desc;
	struct vring_avail __user *avail;
	struct vring_used __user *used;
	struct file *kick;
	struct file *call;
	struct file *error;
	struct eventfd_ctx *call_ctx;
	struct eventfd_ctx *error_ctx;
	struct eventfd_ctx *log_ctx;

	struct vhost_poll poll;

	/* The routine to call when the Guest pings us, or timeout. */
	vhost_work_fn_t handle_kick;

	/* Last available index we saw. */
	u16 last_avail_idx;

	/* Caches available index value from user. */
	u16 avail_idx;

	/* Last index we used. */
	u16 last_used_idx;

	/* Used flags */
	u16 used_flags;

	/* Last used index value we have signalled on */
	u16 signalled_used;

	/* Last used index value we have signalled on */
	bool signalled_used_valid;

	/* Log writes to used structure. */
	bool log_used;
	u64 log_addr;

	struct iovec iov[UIO_MAXIOV];
	struct iovec *indirect;
	struct vring_used_elem *heads;
	/* Protected by virtqueue mutex. */
	struct vhost_memory *memory;
	void *private_data;
	unsigned acked_features;
	/* Log write descriptors */
	void __user *log_base;
	struct vhost_log *log;
	int id;
	/* The minimum amount of bytes to be processed in a single service
	 * cycle
	 */
	size_t min_processed_data_limit;

	/* The maximum amount of bytes to be processed in a single service
	 * cycle
	 */
	size_t max_processed_data_limit;
	struct device *vhost_fs_dev;
	struct {
		u64 poll_kicks; /* number of kicks in poll mode */
		u64 poll_cycles; /* cycles spent handling kicks in poll mode */
		u64 poll_bytes; /* bytes sent/received by kicks in poll mode */
		u64 poll_wait; /* cycles elapsed between poll kicks */
		u64 poll_empty; /* number of times the queue was empty during poll */
		u64 poll_empty_cycles; /* number of cycles elapsed while the queue was empty */
		u64 poll_coalesced; /* number of times this queue was coalesced */
		u64 poll_limited; /* number of times the queue was limited by netweight during poll kicks*/
		u64 poll_pending_cycles; /* cycles elapsed between item arrival and poll */
		u64 notif_works; /* number of works in notif mode */
		u64 notif_cycles; /* cycles spent handling works in notif mode */
		u64 notif_bytes; /* bytes sent/received by works in notif mode */
		u64 notif_wait; /* cycles elapsed between work arrival and handling in notif mode */
		u64 notif_limited; /* number of times the queue was limited by netweight in notif mode */

		u64 handled_bytes; /* bytes sent/received */
		u64 handled_packets; /* bytes sent/received */

		u64 ring_full; /* number of times the ring was full */

		u64 stuck_times; /* how many times this queue was stuck and limited other queues */
		u64 stuck_cycles; /* total amount of cycles the queue was stuck */

		u64 last_poll_tsc_end; /* tsc when the last poll finished */
		u64 last_notif_tsc_end; /* tsc when the last notif finished */
		u64 last_poll_empty_tsc; /* tsc when the queue was detected empty for the first time */
		u64 handled_bytes_this_work; /* number of bytes handled by this queue in the last poll/notif. Must be updated by the concrete vhost implementations (vhost-net)*/
		u64 was_limited; /* flag indicating if the queue was limited by net-weight during the last poll/notif. Must be updated by the concrete vhost implementations (vhost-net)*/

		u64 almost_full_times; /* the number of times the queue was almost full  */
		
		u64 ksoftirq_occurrences; /* number of times a softirq occurred during the processing of this queue */
		u64 ksoftirq_time; /* time (ns) that softirq occurred during the processing of this queue */
		u64 ksoftirqs; /* the number of softirq interruts handled during the processing of this queue */
	} stats;
	struct {
		/* When a virtqueue is in vqpoll.enabled mode, it declares
		 * that instead of using guest notifications (kicks) to
		 * discover new work, we prefer to continuously poll this
		 * virtqueue in the worker thread.
		 * If !enabled, the rest of the fields below are undefined.
		 */
		bool enabled;
	 	/* vqpoll.enabled doesn't always mean that this virtqueue is
		 * actually being polled: The backend (e.g., net.c) may
		 * temporarily disable it using vhost_disable/enable_notify().
		 * vqpoll.link is used to maintain the thread's round-robin
		 * list of virtqueus that actually need to be polled.
		 * Note list_empty(link) means this virtqueue isn't polled.
		 */
		struct list_head link;
		/* If this flag is true, the virtqueue is being shut down,
		 * so vqpoll should not be re-enabled.
		 */
		bool shutdown;
		/* The number of items that were pending the last time we checked if it
		 * this virtual queue is stuck */
		u32 last_pending_items;

		/* TSC when we detected for the first time the queue was stuck
		   Used to measure how many cycles the queue has been stuck
		 */
		u64 stuck_cycles;

		/* The number of cycles need to elapse without service to consider the
		 * queue stuck
		 */
		int max_stuck_cycles;

		/* The queue won't be considered stuck if it contains more then this
		 * amount of items. This parameter is used in order to avoid classifying
		 * "bursty" queue as a stuck queue (disable = -1).
		 */
		int max_stuck_pending_items;

		/* The minimum rate in which a polled queue can be polled (disable = 0)
		 * Note: This is designed for non-latency sensitive queues, when we want
		 * to avoid sending a lot of virtual interrupts to the guest.
		 */
		u64 min_poll_rate;

		/* The number of pending items to consider the queue almost full.
		 */
		u32 max_almost_full_pending_items;
		/* virtqueue.avail is a userspace pointer, and each vhost
		 * device may have a different process context, so polling
		 * different vhost devices could involve page-table switches
		 * (and associated TLB flushes), which hurts performance when
		 * adding nearly-idle guests. So instead, we pin these pages
		 * in memory and keep a kernel-mapped pointer to each, so
		 * polling becomes simple memory reads.
		 */
		struct page *avail_page;
		volatile struct vring_avail *avail_mapped;
	} vqpoll;
};

/* The device structure describes an I/O device working with vhost. */
struct vhost_dev {
	struct vhost_memory *memory;
	struct mm_struct *mm;
	struct task_struct *owner;
	struct mutex mutex;
	struct vhost_virtqueue **vqs;
	int nvqs;
	struct file *log_file;
	struct eventfd_ctx *log_ctx;
	struct vhost_worker *worker;
	int id;
	struct device *vhost_fs_dev;
	struct{
//		u64 encrypt_device; /* encrypt the content sent to the device, only available for blk */
		u64 delay_per_work; /* the number of loops per work we have to delay the calculation. */
		u64 delay_per_kbyte; /* the number of loops per kbyte we have to delay the calculation. */
		u64 device_move_total;
		u64 device_move_count;
		u64 device_detach;
		u64 device_attach;
	} stats;
	struct{
		/* The device operation mode. has two operation modes.
		 * 1. VHOST_DEVICE_OPERATION_MODE_NORMAL - The device can receive new
		 *    work. and the worker assigned to it can process them. At this
		 *    state the device CANNOT be transfered safely.
		 * 2. VHOST_DEVICE_OPERATION_MODE_TRANSFERRING - Device operation mode
		 *    during transfer. Arriving new works are attached to the device's
		 *    suspended_work_list to be later processed by the new worker. The
		 *    old worker processes any existing work in its work_list and keeps
		 *    polling the devices virtual queues until it comes across a "detach
		 *    device work" that removes the virtual queues of the device from
		 *    the the old worker's vqpoll_list. The last step in the transfer
		 *    work is to queue an "attach device work" in the new worker.
		 *    The new worker then changes the worker pointer of the device. Adds
		 *    all polled queues to its vqpoll_list. Added all suspended work to
		 *    its work_list. finally the new worker set the operation mode of
		 *    the device back to normal.
		 */
		atomic_t operation_mode;
		spinlock_t suspended_work_lock;
		struct list_head suspended_work_list;
		int suspended_works;
	} transfer;
	struct list_head vhost_dev_table_entry;
};

/* Worker structure, a worker thread performing the real I/O operations. */
struct vhost_worker {
	spinlock_t work_lock;
	struct list_head work_list;
	/* The number of cycles need to elapse to consider the work_list stuck
	 * (0 = disabled) */
	u64 work_list_max_stuck_cycles;
	struct task_struct *worker_thread;

	/* num of devices this worker is currently handling */
	atomic_t num_devices;
	/* worker id */
	int id;
	/* Worker state. The worker has two states:
	 * 1. VHOST_WORKER_STATE_NORMAL - The worker can be assigned new unassigned
	 *    devices, and devices can be transfered to it.
	 *
	 * 2. VHOST_WORKER_STATE_LOCKED - The worker cannot be assigned new
	 *    unassigned devices, and devices cannot be transfered to it. The aim of
	 *    this state is to allow for a user-space program to remove existing
	 *    workers by first marking them as a LOCKED worker, then transferring
	 *    all the devices assigned to it to the other workers. The last step in
	 *    removing the worker, after all devices were transfered away is to is
	 *    to send it a remove control.
	 *
	 * 3.  VHOST_WORKER_STATE_SHUTDOWN - The worker is about to be shutdown.
	 * 	   in order to shutdown a worker it must first be locked, and has no
	 * 	   devices assigned to it. The worker thread will shutdown once all
	 * 	   the works in its work_list are done.
	 */
	atomic_t state;
	/* linked workers list */
	struct list_head node;
	/* tsc when the last work was processed from the work_list */
	u64 last_work_tsc;
	struct device *vhost_fs_dev;
	struct {
		u64 loops; /* number of loops performed */
		u64 enabled_interrupts; /* number of times interrupts were re-enabled */
		u64 tsc_cycles; /* current tsc read */
		u64 cycles; /* cycles spent in the worker, excluding cycles doing queue work */
		u64 total_cycles; /* cycles spent in the worker */
		u64 mm_switches; /* number of times the mm was switched */
		u64 wait; /* number of cycles the worker thread was not running after schedule */
		u64 empty_works; /* number of times there were no works in the queue -- ignoring poll kicks  */
		u64 empty_polls; /* number of times there were no queues to poll and the polling queue was not empty  */
		u64 stuck_works; /* number of times were detected stuck and limited queues */
		u64 noqueue_works; /* number of works which have no queue related to them (e.g. vhost-net rx) */
		u64 pending_works; /* number of pending works */

		u64 last_loop_tsc_end; /* tsc when the last loop was performed */

		u64 poll_cycles; /* cycles spent handling kicks in poll mode */		
		u64 notif_cycles; /* cycles spent handling works in notif mode */
		u64 total_work_cycles; /* total cycles spent handling works */

		u64 ksoftirq_occurrences; /* number of times a softirq occured during worker work */
		u64 ksoftirq_time; /* time (ns) that softirq process took while worker processed its work */
		u64 ksoftirqs; /* the number of softirq interruts handled during worker processed its work */

	} stats;
	struct list_head vqpoll_list;
	/* The maximum number of cycles a worker disable software interrupts while
	 * processing virtio queues. */
	int max_disabled_soft_interrupts_cycles;
};

enum {
	/* the size of the new workers array located in workers pool. */
	VHOST_WORKERS_POOL_NEW_WORKERS_SIZE = 32,
	/* A vacant spot in the new workers array located in workers pool. */
	VHOST_WORKERS_POOL_NEW_WORKERS_VACANT = -1
};

/* The pool of all the worker threads in vhost */
struct vhost_workers_pool {
	/* list of active workers */
	struct list_head workers_list;
	/* lock to protect the workers list */
	spinlock_t workers_lock;
	/* last worker id */
	int last_worker_id;
	struct vhost_worker *default_worker;

	/* An array of new workers. when a new worker is added it should
	 * write its id in the first vacant spot. vacant spots are marked by
	 * VHOST_WORKERS_POOL_NEW_WORKERS_VACANT.
	 * Upon reading the create file we write back the ids of the new workers
	 * and vacant the spot in the new_worker array. */
	int new_workers[VHOST_WORKERS_POOL_NEW_WORKERS_SIZE];
};


/* A table containing all the devices simulated by vhost. */
struct vhost_dev_table {
	/* list of active devices */
	struct list_head entries;
	/* lock to protect the device table */
	spinlock_t entries_lock;
};

int vhost_dev_table_add(struct vhost_dev *dev);
int vhost_dev_table_remove(struct vhost_dev *dev);
struct vhost_dev *vhost_dev_table_get(int device_id);


void vhost_dev_init(struct vhost_dev *, struct vhost_virtqueue **vqs, int nvqs);
long vhost_dev_set_owner(struct vhost_dev *dev);
bool vhost_dev_has_owner(struct vhost_dev *dev);
long vhost_dev_check_owner(struct vhost_dev *);
struct vhost_memory *vhost_dev_reset_owner_prepare(void);
void vhost_dev_reset_owner(struct vhost_dev *, struct vhost_memory *);
void vhost_dev_cleanup(struct vhost_dev *, bool locked);
void vhost_dev_stop(struct vhost_dev *);
long vhost_dev_ioctl(struct vhost_dev *, unsigned int ioctl, void __user *argp);
#define vhost_dev_add_delay_per_work(dev) __delay((dev)->stats.delay_per_work)
#define vhost_dev_add_delay_per_kbyte(dev, len) \
	__delay((dev)->stats.delay_per_kbyte * (len << 10))
long vhost_vring_ioctl(struct vhost_dev *d, int ioctl, void __user *argp);
int vhost_vq_access_ok(struct vhost_virtqueue *vq);
int vhost_log_access_ok(struct vhost_dev *);

int vhost_get_vq_desc(struct vhost_virtqueue *,
		      struct iovec iov[], unsigned int iov_count,
		      unsigned int *out_num, unsigned int *in_num,
		      struct vhost_log *log, unsigned int *log_num);
void vhost_discard_vq_desc(struct vhost_virtqueue *, int n);

int vhost_init_used(struct vhost_virtqueue *);
int vhost_add_used(struct vhost_virtqueue *, unsigned int head, int len);
int vhost_add_used_n(struct vhost_virtqueue *, struct vring_used_elem *heads,
		     unsigned count);
void vhost_add_used_and_signal(struct vhost_dev *, struct vhost_virtqueue *,
			       unsigned int id, int len);
void vhost_add_used_and_signal_n(struct vhost_dev *, struct vhost_virtqueue *,
			       struct vring_used_elem *heads, unsigned count);
void vhost_signal(struct vhost_dev *, struct vhost_virtqueue *);
void vhost_disable_notify(struct vhost_dev *, struct vhost_virtqueue *);
bool vhost_enable_notify(struct vhost_dev *, struct vhost_virtqueue *);

int vhost_log_write(struct vhost_virtqueue *vq, struct vhost_log *log,
		    unsigned int log_num, u64 len);

#define vq_err(vq, fmt, ...) do {                                  \
		pr_debug(pr_fmt(fmt), ##__VA_ARGS__);       \
		if ((vq)->error_ctx)                               \
				eventfd_signal((vq)->error_ctx, 1);\
	} while (0)

enum {
	VHOST_FEATURES = (1ULL << VIRTIO_F_NOTIFY_ON_EMPTY) |
			 (1ULL << VIRTIO_RING_F_INDIRECT_DESC) |
			 (1ULL << VIRTIO_RING_F_EVENT_IDX) |
			 (1ULL << VHOST_F_LOG_ALL),
};

static inline int vhost_has_feature(struct vhost_virtqueue *vq, int bit)
{
	return vq->acked_features & (1 << bit);
}

void vhost_enable_zcopy(int vq);

#endif
