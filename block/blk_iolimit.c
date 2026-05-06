/*
 * Copyright (c) Honor Technologies Co., Ltd. 2020-2024. All rights reserved.
 * Description: Add iolimit policy for blkcg
 * History: From kernel 6.6.0 use blkio policy instead of cgroup subsystem.
 * Note: This file can only be used after kernel 6.6.0. For old kernels before,
 *       please use cgroup_io_limit.c
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cgroup.h>
#include <linux/atomic.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/version.h>
#include "blk-cgroup.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#define ANDROID_OEM_DATA_IOSMART 1
#endif

static inline bool blk_cant_iolimit(struct blkcg *blkcg)
{
#ifdef ANDROID_OEM_DATA_IOSMART
	struct blkcg_oem_data *p_oem_data =
		(struct blkcg_oem_data *)&blkcg->android_oem_data1;
	return (p_oem_data->type <= BLK_THROTL_KBG);
#else
	return (blkcg->type <= BLK_THROTL_KBG);
#endif
}

int blk_iolimit_init(struct gendisk *disk);
void blk_iolimit_exit(struct gendisk *disk);

void do_io_write_bandwidth_control(size_t count);
void do_io_read_bandwidth_control(size_t count);

inline void io_read_bandwidth_control(size_t count)
{
	if (unlikely(!task_css_is_root(current, io_cgrp_id)))
		do_io_read_bandwidth_control(count);
}
inline void io_write_bandwidth_control(size_t count)
{
	bool ret;

	rcu_read_lock();
	ret = task_css_is_root(current, io_cgrp_id);
	rcu_read_unlock();

	if (unlikely(!ret))
		do_io_write_bandwidth_control(count);
}
inline void pagefault_io_read_bandwidth_control(size_t count)
{
	task_set_in_pagefault(current);
	do_io_read_bandwidth_control(count);
	task_clear_in_pagefault(current);
}
inline void io_generic_read_bandwidth_control(size_t count)
{
	if (unlikely(!task_css_is_root(current, io_cgrp_id)))
		pagefault_io_read_bandwidth_control(count);
}

#define WAIT_INTERVAL_MS (125)
#define WAIT_PARTS_NUM (8)

enum Switch_Stat {
	STAT_OFF,
	STAT_ON,
};

static struct blkcg_policy iolimit_policy;

/**
 * struct iolimit_blkcg - Per (cgroup, request queue) data.
 * @pd: blkg_policy_data structure.
 */
struct iolimit_blkg {
	struct blkg_policy_data pd;
};

/**
 * struct iolimit_cpd - Per cgroup data.
 * @cpd: blkcg_policy_data structure.
 */
struct iolimit_blkcg {
	struct blkcg_policy_data cpd;

	atomic64_t switching;
	atomic64_t write_limit;
	s64 write_part_nbyte;
	s64 write_already_used;
	struct timer_list write_timer;
	spinlock_t write_lock;
	wait_queue_head_t write_wait;

	atomic64_t read_limit;
	s64 read_part_nbyte;
	s64 read_already_used;
	struct timer_list read_timer;
	spinlock_t read_lock;
	wait_queue_head_t read_wait;
};

static inline struct iolimit_blkg *pd_to_iolimit(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct iolimit_blkg, pd) : NULL;
}

static inline struct iolimit_blkcg *blkcg_to_iolimit(struct blkcg *blkcg)
{
	return container_of(blkcg_to_cpd(blkcg, &iolimit_policy),
			    struct iolimit_blkcg, cpd);
}

static inline struct iolimit_blkcg *css_iolimit(struct cgroup_subsys_state *css)
{
	return blkcg_to_iolimit(css_to_blkcg(css));
}

static inline struct iolimit_blkcg *task_iolimit(struct task_struct *tsk)
{
	return css_iolimit(task_css(tsk, io_cgrp_id));
}

static int is_need_iolimit(struct iolimit_blkcg *iolimitcg)
{
	int ret = 0;

	struct blkcg *blkcg = task_blkcg(current);

	if (blk_cant_iolimit(blkcg))
		return 0;

	ret = signal_pending_state(TASK_INTERRUPTIBLE, current);
	if (ret == TASK_INTERRUPTIBLE)
		return 0;

	return atomic64_read(&iolimitcg->switching);
}

