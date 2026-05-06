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
#include <ufs/hn_ufs.h>

#ifdef CONFIG_HONOR_SCSI_UFS_FFU

#ifdef CONFIG_HONOR_UFS_DSM
#undef CREATE_TRACE_POINTS
#include <trace/hooks/ogki_honor.h>
#endif

/* UFSHCD error handling flags */
enum {
	UFSHCD_EH_IN_PROGRESS = (1 << 0),
};

#define hn_ufshcd_set_eh_in_progress(h) ((h)->eh_flags |= UFSHCD_EH_IN_PROGRESS)
#define hn_ufshcd_eh_in_progress(h) ((h)->eh_flags & UFSHCD_EH_IN_PROGRESS)
#define hn_ufshcd_clear_eh_in_progress(h) \
	((h)->eh_flags &= ~UFSHCD_EH_IN_PROGRESS)

static void wakeup_source_drop(struct wakeup_source *ws)
{
	if (!ws)
		return;
	__pm_relax(ws);
}

static void wakeup_source_prepare(struct wakeup_source *ws, const char *name)
{
	if (ws) {
		memset(ws, 0, sizeof(*ws));
		ws->name = name;
	}
}

void wakeup_source_init(struct wakeup_source *ws, const char *name)
{
	wakeup_source_prepare(ws, name);
	wakeup_source_add(ws);
}

void wakeup_source_trash(struct wakeup_source *ws)
{
	wakeup_source_remove(ws);
	wakeup_source_drop(ws);
}

static int wait_for_ufs_all_complete(struct ufs_hba *hba, int timeout_ms)
{
	if (hba == NULL)
		return -EINVAL;
	while (timeout_ms-- > 0) {
		if (!hba->outstanding_reqs)
			return 0;
		/* wait 1 ms */
		udelay(1000);
	}
	dev_err(hba->dev, "%s: outstanding req : 0x%lx\n", __func__,
		hba->outstanding_reqs);
	return -ETIMEDOUT;
}

/*lint -restore*/
/**
 * * ufs_ffu_pm_runtime_delay_enable - when ffu_pm_work in workqueue is scheduled, after 30 allow pm_runtime
 * * and unlock the wake lock
 * * @work: pointer to work structure
 * *
 * */
void ufs_ffu_pm_runtime_delay_enable(struct work_struct *work)
{
	struct ufs_hba *hba;
	int err;
	unsigned long flags;

	/*lint -e826*/
	hba = container_of(work, struct ufs_hba, ffu_pm_work);
	if (hba == NULL) {
		pr_err("%s hba get error\n", __func__);
		return;
	}

	/*lint -e826*/
	/* follow scsi command timeout value 2s */
	msleep(2000);
	mutex_lock(&hba->eh_mutex);
	spin_lock_irqsave(hba->host->host_lock, flags);
	hba->ufshcd_state = UFSHCD_STATE_RESET;
	hn_ufshcd_set_eh_in_progress(hba);
	spin_unlock_irqrestore(hba->host->host_lock, flags);
	hba->ffu_get_device_flag = true;
	err = hn_ufshcd_reset_and_restore(hba);
	spin_lock_irqsave(hba->host->host_lock, flags);
	if (!err) {
		hba->ufshcd_state = UFSHCD_STATE_OPERATIONAL;
	} else {
		hba->ufshcd_state = UFSHCD_STATE_ERROR;
	}

	hn_ufshcd_clear_eh_in_progress(hba);
	spin_unlock_irqrestore(hba->host->host_lock, flags);
	pm_runtime_set_active(hba->dev);
	mutex_unlock(&hba->eh_mutex);
	/* wait 27s, this msleep does not effect ffu reset time */
	msleep(27000);

	if (hba->ffu_lock.active) {
		/*lint -save -e455*/
		__pm_relax(&hba->ffu_lock);
		/*lint -restore*/
		dev_err(hba->dev, "ffu unlock wake lock.\n");
	}

	pm_runtime_allow(hba->dev);

#ifdef CONFIG_HONOR_UFS_DSM
	if (err) {
		ufs_dsm_report(hba, DSM_UFS_FFU_UPDATE_ERR,
			       "UFS FFU Update Error");
	}
#endif
}

