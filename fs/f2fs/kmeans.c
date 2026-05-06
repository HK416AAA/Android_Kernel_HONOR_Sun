/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Honor Technologies Co., Ltd.
 */
#include <linux/fs.h>

#include <linux/module.h>
#include <linux/f2fs_fs.h>
#include <linux/random.h>
#include <linux/radix-tree.h>
#include <linux/timex.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"

#ifdef CONFIG_HONOR_F2FS_HOTNESS_CLUSTERING
#include "hc.h"
#include "kmeans.h"

static void add_to_nearest_set(unsigned int data, struct mass_center *mass_centers, int center_num);
static int find_initial_cluster(unsigned int *data, int data_num, struct mass_center *mass_centers,
		int center_num, int init_random);
static unsigned long long random(void);
static void bubble_sort(unsigned int *x, int num);

struct timespec64 ts_start, ts_end;
struct timespec64 ts_delta;

int f2fs_hc(struct f2fs_sb_info *sbi)
{
	struct radix_tree_iter iter;
	void __rcu **slot;
	__u64 value;
	__u32 irr;
	int type;
	int center_num = N_CLUSTERS;
	unsigned int *data;
	struct mass_center *mass_centers;
	int data_num;
	int i, flag, loop_count, j;
	bool clustered = false;
	int ret = 0;
	int he_count;

	if (!sbi->hi)
		return 0;

	ktime_get_boottime_ts64(&ts_start);

	he_count = atomic_read(&sbi->hi->count);
	f2fs_info(sbi, "Doing f2fs hc, count = %u.\n", he_count);
	if (he_count == 0 || he_count >= DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD) {
		f2fs_info(sbi, "In function %s, sbi->hi->count is out of valid range.\n", __func__);
		ret = -1;
		clustered = false;
		goto reduce;
	}

retry:
	clustered = true;
	data = vmalloc(sizeof(unsigned int) * he_count);
	if (!data) {
		f2fs_info(sbi, "In %s: data == NULL, count = %u.\n", __func__, he_count);
		ret = -1;
		goto out;
	}

	mass_centers = kmalloc(sizeof(struct mass_center) * center_num, GFP_KERNEL);
	if (!mass_centers) {
		f2fs_info(sbi, "In %s: mass_center is NULL.\n", __func__);
		vfree(data);
		ret = -1;
		goto out;
	}
	data_num = 0;

	for (type = 0; type < TEMP_TYPE_NUM; type++) {
		rcu_read_lock();
		radix_tree_for_each_slot(slot, &sbi->hi->hotness_rt_array[type], &iter, 0) {
			value = (__u64) radix_tree_deref_slot(slot);
			irr = (value & __INT32_MAX__) >> 2;
			if (irr && (irr != (__INT32_MAX__ >> 2))) {
				data[data_num++] = irr;
				if (data_num >= he_count) {
					rcu_read_unlock();
					goto do_cluster;
				}
			}
		}
		rcu_read_unlock();
	}

do_cluster:
	f2fs_info(sbi, "In function %s, data_num = %d.\n", __func__, data_num);
	if (data_num <= DEF_HC_MIN_CLUSTER_THRESHOLD) {
		f2fs_info(sbi, "In %s: valid hotness entry too less to cluster.\n", __func__);
		ret = -1;
		goto free;
	}
	if (find_initial_cluster(data, data_num, mass_centers, center_num, RANDOM_SEED)) {
		f2fs_info(sbi, "In %s: find_initial_cluster error.\n", __func__);
		ret = -1;
		goto free;
	}

	flag = 1;
	loop_count = 0;
	while (flag == 1 && loop_count < MAX_LOOP_NUM) {
		flag = 0;
		++loop_count;

		for (i = 0; i < center_num; ++i) {
			mass_centers[i].total_irr = 0;
			mass_centers[i].entry_num = 0;
		}
		for (j = 0; j < data_num; ++j)
			add_to_nearest_set(data[j], mass_centers, center_num);
		for (i = 0; i < center_num; ++i) {
			if (mass_centers[i].entry_num == 0)
				continue;
			if (mass_centers[i].center !=
				mass_centers[i].total_irr / mass_centers[i].entry_num) {
				flag = 1;
				mass_centers[i].center =
					mass_centers[i].total_irr / mass_centers[i].entry_num;
			}
		}
	}
	for (i = 0; i < center_num; ++i)
		sbi->centers[i] = (unsigned int)mass_centers[i].center;
	bubble_sort(sbi->centers, center_num);

	if (center_num == 3)
		f2fs_info(sbi, "centers: %u, %u, %u\n", sbi->centers[0], sbi->centers[1],
				sbi->centers[2]);
	else
		f2fs_info(sbi, "center num is error!\n");

free:
	vfree(data);
	kfree(mass_centers);
reduce:
	if (he_count >= DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD) {
		reduce_hotness_entry(sbi);
		if (!clustered) {
			he_count = atomic_read(&sbi->hi->count);
			goto retry;
		}
	}
out:
	ktime_get_boottime_ts64(&ts_end);
	ts_delta = timespec64_sub(ts_end, ts_start);
	f2fs_info(sbi, "[f2fs] time consumed: %lld (ns)\n", timespec64_to_ns(&ts_delta));

	return ret;
}

