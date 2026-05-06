/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Honor Technologies Co., Ltd.
 */
#ifndef _LINUX_HC_H
#define _LINUX_HC_H

#include <linux/timex.h>
#include <linux/workqueue.h>    /* for work queue */
#include <linux/slab.h>         /* for kmalloc() */

#ifdef CONFIG_HONOR_F2FS_HOTNESS_CLUSTERING

#define DEF_HC_THREAD_MIN_SLEEP_TIME	120000	/*    2 mins     */
#define DEF_HC_THREAD_MAX_SLEEP_TIME	3840000 /*    64 mins   */

#define DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD	1000000
#define DEF_HC_HOTNESS_ENTRY_SHRINK_NUM		100000
#define DEF_HC_THREAD_DELTA_BLOCKS		100000
#define MAX_HOTNESS_ENTRY		1500000	/* extend plug to save entry before run reduce */
#define DEF_HC_MIN_CLUSTER_THRESHOLD	1000

#define DEF_IRR_UPDATE_WEIGHT		30

struct f2fs_hc_kthread {
	struct task_struct *f2fs_hc_task;
	wait_queue_head_t hc_wait_queue_head;

	/* for hc sleep time */
	unsigned int min_sleep_time;
	unsigned int max_sleep_time;
};

int insert_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, __u64 value, int type);
int update_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr_old, block_t blkaddr_new,
		__u64 value, int type_old, int type_new);
__u64 lookup_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, int type);
void reduce_hotness_entry(struct f2fs_sb_info *sbi);
void release_hotness_entry(struct f2fs_sb_info *sbi);

int hotness_decide(struct f2fs_io_info *fio, int type_old, int type_origin, __u64 *value_ptr);
void hotness_maintain(struct f2fs_io_info *fio, int type_old, int type_new, __u64 value);
static inline void hc_decrease_sleep_time(struct f2fs_hc_kthread *hc_th, unsigned int *wait)
{
	unsigned int min_time = hc_th->min_sleep_time;

	if ((long long)((*wait)>>1) <= (long long)min_time)
		*wait = min_time;
	else
		*wait = ((*wait)>>1);
}

static inline void hc_increase_sleep_time(struct f2fs_hc_kthread *hc_th, unsigned int *wait)
{
	unsigned int max_time = hc_th->max_sleep_time;

	if ((long long)((*wait)<<1) >= (long long)max_time)
		*wait = max_time;
	else
		*wait = ((*wait)<<1);
}

#endif
#endif
