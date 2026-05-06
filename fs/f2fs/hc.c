/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Honor Technologies Co., Ltd.
 */
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/init.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/freezer.h>
#include <linux/sched/signal.h>
#include <linux/random.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#ifdef CONFIG_HONOR_F2FS_HOTNESS_CLUSTERING
#include "hc.h"
#include "kmeans.h"

int insert_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, __u64 value, int type)
{
	int ret = 0;

	if (atomic_read(&sbi->hi->count) >= MAX_HOTNESS_ENTRY)
		goto out;

	if (!mutex_trylock(&sbi->hi->hotness_tree_lock[type]))
		goto out;
	ret = radix_tree_insert(&sbi->hi->hotness_rt_array[type], blkaddr, (void *) value);
	if (ret == -EEXIST) {
		if (radix_tree_delete(&sbi->hi->hotness_rt_array[type], blkaddr))
			atomic_dec(&sbi->hi->count);
		ret = radix_tree_insert(&sbi->hi->hotness_rt_array[type], blkaddr, (void *) value);
	}
	mutex_unlock(&sbi->hi->hotness_tree_lock[type]);

	if (!ret)
		atomic_inc(&sbi->hi->count);
out:
	sbi->hi->new_blk_cnt++;
	return ret;
}

int update_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr_old, block_t blkaddr_new,
		__u64 value, int type_old, int type_new)
{
	void *entry = NULL;
	int ret = 0;

	if (!mutex_trylock(&sbi->hi->hotness_tree_lock[type_old]))
		goto insert;
	entry = radix_tree_delete(&sbi->hi->hotness_rt_array[type_old], blkaddr_old);
	mutex_unlock(&sbi->hi->hotness_tree_lock[type_old]);
	if (entry)
		atomic_dec(&sbi->hi->count);
insert:
	if (atomic_read(&sbi->hi->count) >= MAX_HOTNESS_ENTRY)
		goto out;
	if (!mutex_trylock(&sbi->hi->hotness_tree_lock[type_new]))
		goto out;
	ret = radix_tree_insert(&sbi->hi->hotness_rt_array[type_new], blkaddr_new, (void *) value);
	if (ret == -EEXIST) {
		if (radix_tree_delete(&sbi->hi->hotness_rt_array[type_new], blkaddr_new))
			atomic_dec(&sbi->hi->count);
		ret = radix_tree_insert(&sbi->hi->hotness_rt_array[type_new], blkaddr_new,
			(void *) value);
	}
	mutex_unlock(&sbi->hi->hotness_tree_lock[type_new]);
	if (!ret)
		atomic_inc(&sbi->hi->count);
out:
	sbi->hi->upd_blk_cnt++;
	if (blkaddr_old != blkaddr_new)
		sbi->hi->opu_blk_cnt++;
	else
		sbi->hi->ipu_blk_cnt++;
	return 0;
}
__u64 lookup_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, int type)
{
	void *value;

	if (type >= TEMP_TYPE_NUM || type < 0)
		return 0;
	rcu_read_lock();
	value = radix_tree_lookup(&sbi->hi->hotness_rt_array[type], blkaddr);
	rcu_read_unlock();
	if (value)
		return (__u64) value;
	return 0;
}

void reduce_hotness_entry(struct f2fs_sb_info *sbi)
{
	struct radix_tree_iter iter;
	void __rcu **slot;
	int type;
	unsigned int count = 0;

	for (type = TEMP_TYPE_NUM - 1; type >= 0; type--) {
		mutex_lock(&sbi->hi->hotness_tree_lock[type]);
		radix_tree_for_each_slot(slot, &sbi->hi->hotness_rt_array[type], &iter, 0) {
			radix_tree_iter_delete(&sbi->hi->hotness_rt_array[type], &iter, slot);
			atomic_dec(&sbi->hi->count);
			if ((count++ >= DEF_HC_HOTNESS_ENTRY_SHRINK_NUM) &&
			(atomic_read(&sbi->hi->count) < DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD)) {
				mutex_unlock(&sbi->hi->hotness_tree_lock[type]);
				goto out;
			}
		}
		mutex_unlock(&sbi->hi->hotness_tree_lock[type]);
	}
out:
	sbi->hi->rmv_blk_cnt += count;
}

