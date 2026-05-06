/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef HN_UFS__H
#define HN_UFS__H

#include <linux/bitfield.h>
#include <linux/blk-crypto-profile.h>
#include <linux/blk-mq.h>
#include <linux/devfreq.h>
#include <linux/msi.h>
#include <linux/pm_runtime.h>
#include <linux/dma-direction.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <ufs/unipro.h>
#include <ufs/ufs.h>
#include <ufs/ufs_quirks.h>
#include <ufs/ufshci.h>

#include <linux/async.h>
#include <linux/devfreq.h>
#include <linux/nls.h>
#include <linux/of.h>
#include <linux/bitfield.h>
#include <linux/blk-pm.h>
#include <linux/blkdev.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/sched/clock.h>
#include <linux/iopoll.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_eh.h>
#include "ufshcd-priv.h"
#include <ufs/ufs_quirks.h>
#include <ufs/unipro.h>
#include "ufs-sysfs.h"
#include "ufs-debugfs.h"
#include "ufs-fault-injection.h"
#include "ufs_bsg.h"
#include "ufshcd-crypto.h"
#include <asm/unaligned.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/version.h>

#undef CREATE_TRACE_POINTS
#include <trace/hooks/ufshcd.h>

#ifdef CONFIG_HONOR_SCSI_UFS_FFU
#define WB_MODE_MASK (0x1F)
#define DOWNLOAD_MODE (0xE)
#define QUERY_DESC_GEOMETRY_MAX_SIZE 0x44

void ufshcd_start_delay_work(struct ufs_hba *hba, struct scsi_cmnd *cmd);
int ufshcd_ffu_get_device_data(struct ufs_hba *hba);
void wakeup_source_init(struct wakeup_source *ws, const char *name);
void wakeup_source_trash(struct wakeup_source *ws);
void ufs_ffu_pm_runtime_delay_enable(struct work_struct *work);
int ufshcd_state_of_ffu_check(struct ufs_hba *hba, struct scsi_cmnd *cmd);
#endif

#ifdef CONFIG_FACTORY_HONOR_PIN_WB_SET
/* Descriptor idn for Query Request */
#define UFSF_QUERY_DESC_IDN_VENDOR_DEVICE_SAMSUNG 0xF0

/* Device descriptor parameters offsets in bytes*/
#define DEVICE_DESC_PARAM_EX_FEAT_SUP_KIOXIA_GEN12 \
	0x4F // kioxia GEN12, 4Fh bit[31]
#define DEVICE_DESC_PARAM_EX_FEAT_SUP_JEDEC \
	0x4D // micron, kioxia GEN13, wdc: bit[0]WB resize, bit[1]FIFO, bit[2]PIN
#define DEVICE_DESC_PARAM_WB_VER_SAMSUNG \
	0xF9 // Samsung needs to check the vendor proprietary registers. DESCRIPTOR IDN=F0h, INDEX=00h, and SELECTOR=00h, Offset F9h=0214h, support pin wb
#define DEVICE_DESC_PARAM_WB_BUF_TYPE \
	0x54 // 00h: LU dedicated buffer type, 01h: Single shared buffer type
#define DEVICE_DESC_PARAM_WB_SHARED_BUF_ALLOC_UNITS 0x55

/* Attribute idn for Query requests */
#define QUERY_ATTR_IDN_WB_BUF_PARTIAL_FLUSH_MODE_JEDEC \
	0x3F // hynix, kioxia GEN12 GEN13, micron, wdc
#define QUERY_ATTR_IDN_WB_PINNED_BUF_ALLOC_UNITS_JEDEC \
	0x45 // hynix, kioxia GEN13, micron, wdc
#define QUERY_ATTR_IDN_WB_MIN_NON_PINNED_BUF_ALLOC_UNITS_JEDEC \
	0x46 // hynix, kioxia GEN13, micron, wdc
/* for samsung only */
#define QUERY_ATTR_IDN_WB_BUF_PARTIAL_FLUSH_MODE_SAMSUNG 0x93
#define QUERY_ATTR_IDN_WB_PINNED_BUF_ALLOC_UNITS_SAMSUNG 0x9A
#define QUERY_ATTR_IDN_WB_MIN_NON_PINNED_BUF_ALLOC_UNITS_SAMSUNG 0x9B
/* for kioxia GEN12 only */
#define QUERY_ATTR_IDN_WB_PINNED_BUF_ALLOC_UNITS_KIOXIA_GEN12 0x80
#define QUERY_ATTR_IDN_WB_MIN_NON_PINNED_BUF_ALLOC_UNITS_KIOXIA_GEN12 0x81

/* Samsung needs to check the vendor proprietary registers. DESCRIPTOR IDN=F0h, INDEX=00h, and SELECTOR=00h, Offset F9h=0214h, support pin wb*/
#define UFS_PIN_WB_VER_SAMSUNG 0x0214
/*
* micron, kioxia GEN13, and wdc need to check registers specified by JEDEC. DESCRIPTOR IDN=00h, INDEX=00h, and SELECTOR=00h, Offset 4Dh, bit[0]resize, bit[1]FIFO, bit[2]PIN
* Hynix device descriptor 4Dh does not follow the JEDEC. Offset 4Dh 2bytes = 0000
*/
#define UFS_FEATURE_SUPPORT_PIN_WB_BIT BIT(2)
/* Kioxia GEN12 need to check registers specified by JEDEC. DESCRIPTOR IDN=00h, INDEX=00h, and SELECTOR=00h, Offset 4Fh, bit[31]PIN*/
#define UFS_FEATURE_SUPPORT_PIN_WB_BIT_KIOXIA_GEN12 BIT(31)

/* Preset register values related to pin wb */
#define PRESET_PINNED_MODE_JEDEC 0x2 //JEDEC
#define PRESET_PINNED_MODE_SAMSUNG 0x1 //For samsung only
#define PRESET_PINNED_BUG_ALLOC_UNITS \
	0x800 // Set the pin-area to 8G in units of device size 256G (i.e., 512G: 16G; 1T: 32G)
#define PRESET_PINNED_BUG_ALLOC_UNITS_DEVICE_SIZE_UNIT 256 // 256G
#define PRESET_MIN_NON_PINNED_BUF_ALLOC_UNITS 0x200 // 2G

#define HONOR_GET_INFO_1G (2 * 1024 * 1024)
#define HONOR_GET_INFO_UFS_MIN 2
#define HONOR_GET_INFO_UFS_MAX 12

enum {
	WB_BUF_TYPE_LU = 0,
	WB_BUF_TYPE_SHARED,
};

struct ufswb_dev_info {
	/* from Device Descriptor */
	u16 wb_ver;
	u32 wb_shared_buf_alloc_units;
	u8 wb_buf_type;

	/* set idn according to UFS device vendor */
	u8 idn_partial_flush_mode;
	u8 idn_pinned_buf_alloc_units;
	u8 idn_min_non_pinned_buf_alloc_units;
	u32 partial_flush_mode;

	/* from Device Attribute */
	u32 partial_flush_mode_read_from_dev;

	/* multiples of 256GB */
	u8 multiplier_coefficient;
};

void hn_ufshcd_config_pin_wb(struct ufs_hba *hba);
#endif

#endif /* End of Header */
