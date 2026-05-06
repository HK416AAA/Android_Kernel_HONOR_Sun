/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Honor Technologies Co., Ltd. 2021-2021. All rights reserved.
 * Description: I/O performence measure
 * Author:  lipeng
 * Create:  2021-06-10
 */

#ifndef _IO_MEASURE_H
#define _IO_MEASURE_H
#include <linux/version.h>

enum iom_pgcache_type {
	IOM_CACHE_ACCESS,
	IOM_CACHE_MISS,
	IOM_CACHE_ADD,
	IOM_CACHE_RA_ADD,
	IOM_CACHE_RA_HIT,
	IOM_CACHE_TYPE_NR
};
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0))
#define IOMT_CMD_TAG_STORE_LEN 64

struct iomeasure_timestamp {
	unsigned char type;
	unsigned long ticks;
	ktime_t ktime;
};

struct iom_scsi_time_store {
	struct iomeasure_timestamp iom_start_time[IOMT_CMD_TAG_STORE_LEN];
};
#endif

void io_measure_pgcache_stat_inc(enum iom_pgcache_type type);
void io_measure_blk_sched_log(void);
void io_measure_scsi_sched_log(void);

#endif