static unsigned long long __calculate_block_irr(__u64 value, __u64 lws)
{
	unsigned int rem_old, rem_new;
	unsigned long long res;
	unsigned int weight = DEF_IRR_UPDATE_WEIGHT;
	__u64 lws_old = value >> 32;
	__u32 irr_new, irr_old;

	irr_old = (value & __INT32_MAX__) >> 2;
	if (lws > lws_old)
		irr_new = lws - lws_old;
	else
		irr_new = __INT32_MAX__ - lws_old + lws;

	if (irr_old == (__INT32_MAX__>>2))
		return irr_new;
	res = div_u64_rem(irr_new, 100, &rem_new) * (100 - weight)
		+ div_u64_rem(irr_old, 100, &rem_old) * weight;

	if (rem_new)
		res += rem_new * (100 - weight) / 100;
	if (rem_old)
		res += rem_old * weight / 100;

	if (res > __INT32_MAX__ >> 2)
		res = __INT32_MAX__ >> 2;
	return res;
}

int hotness_decide(struct f2fs_io_info *fio, int type_old, int type_origin, __u64 *value_ptr)
{
	__u64 value = 0, lws;
	__u32 irr, irr1;
	int type_new;
	struct f2fs_sb_info *sbi = fio->sbi;
	struct hotness_info *hi = fio->sbi->hi;

	if (!hi)
		return type_origin;

	lws = atomic_read(&sbi->total_writed_block_count);
	if (type_old != -1)
		value = lookup_hotness_entry(fio->sbi, fio->old_blkaddr, type_old);
	if (!value) {
		irr = __INT32_MAX__ >> 2;
		irr1 = irr << 2;
		value = (lws << 32) + irr1;
		type_new = type_origin;
	} else {
		irr = __calculate_block_irr(value, lws);
		irr1 = irr << 2;
		value = (lws << 32) + irr1;
		if (fio->sbi->centers_valid)
			type_new = kmeans_get_type(fio, irr);
		else
			type_new = type_old;
	}

	if (IS_HOT(type_new))
		fio->temp = HOT;
	else if (IS_WARM(type_new))
		fio->temp = WARM;
	else
		fio->temp = COLD;

	atomic_inc(&hi->blk_cnt_temp[fio->temp]);
	if (irr != (__INT32_MAX__ >> 2)) {
		hi->irr_min[fio->temp] = min(hi->irr_min[fio->temp], irr);
		hi->irr_max[fio->temp] = max(hi->irr_max[fio->temp], irr);
	}
	atomic_inc(&sbi->total_writed_block_count);
	*value_ptr = value;

	return type_new;
}

void hotness_maintain(struct f2fs_io_info *fio, int type_old, int type_new, __u64 value)
{
	if (!fio->sbi->hi)
		return;
	if (type_old > CURSEG_COLD_DATA || type_new > CURSEG_COLD_DATA)
		return;
	if (type_old == -1)
		insert_hotness_entry(fio->sbi, fio->new_blkaddr, value, type_new);
	else
		update_hotness_entry(fio->sbi, fio->old_blkaddr, fio->new_blkaddr, value, type_old,
			type_new);
}

static int init_hc_management(struct f2fs_sb_info *sbi)
{
	unsigned int i;

	sbi->hi = f2fs_kzalloc(sbi, sizeof(struct hotness_info), GFP_KERNEL);
	if (!sbi->hi)
		return -ENOMEM;

	sbi->n_clusters = N_CLUSTERS;
	sbi->centers = f2fs_kzalloc(sbi, sizeof(unsigned int) * sbi->n_clusters, GFP_KERNEL);
	if (!sbi->centers) {
		kfree(sbi->hi);
		sbi->hi = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < TEMP_TYPE_NUM; i++) {
		INIT_RADIX_TREE(&sbi->hi->hotness_rt_array[i], GFP_NOFS);
		mutex_init(&sbi->hi->hotness_tree_lock[i]);
		sbi->hi->irr_min[i] = __INT32_MAX__ >> 2;
	}

	sbi->centers_valid = 0;
	atomic_set(&sbi->total_writed_block_count, 0);

	return 0;
}