static bool is_write_need_wakeup(struct iolimit_blkcg *iolimitcg)
{
	int ret = false;
	struct blkcg *blkcg = NULL;

	if (atomic64_read(&iolimitcg->switching) == 0)
		ret = true;

	if (iolimitcg->write_part_nbyte > iolimitcg->write_already_used)
		ret = true;

	rcu_read_lock();
	if (iolimitcg != task_iolimit(current))
		ret = true;

	blkcg = task_blkcg(current);
	if (blk_cant_iolimit(blkcg))
		ret = true;

	rcu_read_unlock();
	return ret;
}

static bool is_read_need_wakeup(struct iolimit_blkcg *iolimitcg)
{
	int ret = false;
	struct blkcg *blkcg = NULL;

	if (atomic64_read(&iolimitcg->switching) == 0)
		ret = true;

	if (iolimitcg->read_part_nbyte > iolimitcg->read_already_used)
		ret = true;

	rcu_read_lock();
	if (iolimitcg != task_iolimit(current))
		ret = true;

	blkcg = task_blkcg(current);
	if (blk_cant_iolimit(blkcg))
		ret = true;

	rcu_read_unlock();
	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
void do_io_write_bandwidth_control(size_t count)
{
	size_t may_io_cnt;
	struct iolimit_blkcg *iolimitcg = NULL;
	struct cgroup_subsys_state *blkio_css = NULL;

repeat:
	rcu_read_lock();
	iolimitcg = task_iolimit(current);
	if (!is_need_iolimit(iolimitcg)) {
		rcu_read_unlock();
		return;
	}

	spin_lock_bh(&iolimitcg->write_lock);
	may_io_cnt =
		iolimitcg->write_part_nbyte - iolimitcg->write_already_used;
	if (may_io_cnt < count) {
		spin_unlock_bh(&iolimitcg->write_lock);
		blkio_css = task_css(current, io_cgrp_id);
		if (css_tryget_online(blkio_css)) {
			rcu_read_unlock();
			/*lint -save -e666*/
			wait_event_interruptible_timeout(
				iolimitcg->write_wait,
				is_write_need_wakeup(iolimitcg),
				msecs_to_jiffies(WAIT_INTERVAL_MS));
			/*lint -restore*/
			css_put(blkio_css);
		} else {
			rcu_read_unlock();
		}
		goto repeat;
	} else {
		iolimitcg->write_already_used += count;
	}

	spin_unlock_bh(&iolimitcg->write_lock);
	rcu_read_unlock();
}

void do_io_read_bandwidth_control(size_t count)
{
	size_t may_io_cnt;
	struct iolimit_blkcg *iolimitcg = NULL;
	struct cgroup_subsys_state *blkio_css = NULL;

repeat:
	rcu_read_lock();
	iolimitcg = task_iolimit(current);
	if (!is_need_iolimit(iolimitcg)) {
		rcu_read_unlock();
		return;
	}

	spin_lock_bh(&iolimitcg->read_lock);
	may_io_cnt = iolimitcg->read_part_nbyte - iolimitcg->read_already_used;
	if (may_io_cnt < count) {
		spin_unlock_bh(&iolimitcg->read_lock);
		blkio_css = task_css(current, io_cgrp_id);
		if (css_tryget_online(blkio_css)) {
			rcu_read_unlock();
			/*lint -save -e666*/
			wait_event_interruptible_timeout(
				iolimitcg->read_wait,
				is_read_need_wakeup(iolimitcg),
				msecs_to_jiffies(WAIT_INTERVAL_MS));
			/*lint -restore*/
			css_put(blkio_css);
		} else {
			rcu_read_unlock();
		}

		if (task_in_pagefault(current))
			return;
		goto repeat;
	} else {
		iolimitcg->read_already_used += count;
	}

	spin_unlock_bh(&iolimitcg->read_lock);
	rcu_read_unlock();
}
#else
void do_io_write_bandwidth_control(size_t count)
{
	size_t may_io_cnt;
	struct iolimit_blkcg *iolimitcg = NULL;
	size_t fragment_count;
	struct cgroup_subsys_state *blkio_css = NULL;

	while (count) {
		if (count > PAGE_SIZE) {
			fragment_count = PAGE_SIZE;
			count -= PAGE_SIZE;
		} else {
			fragment_count = count;
			count = 0;
		}
repeat:
		rcu_read_lock();
		iolimitcg = task_iolimit(current);
		if (!is_need_iolimit(iolimitcg)) {
			rcu_read_unlock();
			return;
		}

		spin_lock_bh(&iolimitcg->write_lock);
		may_io_cnt = iolimitcg->write_part_nbyte -
			     iolimitcg->write_already_used;
		if (may_io_cnt < fragment_count) {
			spin_unlock_bh(&iolimitcg->write_lock);
			blkio_css = task_css(current, io_cgrp_id);
			if (css_tryget_online(blkio_css)) {
				rcu_read_unlock();
				/*lint -save -e666*/
				wait_event_interruptible_timeout(
					iolimitcg->write_wait,
					is_write_need_wakeup(iolimitcg),
					msecs_to_jiffies(WAIT_INTERVAL_MS));
				/*lint -restore*/
				css_put(blkio_css);
			} else {
				rcu_read_unlock();
			}
			goto repeat;
		}
		iolimitcg->write_already_used += fragment_count;
		spin_unlock_bh(&iolimitcg->write_lock);
		rcu_read_unlock();
	}
}

void do_io_read_bandwidth_control(size_t count)
{
	size_t may_io_cnt;
	struct iolimit_blkcg *iolimitcg = NULL;
	size_t fragment_count;
	struct cgroup_subsys_state *blkio_css = NULL;

	while (count) {
		if (count > PAGE_SIZE) {
			fragment_count = PAGE_SIZE;
			count -= PAGE_SIZE;
		} else {
			fragment_count = count;
			count = 0;
		}
repeat:
		rcu_read_lock();
		iolimitcg = task_iolimit(current);
		if (!is_need_iolimit(iolimitcg)) {
			rcu_read_unlock();
			return;
		}

		spin_lock_bh(&iolimitcg->read_lock);
		may_io_cnt = iolimitcg->read_part_nbyte -
			     iolimitcg->read_already_used;
		if (may_io_cnt < fragment_count) {
			spin_unlock_bh(&iolimitcg->read_lock);
			blkio_css = task_css(current, io_cgrp_id);
			if (css_tryget_online(blkio_css)) {
				rcu_read_unlock();
				/*lint -save -e666*/
				wait_event_interruptible_timeout(
					iolimitcg->read_wait,
					is_read_need_wakeup(iolimitcg),
					msecs_to_jiffies(WAIT_INTERVAL_MS));
				/*lint -restore*/
				css_put(blkio_css);
			} else {
				rcu_read_unlock();
			}

			if (task_in_pagefault(current))
				return;
			goto repeat;
		}
		iolimitcg->read_already_used += fragment_count;
		spin_unlock_bh(&iolimitcg->read_lock);
		rcu_read_unlock();
	}
}
#endif

static void handle_write_timer(struct iolimit_blkcg *iolimitcg)
{
	if (!iolimitcg)
		return;
	spin_lock_bh(&iolimitcg->write_lock);
	iolimitcg->write_already_used = 0;
	spin_unlock_bh(&iolimitcg->write_lock);
	wake_up_all(&iolimitcg->write_wait);
	mod_timer(&iolimitcg->write_timer, jiffies + (HZ / WAIT_PARTS_NUM));
}

static void handle_read_timer(struct iolimit_blkcg *iolimitcg)
{
	if (!iolimitcg)
		return;
	spin_lock_bh(&iolimitcg->read_lock);
	iolimitcg->read_already_used = 0;
	spin_unlock_bh(&iolimitcg->read_lock);
	wake_up_all(&iolimitcg->read_wait);
	mod_timer(&iolimitcg->read_timer, jiffies + (HZ / WAIT_PARTS_NUM));
}

/* timer_list->function prototype changed in v4.15 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
static void write_timer_handler(unsigned long data)
{
	struct iolimit_blkcg *iolimitcg = (struct iolimit_blkcg *)data;

	handle_write_timer(iolimitcg);
}

static void read_timer_handler(unsigned long data)
{
	struct iolimit_blkcg *iolimitcg = (struct iolimit_blkcg *)data;

	handle_read_timer(iolimitcg);
}

static void init_write_timer(struct iolimit_blkcg *iolimitcg)
{
	setup_timer(&iolimitcg->write_timer, write_timer_handler,
		    (unsigned long)iolimitcg);
}

static void init_read_timer(struct iolimit_blkcg *iolimitcg)
{
	setup_timer(&iolimitcg->read_timer, read_timer_handler,
		    (unsigned long)iolimitcg);
}
#else
static void write_timer_handler(struct timer_list *t)
{
	struct iolimit_blkcg *iolimitcg = from_timer(iolimitcg, t, write_timer);

	handle_write_timer(iolimitcg);
}

static void read_timer_handler(struct timer_list *t)
{
	struct iolimit_blkcg *iolimitcg = from_timer(iolimitcg, t, read_timer);

	handle_read_timer(iolimitcg);
}

static void init_write_timer(struct iolimit_blkcg *iolimitcg)
{
	timer_setup(&iolimitcg->write_timer, write_timer_handler, 0);
}

static void init_read_timer(struct iolimit_blkcg *iolimitcg)
{
	timer_setup(&iolimitcg->read_timer, read_timer_handler, 0);
}
#endif

static struct blkg_policy_data *iolimit_alloc_pd(struct gendisk *disk,
						 struct blkcg *blkcg, gfp_t gfp)
{
	struct iolimit_blkg *iolimit_blkg;

	iolimit_blkg = kzalloc(sizeof(*iolimit_blkg), gfp);
	if (!iolimit_blkg)
		return NULL;

	return &iolimit_blkg->pd;
}

static void iolimit_free_pd(struct blkg_policy_data *pd)
{
	struct iolimit_blkg *iolimit_blkg = pd_to_iolimit(pd);

	kfree(iolimit_blkg);
}

static struct blkcg_policy_data *iolimit_alloc_cpd(gfp_t gfp)
{
	struct iolimit_blkcg *iolimitcg;

	iolimitcg = kzalloc(sizeof(*iolimitcg), gfp);
	if (!iolimitcg)
		return NULL;

	atomic64_set(&iolimitcg->switching, 0);

	atomic64_set(&iolimitcg->write_limit, 0);
	iolimitcg->write_part_nbyte = 0;
	iolimitcg->write_already_used = 0;
	init_write_timer(iolimitcg);
	spin_lock_init(&iolimitcg->write_lock);
	init_waitqueue_head(&iolimitcg->write_wait);

	atomic64_set(&iolimitcg->read_limit, 0);
	iolimitcg->read_part_nbyte = 0;
	iolimitcg->read_already_used = 0;
	init_read_timer(iolimitcg);
	spin_lock_init(&iolimitcg->read_lock);
	init_waitqueue_head(&iolimitcg->read_wait);

	return &iolimitcg->cpd;
}

static void iolimit_free_cpd(struct blkcg_policy_data *cpd)
{
	struct iolimit_blkcg *iolimitcg =
		container_of(cpd, typeof(*iolimitcg), cpd);

	del_timer_sync(&iolimitcg->write_timer);
	del_timer_sync(&iolimitcg->read_timer);

	kfree(iolimitcg);
}

static s64 iolimit_switching_read(struct cgroup_subsys_state *css,
				  struct cftype *cft)
{
	struct iolimit_blkcg *iolimitcg = NULL;

	if (!css)
		return -EINVAL;

	iolimitcg = css_iolimit(css);
	return atomic64_read(&iolimitcg->switching);
}

static int iolimit_switching_write(struct cgroup_subsys_state *css,
				   struct cftype *cft, s64 switching)
{
	struct iolimit_blkcg *iolimitcg = NULL;
	int err = 0;

	if (((switching != STAT_OFF) && (switching != STAT_ON)) || !css) {
		err = -EINVAL;
		goto out;
	}

	iolimitcg = css_iolimit(css);
	atomic64_set(&iolimitcg->switching, switching);
	if (switching == STAT_OFF) {
		wake_up_all(&iolimitcg->write_wait);
		del_timer_sync(&iolimitcg->write_timer);

		wake_up_all(&iolimitcg->read_wait);
		del_timer_sync(&iolimitcg->read_timer);
	} else {
		mod_timer(&iolimitcg->write_timer,
			  jiffies + (HZ / WAIT_PARTS_NUM));
		iolimitcg->write_already_used = iolimitcg->write_part_nbyte;

		mod_timer(&iolimitcg->read_timer,
			  jiffies + (HZ / WAIT_PARTS_NUM));
		iolimitcg->read_already_used = iolimitcg->read_part_nbyte;
	}
out:
	return err;
}

static s64 writeiolimit_read(struct cgroup_subsys_state *css,
			     struct cftype *cft)
{
	struct iolimit_blkcg *iolimitcg = NULL;

	if (!css)
		return -EINVAL;

	iolimitcg = css_iolimit(css);
	return atomic64_read(&iolimitcg->write_limit);
}

static int writeiolimit_write(struct cgroup_subsys_state *css,
			      struct cftype *cft, s64 limit)
{
	struct iolimit_blkcg *iolimitcg = NULL;
	int err = 0;

	if ((limit <= 0) || !css) {
		err = -EINVAL;
		goto out;
	}

	iolimitcg = css_iolimit(css);
	atomic64_set(&iolimitcg->write_limit, limit);
	spin_lock_bh(&iolimitcg->write_lock);
	iolimitcg->write_part_nbyte = limit / WAIT_PARTS_NUM;
	spin_unlock_bh(&iolimitcg->write_lock);
out:
	return err;
}

static s64 readiolimit_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct iolimit_blkcg *iolimitcg = NULL;

	if (!css)
		return -EINVAL;

	iolimitcg = css_iolimit(css);
	return atomic64_read(&iolimitcg->read_limit);
}

static int readiolimit_write(struct cgroup_subsys_state *css,
			     struct cftype *cft, s64 limit)
{
	struct iolimit_blkcg *iolimitcg = NULL;
	int err = 0;

	if ((limit <= 0) || !css) {
		err = -EINVAL;
		goto out;
	}

	iolimitcg = css_iolimit(css);
	atomic64_set(&iolimitcg->read_limit, limit);
	spin_lock_bh(&iolimitcg->read_lock);
	iolimitcg->read_part_nbyte = limit / WAIT_PARTS_NUM;
	spin_unlock_bh(&iolimitcg->read_lock);
out:
	return err;
}

static struct cftype iolimit_legacy_files[] = {
	{
		.name = "iolimit.switching",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_s64 = iolimit_switching_read,
		.write_s64 = iolimit_switching_write,
	},
	{
		.name = "iolimit.write_limit",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_s64 = writeiolimit_read,
		.write_s64 = writeiolimit_write,
	},
	{
		.name = "iolimit.read_limit",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_s64 = readiolimit_read,
		.write_s64 = readiolimit_write,
	},
	{}};

static struct blkcg_policy iolimit_policy = {
	.legacy_cftypes = iolimit_legacy_files,

	.cpd_alloc_fn = iolimit_alloc_cpd,
	.cpd_free_fn = iolimit_free_cpd,

	.pd_alloc_fn = iolimit_alloc_pd,
	.pd_free_fn = iolimit_free_pd,
};

void blk_iolimit_exit(struct gendisk *disk)
{
	blkcg_deactivate_policy(disk, &iolimit_policy);
}

int blk_iolimit_init(struct gendisk *disk)
{
	int retval = blkcg_activate_policy(disk, &iolimit_policy);
	return retval;
}

static int __init iolimit_init(void)
{
	int retval = blkcg_policy_register(&iolimit_policy);
	return retval;
}

static void __exit iolimit_exit(void)
{
	blkcg_policy_unregister(&iolimit_policy);
}

module_init(iolimit_init);
module_exit(iolimit_exit);