/**
 * * ufs_ffu_pm_runtime_delay_process - ffu-write buffer request issue, forbid pm_runtime
 * * and lock the wake lock, forbid system suspend.ffu work in queue is scheduled
 * * @hba: pointer to adapter instance
 * *
 * */
/*lint -save -e456 -e454*/
void ufs_ffu_pm_runtime_delay_process(struct ufs_hba *hba)
{
	pm_runtime_forbid(hba->dev);
	if (!hba->ffu_lock.active) {
		__pm_stay_awake(&hba->ffu_lock);
		dev_err(hba->dev, "ffu lock wake lock.\n");
	}
	schedule_work(&hba->ffu_pm_work);
}

void ufshcd_start_delay_work(struct ufs_hba *hba, struct scsi_cmnd *cmd)
{
	/* UFS supports only the MODE field value 0Eh: Download microcode with offsets, save, and defer active */
	if (unlikely((cmd != NULL) && (cmd->cmnd != NULL) &&
		     (cmd->cmnd[0] == WRITE_BUFFER) &&
		     ((cmd->cmnd[1] & WB_MODE_MASK) == DOWNLOAD_MODE))) {
		ufs_ffu_pm_runtime_delay_process(hba);
	}
	return;
}
EXPORT_SYMBOL_GPL(ufshcd_start_delay_work);

static int ufshcd_read_geometry_desc(struct ufs_hba *hba, u8 *buf, u32 size)
{
	return ufshcd_read_desc_param(hba, QUERY_DESC_IDN_GEOMETRY, 0, 0, buf,
				      size);
}

static sector_t ufs_get_geometry_info(struct ufs_hba *hba)
{
	int err;
	uint8_t desc_buf[QUERY_DESC_GEOMETRY_MAX_SIZE];
	u64 total_raw_device_capacity = 0;

	err = ufshcd_read_geometry_desc(hba, desc_buf,
					QUERY_DESC_GEOMETRY_MAX_SIZE);
	if (err) {
		dev_err(hba->dev, "%s: Failed getting geometry info\n",
			__func__);
		goto out;
	}
	total_raw_device_capacity =
		(u64)desc_buf[GEOMETRY_DESC_PARAM_DEV_CAP + 0] << 56 |
		(u64)desc_buf[GEOMETRY_DESC_PARAM_DEV_CAP + 1] << 48 |
		(u64)desc_buf[GEOMETRY_DESC_PARAM_DEV_CAP + 2] << 40 |
		(u64)desc_buf[GEOMETRY_DESC_PARAM_DEV_CAP + 3] << 32 |
		(u64)desc_buf[GEOMETRY_DESC_PARAM_DEV_CAP + 4] << 24 |
		(u64)desc_buf[GEOMETRY_DESC_PARAM_DEV_CAP + 5] << 16 |
		(u64)desc_buf[GEOMETRY_DESC_PARAM_DEV_CAP + 6] << 8 |
		desc_buf[GEOMETRY_DESC_PARAM_DEV_CAP + 7] << 0;

out:
	return total_raw_device_capacity;
}

