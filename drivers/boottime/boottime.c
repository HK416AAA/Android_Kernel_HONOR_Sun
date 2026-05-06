// SPDX-License-Identifier: GPL-2.0
/*
 * boot time implementation
 *
 * Copyright (c) 2021-2025 Honor Technologies Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define LOG_TAG "BOOTTIME "

#define BOOT_STR_SIZE 128 // max len of string.
#define BOOT_LOG_NUM 256 // max size of boot log stored.
#define BOOT_50_MS 50000
#define ARCH_TIMER_RATE_VALID 1000000

#define BOOTUP_DONE "[INFOR]_wm_boot_animation_done"
struct boot_log_struct {
	u32 time;
	u32 ktime;
	char event[BOOT_STR_SIZE];
} boottime[BOOT_LOG_NUM];

static int boot_log_count;
static DEFINE_MUTEX(boottime_lock);
static int boottime_enabled = 1;

void log_boot(char *str);
static u32 arch_timer_rate;


/**
 * do_boottime_initcall - record duration time while calling initcall function.
 *   if the duration time is bigger than the threshold value, this function will save
 *   the duration time into locall buffer.
 * @fn: initcall function.
 *
 * This function returns whatever initcall function returns.
 */
int __init_or_module do_boottime_initcall(initcall_t fn)
{
	int ret;
	unsigned long long duration;
	ktime_t calltime, delta, rettime;
	char log_info[BOOT_STR_SIZE] = {0};

	calltime = ktime_get();
	ret = fn();
	rettime = ktime_get();
	delta = ktime_sub(rettime, calltime);
	duration = (unsigned long long)ktime_to_ns(delta) >> 10;
	if (duration > BOOT_50_MS) {
		snprintf(log_info, sizeof(log_info), "[WARNING] %pS %lld usecs",
			 fn, duration);
		log_boot(log_info);
	}
	return ret;
}

EXPORT_SYMBOL(do_boottime_initcall);

/**
 * log_boot - saving information into local buffer, boottime[].
 *   max size of boottime buffer is BOOT_LOG_NUM.
 * @str: information to be recorded.
 */
void log_boot(char *str)
{
	u64 tc, ts;

	mutex_lock(&boottime_lock);
	if (!str || !boottime_enabled || boot_log_count >= BOOT_LOG_NUM) {
		mutex_unlock(&boottime_lock);
		return;
	}

	tc = arch_timer_read_counter();
	ts = sched_clock();

	if (arch_timer_rate > 0) {
		boottime[boot_log_count].time = tc * MSEC_PER_SEC / arch_timer_rate;
		boottime[boot_log_count].ktime = ts / NSEC_PER_MSEC;
		strscpy((char *)&boottime[boot_log_count].event, str, BOOT_STR_SIZE);
		boot_log_count++;
	}
	mutex_unlock(&boottime_lock);
}

static int boottime_show(struct seq_file *m, void *v)
{
	int i, cnt;

	mutex_lock(&boottime_lock);
	cnt = boot_log_count;
	mutex_unlock(&boottime_lock);

	seq_puts(m, "time\tktime\tboot events\n");
	seq_puts(m, "----------------------------------------------------------\n");
	for (i = 0; i < cnt; i++) {
		seq_printf(m, "%u\t%u\t%s\n", boottime[i].time,
				boottime[i].ktime, boottime[i].event);
	}
	seq_puts(m, "----------------------------------------------------------\n");
	seq_printf(m, "%s\n", boottime_enabled ? "starting..." : "start done");
	return 0;
}

static int boottime_open(struct inode *inode, struct file *file)
{
	return single_open(file, boottime_show, inode->i_private);
}

static ssize_t boottime_write(struct file *filp, const char *ubuf, size_t cnt,
				  loff_t *data)
{
	char buf[BOOT_STR_SIZE] = {0};
	size_t copy_size = cnt;

	if (cnt >= sizeof(buf))
		copy_size = BOOT_STR_SIZE - 1;
	if (copy_from_user(&buf, ubuf, copy_size))
		return -EFAULT;

	if (cnt == 1) {
		if (buf[0] == '0') {
			boottime_enabled = 0; // boot up complete
			return 1;
		} else if (buf[0] == '1') {
			boottime_enabled = 1;
			return 1;
		}
	}
	buf[copy_size] = 0;
	log_boot(buf);
	return cnt;
}

static const struct proc_ops boottime_fops = {
	.proc_open = boottime_open,
	.proc_write = boottime_write,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int boot_time_init(void)
{
	struct proc_dir_entry *pe;

	arch_timer_rate = arch_timer_get_cntfrq();
	if (arch_timer_rate < ARCH_TIMER_RATE_VALID) {
		pr_err(LOG_TAG "arch_timer_rate '%u' invalid\n", arch_timer_rate);
		return -EINVAL;
	}

	pe = proc_create("boottime", 0664, NULL, &boottime_fops);
	if (!pe) {
		pr_err(LOG_TAG "failed to create '/proc/boottime'\n");
		return -ENOMEM;
	}
	return 0;
}

static void __exit boot_time_exit(void)
{
	remove_proc_entry("boottime", NULL);
}

MODULE_AUTHOR("Honor");
MODULE_DESCRIPTION("Boot Time Recoder");
MODULE_LICENSE("GPL");

module_init(boot_time_init);
module_exit(boot_time_exit);
