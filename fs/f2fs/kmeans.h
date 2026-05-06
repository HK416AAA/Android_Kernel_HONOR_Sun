/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Honor Technologies Co., Ltd.
 */
#ifndef _LINUX_KMEANS_H
#define _LINUX_KMEANS_H

#ifdef CONFIG_HONOR_F2FS_HOTNESS_CLUSTERING
#define diff(a, b) (((a) < (b)) ? ((b) - (a)) : ((a) - (b)))
#define min_hc_3(a, b, c) (((a) < (b)) ? (((a) < (c)) ? \
	CURSEG_HOT_DATA : CURSEG_COLD_DATA) : (((c) > (b)) ? CURSEG_WARM_DATA : CURSEG_COLD_DATA))
#define min_hc_2(a, b) (((a) < (b)) ? CURSEG_HOT_DATA : CURSEG_WARM_DATA)
#define MAX_LOOP_NUM 1000
#define RANDOM_SEED 0  /* kmeans++ seed or random seed */

struct mass_center {
	unsigned long long center;
	unsigned long long total_irr;
	unsigned long long entry_num;
};
int f2fs_hc(struct f2fs_sb_info *sbi);
int kmeans_get_type(struct f2fs_io_info *fio, __u32 irr);

#endif
#endif