int ufshcd_ffu_get_device_data(struct ufs_hba *hba)
{
	int ret = 0;

	if (!hba) {
		printk(KERN_ERR
		       "%s: Failed getting device info. null pointer.\n",
		       __func__);
		return EINVAL;
	}
	if (hba->ffu_get_device_flag) {
		ret = hn_ufs_get_device_desc(hba);
		if (ret) {
			dev_info(hba->dev,
				 "%s: Failed getting device info. err = %d\n",
				 __func__, ret);
			return ret;
		}
		hn_ufs_fixup_device_setup(hba);
		ufs_get_geometry_info(hba);
		hba->ffu_get_device_flag = false;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(ufshcd_ffu_get_device_data);

int ufshcd_state_of_ffu_check(struct ufs_hba *hba, struct scsi_cmnd *cmd)
{
	int err = 0;
	unsigned long flags;
	if (cmd->cmnd[0] == WRITE_BUFFER)
		pr_err("MYLOG: %s:line%d-%s(),0x%x,0x%x\n", __FILE__, __LINE__,
		       __func__, cmd->cmnd[0], cmd->cmnd[1]);

	if (!hba || !cmd)
		goto out;

	spin_lock_irqsave(hba->host->host_lock, flags);
	if (hba->ufshcd_state == UFSHCD_STATE_FFU) {
		if ((cmd != NULL) &&
		    ((cmd->cmnd[0] != WRITE_BUFFER) ||
		     ((cmd->cmnd[1] & WB_MODE_MASK) != DOWNLOAD_MODE))) {
			err = SCSI_MLQUEUE_HOST_BUSY;
			spin_unlock_irqrestore(hba->host->host_lock, flags);
			goto out;
		}
	}
	if (unlikely((cmd != NULL) && (cmd->cmnd[0] == WRITE_BUFFER) &&
		     ((cmd->cmnd[1] & WB_MODE_MASK) == DOWNLOAD_MODE))) {
		hba->ufshcd_state = UFSHCD_STATE_FFU;
		/* wait for ufs all complete timeout time 1s */
		err = wait_for_ufs_all_complete(hba, 1000);
	}
	spin_unlock_irqrestore(hba->host->host_lock, flags);

out:
	return err;
}
#endif

#ifdef CONFIG_FACTORY_HONOR_PIN_WB_SET
static void hn_ufshcd_config_pin_wb_register_default_value(
	struct ufs_hba *hba, struct ufswb_dev_info *wb_dev_info)
{
	unsigned int pinned_buf_alloc_units =
		PRESET_PINNED_BUG_ALLOC_UNITS *
		wb_dev_info->multiplier_coefficient;
	unsigned int min_non_pinned_buf_alloc_units =
		PRESET_MIN_NON_PINNED_BUF_ALLOC_UNITS;
	u32 partial_flush_mode_disable = 0;
	int ret;

	/*
	* Note: The order of setting attributes cannot be changed because WDC requires the
	* "Pinned Partial Flush Mode" attribute to be 0 when setting the size of the pin area.
	*/
	if (wb_dev_info->partial_flush_mode_read_from_dev) {
		ret = ufshcd_query_attr_retry(
			hba, UPIU_QUERY_OPCODE_WRITE_ATTR,
			wb_dev_info->idn_partial_flush_mode, 0, 0,
			&partial_flush_mode_disable);
		if (ret) {
			pr_err("ufspinwb: %s:%d, set attr [0x%x] fail. [%d]",
			       __func__, __LINE__,
			       wb_dev_info->idn_partial_flush_mode, ret);
			return;
		}
	}

	ret = ufshcd_query_attr_retry(hba, UPIU_QUERY_OPCODE_WRITE_ATTR,
				      wb_dev_info->idn_pinned_buf_alloc_units,
				      0, 0, &pinned_buf_alloc_units);
	if (ret) {
		pr_err("ufspinwb: %s:%d, set attr [0x%x] fail. [%d]", __func__,
		       __LINE__, wb_dev_info->idn_pinned_buf_alloc_units, ret);
		return;
	}
	pr_info("ufspinwb: %s:%d, set pinned_buf_alloc_units [0x%x] to default value: 0x%x",
		__func__, __LINE__, wb_dev_info->idn_pinned_buf_alloc_units,
		pinned_buf_alloc_units);

	/* Note: For WDC, the 46h register does not follow JEDEC and its type is non-writable. */
	if (hba->dev_info.wmanufacturerid != UFS_VENDOR_WDC) {
		ret = ufshcd_query_attr_retry(
			hba, UPIU_QUERY_OPCODE_WRITE_ATTR,
			wb_dev_info->idn_min_non_pinned_buf_alloc_units, 0, 0,
			&min_non_pinned_buf_alloc_units);
		if (ret) {
			pr_err("ufspinwb: %s:%d, set attr [0x%x] fail. [%d]",
			       __func__, __LINE__,
			       wb_dev_info->idn_min_non_pinned_buf_alloc_units,
			       ret);
			return;
		}
		pr_info("ufspinwb: %s:%d, set min_non_pinned_buf_alloc_units [0x%x] to default value: 0x%x",
			__func__, __LINE__,
			wb_dev_info->idn_min_non_pinned_buf_alloc_units,
			min_non_pinned_buf_alloc_units);
	}

	ret = ufshcd_query_attr_retry(hba, UPIU_QUERY_OPCODE_WRITE_ATTR,
				      wb_dev_info->idn_partial_flush_mode, 0, 0,
				      &wb_dev_info->partial_flush_mode);
	if (ret) {
		pr_err("ufspinwb: %s:%d, set attr [0x%x] fail. [%d]", __func__,
		       __LINE__, wb_dev_info->idn_partial_flush_mode, ret);
		return;
	}
	pr_info("ufspinwb: %s:%d, set partial_flush_mode [0x%x] to default value: 0x%x",
		__func__, __LINE__, wb_dev_info->idn_partial_flush_mode,
		wb_dev_info->partial_flush_mode);

	return;
}

static inline int
hn_ufshcd_get_dev_size_calculate_coefficient(struct ufs_hba *hba,
					     struct ufswb_dev_info *wb_dev_info)
{
	sector_t dev_size = 0;
	unsigned int ufs_size;
	unsigned int ufs_size_gb;
	int multiplier_coefficient = -1;
	int i = 0;

	/* qTotalRawDeviceCapacity unit: 512 bytes, multiply by 512 and divide by 1024*1024 to convert to GB */
	dev_size = ufs_get_geometry_info(hba);
	if (dev_size <= 0)
		return -1;
	ufs_size_gb = dev_size / HONOR_GET_INFO_1G;
	/* the dev_size is usually not exactly an integer multiple of 256G so it needs to be aligned */
	for (i = HONOR_GET_INFO_UFS_MIN; i <= HONOR_GET_INFO_UFS_MAX; i++) {
		ufs_size = 1 << i;
		if (ufs_size_gb < ufs_size) {
			pr_info("ufspinwb: %s:%d, ufs_size:%d G\n", __func__,
				__LINE__, ufs_size);
			break;
		}
	}

	multiplier_coefficient =
		(int)(ufs_size /
		      PRESET_PINNED_BUG_ALLOC_UNITS_DEVICE_SIZE_UNIT);
	pr_info("ufspinwb: %s:%d, dev_size: %llu, multiplier_coefficient: %d",
		__func__, __LINE__, dev_size, multiplier_coefficient);

	return multiplier_coefficient;
}

static int hn_ufshcd_check_pin_wb_registers_already_setted(
	struct ufs_hba *hba, struct ufswb_dev_info *wb_dev_info)
{
	unsigned int partial_flush_mode_read_from_dev = -1;
	unsigned int pinned_buf_alloc_units_read_from_dev = -1;
	unsigned int min_non_pinned_buf_alloc_units_read_from_dev = -1;
	int ret = 0;

	/*
	* the subsequent process will only check and preset register values for devices with 256G and above
	* "1" means that it is the one multiple of the 256G size
	*/
	wb_dev_info->multiplier_coefficient =
		hn_ufshcd_get_dev_size_calculate_coefficient(hba, wb_dev_info);
	if (wb_dev_info->multiplier_coefficient < 1)
		return -1;

	ret = ufshcd_query_attr_retry(hba, UPIU_QUERY_OPCODE_READ_ATTR,
				      wb_dev_info->idn_partial_flush_mode, 0, 0,
				      &partial_flush_mode_read_from_dev);
	if (ret) {
		pr_err("ufspinwb: %s:%d, read attr [0x%x] fail. [%d]", __func__,
		       __LINE__, wb_dev_info->idn_partial_flush_mode, ret);
		return ret;
	}
	pr_info("ufspinwb: %s:%d, default partial_flush_mode_read_from_dev [0x%x]: 0x%x",
		__func__, __LINE__, wb_dev_info->idn_partial_flush_mode,
		partial_flush_mode_read_from_dev);
	wb_dev_info->partial_flush_mode_read_from_dev =
		partial_flush_mode_read_from_dev;

	ret = ufshcd_query_attr_retry(hba, UPIU_QUERY_OPCODE_READ_ATTR,
				      wb_dev_info->idn_pinned_buf_alloc_units,
				      0, 0,
				      &pinned_buf_alloc_units_read_from_dev);
	if (ret) {
		pr_err("ufspinwb: %s:%d, read attr [0x%x] fail. [%d]", __func__,
		       __LINE__, wb_dev_info->idn_pinned_buf_alloc_units, ret);
		return ret;
	}
	pr_info("ufspinwb: %s:%d, default pinned_buf_alloc_units_read_from_dev [0x%x]: 0x%x",
		__func__, __LINE__, wb_dev_info->idn_pinned_buf_alloc_units,
		pinned_buf_alloc_units_read_from_dev);

	ret = ufshcd_query_attr_retry(
		hba, UPIU_QUERY_OPCODE_READ_ATTR,
		wb_dev_info->idn_min_non_pinned_buf_alloc_units, 0, 0,
		&min_non_pinned_buf_alloc_units_read_from_dev);
	if (ret) {
		pr_err("ufspinwb: %s:%d, read attr [0x%x] fail. [%d]", __func__,
		       __LINE__,
		       wb_dev_info->idn_min_non_pinned_buf_alloc_units, ret);
		return ret;
	}
	pr_info("ufspinwb: %s:%d, default min_non_pinned_buf_alloc_units_read_from_dev [0x%x]: 0x%x",
		__func__, __LINE__,
		wb_dev_info->idn_min_non_pinned_buf_alloc_units,
		min_non_pinned_buf_alloc_units_read_from_dev);

	/* Note: For WDC, the 46h register does not follow JEDEC and its type is non-writable. */
	if (hba->dev_info.wmanufacturerid != UFS_VENDOR_WDC) {
		if ((partial_flush_mode_read_from_dev ==
		     wb_dev_info->partial_flush_mode) &&
		    (pinned_buf_alloc_units_read_from_dev ==
		     PRESET_PINNED_BUG_ALLOC_UNITS *
			     wb_dev_info->multiplier_coefficient) &&
		    (min_non_pinned_buf_alloc_units_read_from_dev ==
		     PRESET_MIN_NON_PINNED_BUF_ALLOC_UNITS)) {
			pr_info("ufspinwb: %s:%d. Tip: the values of the registers[3Fh, 45h, 46h] are the same as the preset values, \
			i.e., they have already been set as preset values",
				__func__, __LINE__);
			return -1;
		}
	} else {
		if ((partial_flush_mode_read_from_dev ==
		     wb_dev_info->partial_flush_mode) &&
		    (pinned_buf_alloc_units_read_from_dev ==
		     PRESET_PINNED_BUG_ALLOC_UNITS *
			     wb_dev_info->multiplier_coefficient)) {
			pr_info("ufspinwb: %s:%d. Tip: the values of the registers[3Fh, 45h] are the same as the preset values, \
			i.e., they have already been set as preset values",
				__func__, __LINE__);
			return -1;
		}
	}

	pr_info("ufspinwb: %s:%d, the value(s) of the register(s) related to PINWB are(is) different from the preset value(s)",
		__func__, __LINE__);
	return ret;
}

static inline int ufswb_version_check(struct ufswb_dev_info *wb_dev_info)
{
	pr_info("ufspinwb: %s:%d, Support WB Spec : Driver = %.4X, Device = %.4X",
		__func__, __LINE__, UFS_PIN_WB_VER_SAMSUNG,
		wb_dev_info->wb_ver);

	if (wb_dev_info->wb_ver != UFS_PIN_WB_VER_SAMSUNG)
		return -ENODEV;

	return 0;
}

static inline int hn_ufshcd_read_desc(struct ufs_hba *hba, u8 desc_id,
				      u8 desc_index, u8 *desc_buf, int size)
{
	int ret;

	ret = ufshcd_query_descriptor_retry(hba, UPIU_QUERY_OPCODE_READ_DESC,
					    desc_id, desc_index, 0, desc_buf,
					    &size);

	if (ret)
		pr_err("ufspinwb: %s:%d, reading Device Desc failed. err = %d",
		       __func__, __LINE__, ret);

	return ret;
}

static int hn_ufshcd_check_pin_wb_support(struct ufs_hba *hba,
					  struct ufswb_dev_info *wb_dev_info)
{
	u8 desc_buf[QUERY_DESC_MAX_SIZE] = {0};
	int ret = 0;

	if (hba->dev_info.wspecversion < 0x400) {
		pr_info("ufspinwb: %s:%d. Tip: UFS version [%x] does not support PINWB",
			__func__, __LINE__, hba->dev_info.wspecversion);
		return -EPERM;
	}

	if (hba->dev_info.wmanufacturerid == UFS_VENDOR_SAMSUNG)
		ret = hn_ufshcd_read_desc(
			hba, UFSF_QUERY_DESC_IDN_VENDOR_DEVICE_SAMSUNG, 0,
			desc_buf, QUERY_DESC_MAX_SIZE);
	else
		ret = hn_ufshcd_read_desc(hba, QUERY_DESC_IDN_DEVICE, 0,
					  desc_buf, QUERY_DESC_MAX_SIZE);
	if (ret)
		return ret;

	wb_dev_info->wb_buf_type = desc_buf[DEVICE_DESC_PARAM_WB_BUF_TYPE];
	wb_dev_info->wb_shared_buf_alloc_units = get_unaligned_be32(
		desc_buf + DEVICE_DESC_PARAM_WB_SHARED_BUF_ALLOC_UNITS);
	pr_info("ufspinwb: %s:%d, wb_dev [0x%.2X] bWriteBoosterBufferType [%u], wb_dev [0x%.2X] dNumSharedWriteBoosterBufferAllocUnits [%u]",
		__func__, __LINE__, DEVICE_DESC_PARAM_WB_BUF_TYPE,
		wb_dev_info->wb_buf_type,
		DEVICE_DESC_PARAM_WB_SHARED_BUF_ALLOC_UNITS,
		wb_dev_info->wb_shared_buf_alloc_units);

	if (wb_dev_info->wb_buf_type == WB_BUF_TYPE_SHARED &&
	    wb_dev_info->wb_shared_buf_alloc_units == 0) {
		pr_err("ufspinwb: %s:%d, WB use shared buffer. But alloc unit is (0)",
		       __func__, __LINE__);
		return -EPERM;
	}

	switch (hba->dev_info.wmanufacturerid) {
	case UFS_VENDOR_MICRON:
	case UFS_VENDOR_SKHYNIX:
	case UFS_VENDOR_WDC:
		if (!(get_unaligned_be16(desc_buf +
					 DEVICE_DESC_PARAM_EX_FEAT_SUP_JEDEC) &
		      UFS_FEATURE_SUPPORT_PIN_WB_BIT))
			goto err_out;
		break;

	case UFS_VENDOR_SAMSUNG:
		wb_dev_info->wb_ver = get_unaligned_be16(
			desc_buf + DEVICE_DESC_PARAM_WB_VER_SAMSUNG);
		if (ufswb_version_check(wb_dev_info))
			goto err_out;
		wb_dev_info->idn_partial_flush_mode =
			QUERY_ATTR_IDN_WB_BUF_PARTIAL_FLUSH_MODE_SAMSUNG;
		wb_dev_info->idn_pinned_buf_alloc_units =
			QUERY_ATTR_IDN_WB_PINNED_BUF_ALLOC_UNITS_SAMSUNG;
		wb_dev_info->idn_min_non_pinned_buf_alloc_units =
			QUERY_ATTR_IDN_WB_MIN_NON_PINNED_BUF_ALLOC_UNITS_SAMSUNG;
		wb_dev_info->partial_flush_mode = PRESET_PINNED_MODE_SAMSUNG;
		pr_info("ufspinwb: %s:%d, device[%d]: Pinned Partial Flush Mode support",
			__func__, __LINE__, hba->dev_info.wmanufacturerid);
		return 0;

	case UFS_VENDOR_TOSHIBA:
		if (get_unaligned_be32(
			    desc_buf +
			    DEVICE_DESC_PARAM_EX_FEAT_SUP_KIOXIA_GEN12) &
		    UFS_FEATURE_SUPPORT_PIN_WB_BIT_KIOXIA_GEN12) {
			wb_dev_info->idn_pinned_buf_alloc_units =
				QUERY_ATTR_IDN_WB_PINNED_BUF_ALLOC_UNITS_KIOXIA_GEN12;
			wb_dev_info->idn_min_non_pinned_buf_alloc_units =
				QUERY_ATTR_IDN_WB_MIN_NON_PINNED_BUF_ALLOC_UNITS_KIOXIA_GEN12;
			wb_dev_info->idn_partial_flush_mode =
				QUERY_ATTR_IDN_WB_BUF_PARTIAL_FLUSH_MODE_JEDEC;
			wb_dev_info->partial_flush_mode =
				PRESET_PINNED_MODE_JEDEC;
			pr_info("ufspinwb: %s:%d, device[%d] GEN 12 bUFSExFeaturesSupport[0x%x]: Pinned Partial Flush Mode support",
				__func__, __LINE__,
				hba->dev_info.wmanufacturerid,
				DEVICE_DESC_PARAM_EX_FEAT_SUP_KIOXIA_GEN12);
			return 0;
		} else if (get_unaligned_be16(
				   desc_buf +
				   DEVICE_DESC_PARAM_EX_FEAT_SUP_JEDEC) &
			   UFS_FEATURE_SUPPORT_PIN_WB_BIT) {
			break;
		} else {
			goto err_out;
		}
		break;

	default:
		pr_info("ufspinwb: %s:%d. Tip: the current device[%d] does not support PIN",
			__func__, __LINE__, hba->dev_info.wmanufacturerid);
		return -EPERM;
	}

	wb_dev_info->idn_partial_flush_mode =
		QUERY_ATTR_IDN_WB_BUF_PARTIAL_FLUSH_MODE_JEDEC;
	wb_dev_info->idn_pinned_buf_alloc_units =
		QUERY_ATTR_IDN_WB_PINNED_BUF_ALLOC_UNITS_JEDEC;
	wb_dev_info->idn_min_non_pinned_buf_alloc_units =
		QUERY_ATTR_IDN_WB_MIN_NON_PINNED_BUF_ALLOC_UNITS_JEDEC;
	wb_dev_info->partial_flush_mode = PRESET_PINNED_MODE_JEDEC;
	if (hba->dev_info.wmanufacturerid != UFS_VENDOR_WDC) {
		pr_info("ufspinwb: %s:%d, device[%d] bUFSExFeaturesSupport[0x%x]: Pinned Partial Flush Mode support",
			__func__, __LINE__, hba->dev_info.wmanufacturerid,
			DEVICE_DESC_PARAM_EX_FEAT_SUP_JEDEC);
	} else {
		pr_info("ufspinwb: %s:%d, device[%d]: Pinned Partial Flush Mode support",
			__func__, __LINE__, hba->dev_info.wmanufacturerid);
	}

	return ret;
err_out:
	pr_info("ufspinwb: %s:%d. Tip: the current firmware version of device[%d] does not support PIN",
		__func__, __LINE__, hba->dev_info.wmanufacturerid);
	return -EPERM;
}

void hn_ufshcd_config_pin_wb(struct ufs_hba *hba)
{
	int ret = -1;
	struct ufswb_dev_info wb_dev_info;

	ret = hn_ufshcd_check_pin_wb_support(hba, &wb_dev_info);
	if (ret)
		return;

	ret = hn_ufshcd_check_pin_wb_registers_already_setted(hba,
							      &wb_dev_info);
	if (ret)
		return;

	hn_ufshcd_config_pin_wb_register_default_value(hba, &wb_dev_info);
}
#endif

MODULE_LICENSE("GPL v2");