int f2fs_build_hc_manager(struct f2fs_sb_info *sbi)
{
	return init_hc_management(sbi);
}

void f2fs_destory_hc_manager(struct f2fs_sb_info *sbi)
{
	if (!sbi->hi)
		return;

	release_hotness_entry(sbi);
	kfree(sbi->hi);
	sbi->hi = NULL;

	if (sbi->centers) {
		kfree(sbi->centers);
		sbi->centers = NULL;
	}
}

static int kmeans_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct f2fs_hc_kthread *hc_th = sbi->hc_thread;
	wait_queue_head_t *wq = &sbi->hc_thread->hc_wait_queue_head;
	int err;
	unsigned int total_blocks;
	unsigned int last_total_blocks;
	unsigned int wait_ms;

	wait_ms = hc_th->min_sleep_time;

	set_freezable();
	do {
		if (!sbi->hi) {
			sbi->centers_valid = 0;
			break;
		}
		last_total_blocks = sbi->hi->new_blk_cnt + sbi->hi->upd_blk_cnt;

		wait_event_interruptible_timeout(*wq, kthread_should_stop() || freezing(current),
				msecs_to_jiffies(wait_ms));
		if (try_to_freeze())
			continue;
		if (kthread_should_stop())
			break;
		total_blocks = sbi->hi->new_blk_cnt + sbi->hi->upd_blk_cnt;

		if (total_blocks - last_total_blocks > DEF_HC_THREAD_DELTA_BLOCKS)
			hc_decrease_sleep_time(hc_th, &wait_ms);
		else
			hc_increase_sleep_time(hc_th, &wait_ms);

		err = f2fs_hc(sbi);
		if (!err)
			sbi->centers_valid = 1;
	} while (!kthread_should_stop());
	return 0;
}

int f2fs_start_hc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_hc_kthread *hc_th;
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	int err = 0;

	if (!sbi->hi) {
		f2fs_err(sbi, "hotness info alloc fail, need start hc thread");
		return 0;
	}

	hc_th = f2fs_kmalloc(sbi, sizeof(struct f2fs_hc_kthread), GFP_KERNEL);
	if (!hc_th) {
		err = -ENOMEM;
		goto out;
	}

	hc_th->min_sleep_time = DEF_HC_THREAD_MIN_SLEEP_TIME;
	hc_th->max_sleep_time = DEF_HC_THREAD_MAX_SLEEP_TIME;

	sbi->hc_thread = hc_th;
	init_waitqueue_head(&sbi->hc_thread->hc_wait_queue_head);
	sbi->hc_thread->f2fs_hc_task = kthread_run(kmeans_thread_func, sbi,
			"f2fs_hc-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(hc_th->f2fs_hc_task)) {
		err = PTR_ERR(hc_th->f2fs_hc_task);
		kfree(hc_th);
		sbi->hc_thread = NULL;
	}
out:
	return err;
}

void f2fs_stop_hc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_hc_kthread *hc_th = sbi->hc_thread;

	if (!hc_th)
		return;
	kthread_stop(hc_th->f2fs_hc_task);
	kfree(hc_th);
	sbi->hc_thread = NULL;
}

void release_hotness_entry(struct f2fs_sb_info *sbi)
{
	struct radix_tree_iter iter;
	void __rcu **slot;
	int type;

	if (atomic_read(&sbi->hi->count) == 0)
		return;
	for (type = 0; type < TEMP_TYPE_NUM; type++) {
		radix_tree_for_each_slot(slot, &sbi->hi->hotness_rt_array[type], &iter, 0) {
			radix_tree_delete(&sbi->hi->hotness_rt_array[type], iter.index);
		}
	}
}
#endif