int kmeans_get_type(struct f2fs_io_info *fio, __u32 irr)
{
	int type;

	if (fio->sbi->n_clusters == 3) {
		type = min_hc_3(diff(irr, fio->sbi->centers[0]),
			diff(irr, fio->sbi->centers[1]),
			diff(irr, fio->sbi->centers[2]));
	} else {
		type = min_hc_2(diff(irr, fio->sbi->centers[0]),
			diff(irr, fio->sbi->centers[1]));
	}

	return type;
}

static int find_initial_cluster(unsigned int *data, int data_num, struct mass_center *mass_centers,
		int center_num, int init_random)
{
	int i, j, k;
	unsigned int *distance;
	unsigned long long total_distance;
	unsigned long long threshold;
	unsigned long long distance_sum;

	//random seed
	if (init_random == 1) {
random_seed:
		for (i = 0; i < center_num; ++i)
			mass_centers[i].center = data[(int)(random() % data_num)];
		return 0;
	}
	// kmeans++ seed
	mass_centers[0].center = data[(int)(random() % data_num)];
	distance = vmalloc(sizeof(unsigned int) * data_num);
	if (!distance) {
		printk(KERN_ERR "In %s: distance is NULL, data_num = %d.\n", __func__, data_num);
		return -1;
	}
	for (k = 1; k < center_num; ++k) {
		total_distance = 0;
		/* Find the distance from each element to all current centers */
		for (j = 0; j < data_num; ++j) {
			distance[j] = 0;
			for (i = 0; i < k; i++)
				distance[j] += diff(mass_centers[i].center, data[j]);
			total_distance += distance[j];
		}

		/* That are farther from the current centroid are more likely to be selected */
		if (total_distance == 0) {
			vfree(distance);
			goto random_seed;
		}
		threshold = random() % total_distance;
		distance_sum = 0;
		for (j = 0; j < data_num; ++j) {
			distance_sum += distance[j];
			if (distance_sum >= threshold)
				break;
		}

		mass_centers[k].center = data[j];
	}
	vfree(distance);
	return 0;
}

static unsigned long long random(void)
{
	unsigned long long x;

	get_random_bytes(&x, sizeof(x));
	return x;
}

static void add_to_nearest_set(unsigned int data, struct mass_center *mass_centers, int center_num)
{
	unsigned int min = diff(mass_centers[0].center, data);
	int position = 0, i;

	for (i = 1; i < center_num; i++) {
		unsigned int temp = diff(mass_centers[i].center, data);

		if (temp < min) {
			min = temp;
			position = i;
		}
	}
	mass_centers[position].total_irr += data;
	mass_centers[position].entry_num++;
}

static void bubble_sort(unsigned int *x, int num)
{
	int temp, i, j;

	for (i = 0; i < num - 1; ++i)
		for (j = 0; j < num - 1 - i; ++j)
			if (x[j] > x[j + 1]) {
				temp = x[j + 1];
				x[j + 1] = x[j];
				x[j] = temp;
			}
}
#endif
