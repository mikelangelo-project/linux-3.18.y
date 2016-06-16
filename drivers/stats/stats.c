#include <linux/module.h>	/* Needed by all modules */
#include <linux/kernel.h>	/* Needed for KERN_INFO */
#include <linux/init.h>		/* Needed for the macros */

#include <linux/kernel_stat.h>
#include <linux/sched.h>
#include <linux/pid.h>

#include <linux/device.h>

#define MODULE_NAME "stats"

#define MAX_PIDS 100
#define ARRAY_LENGTH(x)  (sizeof(x) / sizeof(x[0]))

cputime_t *utime = NULL;
cputime_t *stime = NULL;


static ssize_t stats_fs_get_kstat_addr(struct class *class,
	struct class_attribute *attr, char *buf);
static ssize_t stats_fs_get_kernel_cpustat_addr(struct class *class,
				struct class_attribute *attr, char *buf);
static ssize_t stats_fs_get_pidstat_addr(struct class *class,
				struct class_attribute *attr, char *buf);
static ssize_t stats_fs_set_pid(struct class *class,
		struct class_attribute *attr, const char *buf, size_t count);

static struct class *stats_fs_class;
static struct class_attribute stats_fs_attrs[] = {
	__ATTR(stat_addr, S_IRUSR | S_IRGRP| S_IROTH, stats_fs_get_kstat_addr, NULL),
	__ATTR(cpustat_addr, S_IRUSR | S_IRGRP| S_IROTH,
		stats_fs_get_kernel_cpustat_addr, NULL),
	__ATTR(pidstat, S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP | S_IROTH,
				stats_fs_get_pidstat_addr, stats_fs_set_pid),
};

static ssize_t stats_fs_get_kstat_addr(struct class *class,
	struct class_attribute *attr, char *buf)
{
	int i;
	ssize_t length = 0;

	for_each_possible_cpu(i) {
		length += sprintf(buf + length, "%p\t", &kstat_cpu(i));
	}
	if (length > 0)
		buf[length - 1] = '\n';
	return length;
}

static ssize_t stats_fs_get_kernel_cpustat_addr(struct class *class,
				struct class_attribute *attr, char *buf)
{
	int i;
	ssize_t length = 0;

	for_each_possible_cpu(i) {
		length += sprintf(buf + length, "%p\t", &kcpustat_cpu(i).cpustat);
	}
	if (length > 0)
		buf[length - 1] = '\n';
	return length;
}

static ssize_t stats_fs_get_pidstat_addr(struct class *class,
				struct class_attribute *attr, char *buf)
{
	ssize_t length = 0;

	if (utime != NULL && stime != NULL){
		length += sprintf(buf, "%p\t%p\n", utime, stime);
	}
	return length;
}

static ssize_t stats_fs_set_pid(struct class *class,
		struct class_attribute *attr, const char *buffer, size_t count)
{
	struct task_struct *task = NULL;
	struct pid *pid = NULL;
	int nr;
	int err;

	if ((err = kstrtoint(buffer, 0, &nr)) != 0){
		printk("unknown number.\n");
		return err;
	}

	if ((pid = find_get_pid(nr)) == NULL) {
		printk("no pid struct for pid: %d.\n", nr);
		return -EINVAL;
	}

	if ((task = get_pid_task(pid, PIDTYPE_PID)) == NULL) {
		printk("no task struct found for pid: %d.\n", nr);
		return -EINVAL;
	}

	utime = &task->utime;
	stime = &task->stime;
	return count;
}

static int __init init_main(void)
{
	int i = 0;
	stats_fs_class = class_create(THIS_MODULE, MODULE_NAME);
	if (IS_ERR(stats_fs_class)){
		WARN(IS_ERR(stats_fs_class), "couldn't create class, err = %ld\n",
				PTR_ERR(stats_fs_class));
		return -1;
	}

	for (; i < ARRAY_LENGTH(stats_fs_attrs); ++i){
		int res;
		printk("creating file %s.\n", stats_fs_attrs[i].attr.name);
		res = class_create_file(stats_fs_class, &stats_fs_attrs[i]);
		if (res < 0){
			WARN(res < 0, "couldn't create class file %s, err = %d.\n",
				stats_fs_attrs[i].attr.name, res);
			return -1;
		}
	}

	printk("kernel stats up.\n");
  return 0;
}

static void __exit cleanup_main(void)
{
	int i;
	for (i = 0; i < ARRAY_LENGTH(stats_fs_attrs); ++i){
		printk("removing file %s.\n", stats_fs_attrs[i].attr.name);
		class_remove_file(stats_fs_class, &stats_fs_attrs[i]);
	}
	class_destroy(stats_fs_class);
  printk("kernel stats down.\n");
}

module_init(init_main);
module_exit(cleanup_main);

MODULE_VERSION("0.0.1");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Eyal Moscovici");
MODULE_DESCRIPTION("misc kernel statistics");
