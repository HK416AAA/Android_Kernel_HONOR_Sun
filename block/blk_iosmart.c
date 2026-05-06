/*
 * Copyright (c) honor Technologies Co., Ltd. 2020-2020. All rights reserved.
 * Description: add iosmart(weight) policy for blkcg
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/blktrace_api.h>
#include <linux/sched/signal.h>
#include <linux/version.h>
#include "blk-cgroup.h"
#include "blk.h"

#define SHARE_SHIFT 14
#define MAX_SHARE (1 << SHARE_SHIFT)

#define BW_SLICE_MIN (8ULL << 20) /* 8MB */
#define BW_SLICE_MAX (256ULL << 20) /* 256MB */
#define BW_SLICE_DEF (32ULL << 20) /* 32MB */

#define IOPS_SLICE_MIN 32
#define IOPS_SLICE_MAX 4096
#define IOPS_SLICE_DEF 256

#define IOSMART_IDLE_INTERVAL 20 /* ms */

#define IOSMART_MAX_WAKEUP 20

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#define ANDROID_OEM_DATA_IOSMART 1
#endif

/* get_disk is removed in mainline v4.16-rc4 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
#define get_disk_local get_disk
#define put_disk_local put_disk
#else
#define get_disk_local get_disk_and_module
#define put_disk_local put_disk_and_module
#endif

enum run_mode {
	MODE_WEIGHT_NONE = 0,
	MODE_WEIGHT_BANDWIDTH = 2,
	MODE_WEIGHT_IOPS = 3,
	MODE_WEIGHT_MAX = 4,
};

enum blk_iosmart_weight_onoff_mode {
	BLK_IOSMART_WEIGHT_OFF,
	BLK_IOSMART_WEIGHT_ON_FS,
};

struct iosmart_service_queue {
	struct iosmart_service_queue *parent_sq;

	unsigned int children_weight;

	/* disk bandwidth share of the queue */
	unsigned int share;
};

struct iosmart_data {
	struct iosmart_service_queue service_queue;
	struct request_queue *queue;

	struct list_head active;
	struct list_head expired;

	uint64_t bw_slice;
	unsigned int iops_slice;

	enum run_mode mode;
};

struct iosmart_grp {
	/* must be the first member */
	struct blkg_policy_data pd;

	/* this group's service queue */
	struct iosmart_service_queue service_queue;

	/* iosmart_data this group belongs to */
	struct iosmart_data *sd;

	uint64_t bps;
	uint64_t bytes_disp;

	unsigned int iops;
	unsigned int io_disp;

	unsigned long intime;

	wait_queue_head_t wait;

	struct list_head node;
	bool expired;
};

static struct blkcg_policy blkcg_policy_iosmart;

static int blk_iosmart_weight_onoff;

static DEFINE_MUTEX(iosmart_mode_lock);

static char *run_mode_name[MODE_WEIGHT_MAX] = {
	[MODE_WEIGHT_NONE] = "none",
	[MODE_WEIGHT_BANDWIDTH] = "weight_bw",
	[MODE_WEIGHT_IOPS] = "weight_iops",
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
static void request_queue_spin_lock_irq(struct request_queue *q)
{
	spin_lock_irq(q->queue_lock);
}

static void request_queue_spin_unlock_irq(struct request_queue *q)
{
	spin_unlock_irq(q->queue_lock);
}
#else
static void request_queue_spin_lock_irq(struct request_queue *q)
{
	spin_lock_irq(&q->queue_lock);
}

static void request_queue_spin_unlock_irq(struct request_queue *q)
{
	spin_unlock_irq(&q->queue_lock);
}

#endif

static bool sd_weight_based(struct iosmart_data *sd)
{
	return sd->mode >= MODE_WEIGHT_BANDWIDTH;
}

static inline struct iosmart_grp *pd_to_sg(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct iosmart_grp, pd) : NULL;
}

static inline struct iosmart_grp *blkg_to_sg(struct blkcg_gq *blkg)
{
	return pd_to_sg(blkg_to_pd(blkg, &blkcg_policy_iosmart));
}

static inline struct blkcg_gq *sg_to_blkg(struct iosmart_grp *sg)
{
	return pd_to_blkg(&sg->pd);
}

static inline struct iosmart_grp *sq_to_sg(struct iosmart_service_queue *sq)
{
	if (sq && sq->parent_sq)
		return container_of(sq, struct iosmart_grp, service_queue);

	return NULL;
}

static inline struct iosmart_data *sq_to_sd(struct iosmart_service_queue *sq)
{
	struct iosmart_grp *sg = sq_to_sg(sq);

	if (sg)
		return sg->sd;

	return container_of(sq, struct iosmart_data, service_queue);
}

static void sg_update_perf(struct iosmart_grp *sg)
{
	enum run_mode mode = sg->sd->mode;
	struct iosmart_service_queue *sq = &sg->service_queue;

	if (mode == MODE_WEIGHT_BANDWIDTH) {
		sg->bps = max_t(uint64_t,
				(sg->sd->bw_slice * sq->share) >> SHARE_SHIFT,
				1024);
		sg->iops = UINT_MAX;
	} else if (mode == MODE_WEIGHT_IOPS) {
		sg->bps = U64_MAX;
		sg->iops = max_t(
			unsigned int,
			(sg->sd->iops_slice * sq->share) >> SHARE_SHIFT, 1);
	} else {
		sg->bps = U64_MAX;
		sg->iops = UINT_MAX;
	}
}

static void sg_update_share(struct iosmart_data *sd, struct iosmart_grp *sg)
{
	struct cgroup_subsys_state *pos_css;
	struct blkcg_gq *blkg, *parent_blkg;
	struct iosmart_grp *child;
	struct iosmart_service_queue *sq;
	unsigned int weight;
#ifdef ANDROID_OEM_DATA_IOSMART
	struct blkg_oem_data *p_oem_data;
#endif

	/*
	 * cgroup v1 uses the if path, although cgroup v2 is not
	 * supported, reserve else path.
	 */
	if (!sg || !sg->service_queue.parent_sq ||
	    !sg->service_queue.parent_sq->parent_sq)
		parent_blkg = sd->queue->root_blkg;
	else
		parent_blkg = sg_to_blkg(sq_to_sg(sg->service_queue.parent_sq));

	blkg_for_each_descendant_pre(blkg, pos_css, parent_blkg) {
		child = blkg_to_sg(blkg);
		sq = &child->service_queue;

		/* root blkg is not restricted */
		if (blkg == sd->queue->root_blkg)
			continue;

		if (!sq->parent_sq || !blkg->online)
			continue;

#ifdef ANDROID_OEM_DATA_IOSMART
		p_oem_data = (struct blkg_oem_data *)&blkg->android_oem_data1;
		weight = p_oem_data->weight;
#else
		weight = blkg->weight;
#endif
		sq->share = max_t(unsigned int,
				  sq->parent_sq->share * weight /
					  sq->parent_sq->children_weight,
				  1);

		sg_update_perf(child);
	}
}

static void sd_change_mode(struct iosmart_data *sd, int mode)
{
	struct request_queue *q = sd->queue;

	if (q->mq_ops)
		blk_mq_freeze_queue(q);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
	else
		blk_queue_bypass_start(q);
#endif
	request_queue_spin_lock_irq(q);
	blk_iosmart_drain_queue(q);

	sd->mode = mode;

	rcu_read_lock();
	sg_update_share(sd, NULL);
	rcu_read_unlock();

	request_queue_spin_unlock_irq(q);

	if (q->mq_ops)
		blk_mq_unfreeze_queue(q);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
	else
		blk_queue_bypass_end(q);
#endif
}

static int iosmart_print_weight_offon(struct seq_file *sf, void *v)
{
	seq_puts(sf, "Weight Based iosmart: ");

	if (blk_iosmart_weight_onoff == BLK_IOSMART_WEIGHT_ON_FS)
		seq_puts(sf, "on-fs\n");
	else
		seq_puts(sf, "off\n");

	return 0;
}

static ssize_t iosmart_set_weight_offon(struct kernfs_open_file *of, char *buf,
					size_t nbytes, loff_t off)
{
	buf = strstrip(buf);
	if (!strcmp(buf, "on-fs"))
		blk_iosmart_weight_onoff = BLK_IOSMART_WEIGHT_ON_FS;
	else if (!strcmp(buf, "off"))
		blk_iosmart_weight_onoff = BLK_IOSMART_WEIGHT_OFF;
	else
		return -EINVAL;

	return nbytes;
}

static u64 sd_prfill_mode_device(struct seq_file *sf,
				 struct blkg_policy_data *pd, int off)
{
	struct iosmart_grp *sg = pd_to_sg(pd);
	const char *dname = blkg_dev_name(pd_to_blkg(pd));
	enum run_mode mode = sg->sd->mode;

	if (!dname)
		return 0;

	if (mode == MODE_WEIGHT_NONE)
		return 0;

	seq_printf(sf, "%s %s\n", dname, run_mode_name[mode]);
	return 0;
}

static int sd_print_mode_device(struct seq_file *sf, void *v)
{
	int i;

	seq_puts(sf, "available ");
	for (i = MODE_WEIGHT_NONE; i < MODE_WEIGHT_MAX; i++) {
		if (i != MODE_WEIGHT_NONE && i != MODE_WEIGHT_BANDWIDTH &&
		    i != MODE_WEIGHT_IOPS)
			continue;

		seq_printf(sf, "%s(%d) ", run_mode_name[i], i);
	}
	seq_printf(sf, "\ndefault %s\n", run_mode_name[MODE_WEIGHT_NONE]);

	mutex_lock(&iosmart_mode_lock);
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), sd_prfill_mode_device,
			  &blkcg_policy_iosmart, 0, false);
	mutex_unlock(&iosmart_mode_lock);

	return 0;
}

static ssize_t sd_set_mode_device(struct kernfs_open_file *of, char *buf,
				  size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct blkg_conf_ctx ctx;
	struct iosmart_grp *sg;
	struct gendisk *disk;
	int ret, mode;
	bool need_change = false;

	mutex_lock(&iosmart_mode_lock);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	ret = blkg_conf_prep(blkcg, &blkcg_policy_iosmart, buf, &ctx);
	if (ret)
		goto out;
#else
	blkg_conf_init(&ctx, buf);
	ret = blkg_conf_prep(blkcg, &blkcg_policy_iosmart, &ctx);
	if (ret)
		goto out_finish;
#endif

	if (kstrtoint(ctx.body, 10, &mode) < 0) {
		ret = -EINVAL;
		goto out_finish;
	}

	if (mode != MODE_WEIGHT_NONE && mode != MODE_WEIGHT_BANDWIDTH &&
	    mode != MODE_WEIGHT_IOPS) {
		ret = -EINVAL;
		goto out_finish;
	}

	sg = blkg_to_sg(ctx.blkg);
	if (sg->sd->mode == mode)
		goto out_finish;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	disk = ctx.bdev->bd_disk;
#else
	disk = ctx.disk;
#endif
	if (!get_disk_local(disk)) {
		ret = -EBUSY;
		goto out_finish;
	}

	need_change = true;

out_finish:
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	blkg_conf_finish(&ctx);
#else
	blkg_conf_exit(&ctx);
#endif

	if (need_change) {
		sd_change_mode(sg->sd, mode);
		put_disk_local(disk);
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
out:
#endif
	mutex_unlock(&iosmart_mode_lock);
	return ret ?: nbytes;
}

static u64 sd_prfill_bw_slice_device(struct seq_file *sf,
				     struct blkg_policy_data *pd, int off)
{
	struct blkcg_gq *blkg = pd_to_blkg(pd);
	const char *dname = blkg_dev_name(blkg);
	uint64_t bw_slice;

	if (!dname)
		return 0;

	bw_slice = pd_to_sg(pd)->sd->bw_slice;
	if (bw_slice == BW_SLICE_DEF)
		return 0;

	seq_printf(sf, "%s: bw_slice %llu\n", dname, bw_slice);
	return 0;
}

static int sd_print_bw_slice_device(struct seq_file *sf, void *v)
{
	seq_printf(sf, "default: %llu\n", BW_SLICE_DEF);

	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  sd_prfill_bw_slice_device, &blkcg_policy_iosmart, 0,
			  false);

	return 0;
}

static ssize_t sd_set_bw_slice_device(struct kernfs_open_file *of, char *buf,
				      size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct iosmart_grp *sg;
	struct blkg_conf_ctx ctx;
	uint64_t val;
	int ret;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	ret = blkg_conf_prep(blkcg, &blkcg_policy_iosmart, buf, &ctx);
	if (ret)
		return ret;
#else
	blkg_conf_init(&ctx, buf);
	ret = blkg_conf_prep(blkcg, &blkcg_policy_iosmart, &ctx);
	if (ret)
		goto out_finish;
#endif

	if (kstrtou64(ctx.body, 10, &val) < 0) {
		ret = -EINVAL;
		goto out_finish;
	}

	if (val < BW_SLICE_MIN || val > BW_SLICE_MAX) {
		ret = -EINVAL;
		goto out_finish;
	}

	sg = blkg_to_sg(ctx.blkg);
	if (sg->sd->bw_slice == val)
		goto out_finish;

	sg->sd->bw_slice = val;

	sg_update_share(sg->sd, NULL);

out_finish:
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	blkg_conf_finish(&ctx);
#else
	blkg_conf_exit(&ctx);
#endif
	return ret ?: nbytes;
}

static u64 sd_prfill_iops_slice_device(struct seq_file *sf,
				       struct blkg_policy_data *pd, int off)
{
	struct blkcg_gq *blkg = pd_to_blkg(pd);
	const char *dname = blkg_dev_name(blkg);
	uint64_t iops_slice;

	if (!dname)
		return 0;

	iops_slice = pd_to_sg(pd)->sd->iops_slice;
	if (iops_slice == IOPS_SLICE_DEF)
		return 0;

	seq_printf(sf, "%s: iops_slice %llu\n", dname, iops_slice);
	return 0;
}

static int sd_print_iops_slice_device(struct seq_file *sf, void *v)
{
	seq_printf(sf, "default: %d\n", IOPS_SLICE_DEF);

	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  sd_prfill_iops_slice_device, &blkcg_policy_iosmart, 0,
			  false);

	return 0;
}

static ssize_t sd_set_iops_slice_device(struct kernfs_open_file *of, char *buf,
					size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct iosmart_grp *sg;
	struct blkg_conf_ctx ctx;
	unsigned int val;
	int ret;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	ret = blkg_conf_prep(blkcg, &blkcg_policy_iosmart, buf, &ctx);
	if (ret)
		return ret;
#else
	blkg_conf_init(&ctx, buf);
	ret = blkg_conf_prep(blkcg, &blkcg_policy_iosmart, &ctx);
	if (ret)
		goto out_finish;
#endif

	if (kstrtouint(ctx.body, 10, &val) < 0) {
		ret = -EINVAL;
		goto out_finish;
	}

	if (val < IOPS_SLICE_MIN || val > IOPS_SLICE_MAX) {
		ret = -EINVAL;
		goto out_finish;
	}

	sg = blkg_to_sg(ctx.blkg);
	if (sg->sd->iops_slice == val)
		goto out_finish;

	sg->sd->iops_slice = val;

	sg_update_share(sg->sd, NULL);

out_finish:
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	blkg_conf_finish(&ctx);
#else
	blkg_conf_exit(&ctx);
#endif
	return ret ?: nbytes;
}

static int iosmart_print_cgroup_type(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));
#ifdef ANDROID_OEM_DATA_IOSMART
	struct blkcg_oem_data *p_oem_data =
		(struct blkcg_oem_data *)&blkcg->android_oem_data1;
	seq_printf(sf, "type: %d\n", p_oem_data->type);
#else
	seq_printf(sf, "type: %d\n", blkcg->type);
#endif

	return 0;
}

static int iosmart_set_cgroup_type(struct cgroup_subsys_state *css,
				   struct cftype *cft, u64 val)
{
	struct blkcg *blkcg = css_to_blkcg(css);
	unsigned int type = (unsigned int)val;
#ifdef ANDROID_OEM_DATA_IOSMART
	struct blkcg_oem_data *p_oem_data =
		(struct blkcg_oem_data *)&blkcg->android_oem_data1;
#endif

	if (type >= BLK_THROTL_TYPE_NR)
		return -EINVAL;

#ifdef ANDROID_OEM_DATA_IOSMART
	p_oem_data->type = type;
#else
	blkcg->type = type;
#endif
	return 0;
}

static u64 sg_prfill_weight_device(struct seq_file *sf,
				   struct blkg_policy_data *pd, int off)
{
	struct blkcg_gq *blkg = pd_to_blkg(pd);
	const char *dname = blkg_dev_name(blkg);
#ifdef ANDROID_OEM_DATA_IOSMART
	struct blkg_oem_data *p_oem_data1 =
		(struct blkg_oem_data *)&blkg->android_oem_data1;
	struct blkcg_oem_data *p_oem_data2 =
		(struct blkcg_oem_data *)&blkg->blkcg->android_oem_data1;
#endif

	if (!dname)
		return 0;

#ifdef ANDROID_OEM_DATA_IOSMART
	if (p_oem_data1->weight == p_oem_data2->weight)
		return 0;
	seq_printf(sf, "%s: weight %u\n", dname, p_oem_data1->weight);
	return 0;
#else
	if (blkg->weight == blkg->blkcg->weight)
		return 0;
	seq_printf(sf, "%s: weight %u\n", dname, blkg->weight);
	return 0;
#endif
}

static int iosmart_print_weight(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));
#ifdef ANDROID_OEM_DATA_IOSMART
	struct blkcg_oem_data *p_oem_data =
		(struct blkcg_oem_data *)&blkcg->android_oem_data1;
#endif

	/*
	 * blkcg weight default is 500, If do not set blkg weight,
	 * blkg weight will be 500.
	 */
#ifdef ANDROID_OEM_DATA_IOSMART
	seq_printf(sf, "default: %u\n", p_oem_data->weight);
#else
	seq_printf(sf, "default: %u\n", blkcg->weight);
#endif
	blkcg_print_blkgs(sf, blkcg, sg_prfill_weight_device,
			  &blkcg_policy_iosmart, 0, false);

	return 0;
}

static ssize_t sg_set_weight_device(struct kernfs_open_file *of, char *buf,
				    size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct iosmart_grp *sg;
	struct iosmart_service_queue *parent_sq;
	struct blkg_conf_ctx ctx;
	unsigned int val;
	int ret;
#ifdef ANDROID_OEM_DATA_IOSMART
	struct blkg_oem_data *p_oem_data;
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	ret = blkg_conf_prep(blkcg, &blkcg_policy_iosmart, buf, &ctx);
	if (ret)
		return ret;
#else
	blkg_conf_init(&ctx, buf);
	ret = blkg_conf_prep(blkcg, &blkcg_policy_iosmart, &ctx);
	if (ret)
		goto out_finish;
#endif

	if (kstrtouint(ctx.body, 10, &val) < 0) {
		ret = -EINVAL;
		goto out_finish;
	}

	if (val < BLKIO_WEIGHT_MIN || val > BLKIO_WEIGHT_MAX) {
		ret = -EINVAL;
		goto out_finish;
	}

#ifdef ANDROID_OEM_DATA_IOSMART
	p_oem_data = (struct blkg_oem_data *)&ctx.blkg->android_oem_data1;
	if (p_oem_data->weight == val)
#else
	if (ctx.blkg->weight == val)
#endif
		goto out_finish;

	sg = blkg_to_sg(ctx.blkg);
	parent_sq = sg->service_queue.parent_sq;
	if (parent_sq) {
#ifdef ANDROID_OEM_DATA_IOSMART
		parent_sq->children_weight -= p_oem_data->weight;
#else
		parent_sq->children_weight -= ctx.blkg->weight;
#endif
		parent_sq->children_weight += val;
	}
#ifdef ANDROID_OEM_DATA_IOSMART
	p_oem_data->weight = val;
#else
	ctx.blkg->weight = val;
#endif

	sg_update_share(sg->sd, sg);

out_finish:
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	blkg_conf_finish(&ctx);
#else
	blkg_conf_exit(&ctx);
#endif
	return ret ?: nbytes;
}

static int sg_print_weight_device(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  sg_prfill_weight_device, &blkcg_policy_iosmart, 0,
			  false);
	return 0;
}

static struct cftype iosmart_legacy_files[] = {
	{
		.name = "throttle.weight_based_offon",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = iosmart_print_weight_offon,
		.write = iosmart_set_weight_offon,
	},
	{
		.name = "throttle.mode_device",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = sd_print_mode_device,
		.write = sd_set_mode_device,
	},
	{
		.name = "throttle.bw_slice_device",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = sd_print_bw_slice_device,
		.write = sd_set_bw_slice_device,
	},
	{
		.name = "throttle.iops_slice_device",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = sd_print_iops_slice_device,
		.write = sd_set_iops_slice_device,
	},
	{
		.name = "throttle.type",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = iosmart_print_cgroup_type,
		.write_u64 = iosmart_set_cgroup_type,
	},
	{
		.name = "throttle.weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = iosmart_print_weight,
	},
	{
		.name = "throttle.weight_device",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = sg_print_weight_device,
		.write = sg_set_weight_device,
	},
	{} /* terminate */
};

static bool sg_may_dispatch(struct iosmart_grp *sg)
{
	if (sg->sd->mode == MODE_WEIGHT_BANDWIDTH)
		return (sg->bytes_disp < sg->bps);

	if (sg->sd->mode == MODE_WEIGHT_IOPS)
		return (sg->io_disp < sg->iops);

	return true;
}

static bool sd_should_start_new_slice(struct iosmart_data *sd)
{
	struct iosmart_grp *sg;
	unsigned long idle_jiffies;
#ifdef ANDROID_OEM_DATA_IOSMART
	struct blkcg_oem_data *p_oem_data;
#endif

	list_for_each_entry(sg, &sd->expired, node) {
#ifdef ANDROID_OEM_DATA_IOSMART
		p_oem_data = (struct blkcg_oem_data *)&sg_to_blkg(sg)
				     ->blkcg->android_oem_data1;
		if (p_oem_data->type <= BLK_THROTL_FG)
#else
		if (sg_to_blkg(sg)->blkcg->type <= BLK_THROTL_FG)
#endif
			return true;
	}

	idle_jiffies = msecs_to_jiffies(IOSMART_IDLE_INTERVAL);
	list_for_each_entry(sg, &sd->active, node) {
		if (time_before(jiffies, sg->intime + idle_jiffies))
			return false;
	}

	return true;
}

static void sd_start_new_slice(struct iosmart_data *sd)
{
	struct iosmart_grp *sg;

	/*
	 * This is in q->queue_lock, to prevent too long execution time,
	 * let each sg wakeup to IOSMART_MAX_WAKEUP.
	 */
	list_for_each_entry(sg, &sd->expired, node) {
		sg->expired = false;
		if (sg->sd->mode == MODE_WEIGHT_BANDWIDTH) {
			sg->bytes_disp = 0;
			wake_up_nr(&sg->wait, IOSMART_MAX_WAKEUP);
		} else if (sg->sd->mode == MODE_WEIGHT_IOPS) {
			sg->io_disp = 0;

			if (sg->iops > IOSMART_MAX_WAKEUP)
				wake_up_nr(&sg->wait, IOSMART_MAX_WAKEUP);
			else
				wake_up_nr(&sg->wait, sg->iops);
		}
	}

	list_splice_tail_init(&sd->expired, &sd->active);
}

static inline void iosmart_update_quota(struct iosmart_grp *sg,
					unsigned int size, enum run_mode mode)
{
	sg->intime = jiffies;
	if (mode == MODE_WEIGHT_BANDWIDTH)
		sg->bytes_disp += size;
	else
		sg->io_disp++;
}

bool blk_throtl_get_quota(struct block_device *bdev, unsigned int size,
			  unsigned long jiffies_time_out, bool wait)
{
	bool ret = true;
	struct request_queue *q;
	struct iosmart_data *sd;
	struct iosmart_grp *sg = NULL;
	struct blkcg_gq *blkg = NULL;
	struct blkcg *new_blkcg, *ori_blkcg = NULL;
	unsigned long start_jiffies, idle_jiffies;
	unsigned long wait_jiffies;

	if (current->flags & (PF_MEMALLOC | PF_KSWAPD | PF_MUTEX_GC))
		return true;

	/*
	 * bdev has been got at mount(), and will be freed at kill_sb() after
	 * all files closed and all dirty pages flushed. So it's safe to
	 * using bdev without lock here.
	 */
	if (unlikely(!bdev) || unlikely(!bdev->bd_disk))
		return true;

	q = bdev_get_queue(bdev);
	if (unlikely(!q))
		return true;

#ifdef ANDROID_OEM_DATA_IOSMART
	sd = (struct iosmart_data *)q->android_oem_data1;
#else
	sd = q->sd;
#endif

	start_jiffies = jiffies;

get_new_group:
	/*
	 * A iosamrt_grp pointer retrieved under rcu can be used to access
	 * basic fields.
	 */
	rcu_read_lock();

	if (blk_iosmart_weight_onoff != BLK_IOSMART_WEIGHT_ON_FS)
		goto out_unlock_rcu;

	if (!sd_weight_based(sd))
		goto out_unlock_rcu;

	new_blkcg = task_blkcg(current);
	if (new_blkcg == &blkcg_root)
		goto out_unlock_rcu;

	if (ori_blkcg != new_blkcg) {
		ori_blkcg = new_blkcg;

		blkg = blkg_lookup(ori_blkcg, q);
		if (unlikely(!blkg)) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
			request_queue_spin_lock_irq(q);
#endif
			blkg = blkg_lookup_create(ori_blkcg, q->disk);
			if (IS_ERR(blkg))
				blkg = NULL;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
			request_queue_spin_unlock_irq(q);
#endif
		}

		sg = blkg_to_sg(blkg);
	}

	if (unlikely(!sg))
		goto out_unlock_rcu;

	request_queue_spin_lock_irq(q);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
	if (unlikely(blk_queue_bypass(q)))
		goto out_unlock;
#endif
	if (unlikely(blk_queue_dying(q)))
		goto out_unlock;

	if (unlikely(!blkg->online))
		goto out_unlock;

	if (unlikely(!sd_weight_based(sd)))
		goto out_unlock;

retry:
	if (sg_may_dispatch(sg)) {
		iosmart_update_quota(sg, size, sd->mode);
		if (!list_empty(&sg->node))
			goto out_unlock;
		blkg_get(blkg);
		list_add_tail(&sg->node, &sd->active);
		goto out_unlock;
	} else if (!wait) {
		ret = false;
		goto out_unlock;
	}

	if (!sg->expired) {
		sg->expired = true;
		if (unlikely(list_empty(&sg->node))) {
			/*
			 * sg not in sd->active list,
			 * this should never happen.
			 */
			WARN_ON(1);
			blkg_get(blkg);
		}
		list_move_tail(&sg->node, &sd->expired);

		if (list_empty(&sd->active))
			goto start_new_slice;
	}

	/*
	 * If jiffies_time_out expires, just return to continue
	 * to read/write.
	 */
	wait_jiffies = jiffies_time_out - (jiffies - start_jiffies);
	if (jiffies - start_jiffies >= jiffies_time_out)
		goto out_unlock;

	if (sd_should_start_new_slice(sd))
		goto start_new_slice;

	blkg_get(blkg);
	request_queue_spin_unlock_irq(q);
	rcu_read_unlock();

	/* Waiting for starting new slice, max waiting time is 20ms */
	idle_jiffies = msecs_to_jiffies(IOSMART_IDLE_INTERVAL);
	if (wait_jiffies > idle_jiffies)
		wait_jiffies = idle_jiffies;

	wait_event_interruptible_timeout(sg->wait, !sg->expired, wait_jiffies);

	blkg_put(blkg);

	/* If this task is interrupted, just return */
	if (signal_pending_state(TASK_INTERRUPTIBLE, current))
		return true;

	/* this task may switch to other group, get tg again */
	goto get_new_group;

start_new_slice:
	sd_start_new_slice(sd);
	goto retry;

out_unlock:
	request_queue_spin_unlock_irq(q);

out_unlock_rcu:
	rcu_read_unlock();
	return ret;
}
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
static struct blkg_policy_data *iosmart_pd_alloc(gfp_t gfp, int node)
{
	struct iosmart_grp *sg;
	sg = kzalloc_node(sizeof(*sg), gfp, node);

#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
static struct blkg_policy_data *
iosmart_pd_alloc(gfp_t gfp, struct request_queue *q, struct blkcg *blkcg)
{
	struct iosmart_grp *sg;

	sg = kzalloc_node(sizeof(*sg), gfp, q->node);
#else /* 8750: kernel 6.6.9 */
static struct blkg_policy_data *iosmart_pd_alloc(struct gendisk *disk,
						 struct blkcg *blkcg, gfp_t gfp)
{
	struct iosmart_grp *sg;

	sg = kzalloc_node(sizeof(*sg), gfp, disk->node_id);
#endif

	if (!sg)
		return NULL;

	sg->bps = U64_MAX;
	sg->iops = UINT_MAX;

	init_waitqueue_head(&sg->wait);

	INIT_LIST_HEAD(&sg->node);
	sg->expired = false;

	return &sg->pd;
}

static void iosmart_pd_init(struct blkg_policy_data *pd)
{
	struct iosmart_grp *sg = pd_to_sg(pd);
	struct blkcg_gq *blkg = pd_to_blkg(pd);
	struct iosmart_data *sd;
	struct iosmart_service_queue *sq = &sg->service_queue;
#ifdef ANDROID_OEM_DATA_IOSMART
	sd = (struct iosmart_data *)blkg->q->android_oem_data1;
#else
	sd = blkg->q->sd;
#endif
	sq->parent_sq = &sd->service_queue;

	/* although cgroup v2 is not supported, reserve this code */
	if (cgroup_subsys_on_dfl(io_cgrp_subsys) && blkg->parent)
		sq->parent_sq = &blkg_to_sg(blkg->parent)->service_queue;

	sg->sd = sd;
}

static void iosmart_pd_online(struct blkg_policy_data *pd)
{
	struct iosmart_grp *sg = pd_to_sg(pd);
	struct blkcg_gq *blkg = pd_to_blkg(pd);
	struct iosmart_service_queue *parent_sq;
#ifdef ANDROID_OEM_DATA_IOSMART
	struct blkg_oem_data *p_oem_data =
		(struct blkg_oem_data *)&blkg->android_oem_data1;
#endif

	/* root blkg is not restricted */
	if (blkg->blkcg == &blkcg_root)
		return;

	parent_sq = sg->service_queue.parent_sq;
	if (parent_sq)
#ifdef ANDROID_OEM_DATA_IOSMART
		parent_sq->children_weight += p_oem_data->weight;
#else
		parent_sq->children_weight += blkg->weight;
#endif

	sg_update_share(sg->sd, sg);
}

static void iosmart_pd_offline(struct blkg_policy_data *pd)
{
	struct iosmart_grp *sg = pd_to_sg(pd);
	struct blkcg_gq *blkg = pd_to_blkg(pd);
	struct iosmart_service_queue *parent_sq;
#ifdef ANDROID_OEM_DATA_IOSMART
	struct blkg_oem_data *p_oem_data =
		(struct blkg_oem_data *)&blkg->android_oem_data1;
#endif

	if (!blkg->online)
		return;

	sg->bps = U64_MAX;
	sg->iops = UINT_MAX;

	if (!list_empty(&sg->node)) {
		/*
		 * There is no need to wake up sg->wait, If access this,
		 * caller must be blkcg_css_offline. task's blkcg will be
		 * changed, ios in sg->wait will belong to new blkcg.
		 */
		list_del_init(&sg->node);
		blkg_put(sg_to_blkg(sg));
	}

	/* root blkg is not restricted */
	if (blkg->blkcg == &blkcg_root)
		return;

	parent_sq = sg->service_queue.parent_sq;
	if (parent_sq)
#ifdef ANDROID_OEM_DATA_IOSMART
		parent_sq->children_weight -= p_oem_data->weight;
#else
		parent_sq->children_weight -= blkg->weight;
#endif

	rcu_read_lock();
	sg_update_share(sg->sd, sg);
	rcu_read_unlock();
}

static void iosmart_pd_free(struct blkg_policy_data *pd)
{
	struct iosmart_grp *sg = pd_to_sg(pd);

	kfree(sg);
}

static struct blkcg_policy blkcg_policy_iosmart = {
	.legacy_cftypes = iosmart_legacy_files,

	.pd_alloc_fn = iosmart_pd_alloc,
	.pd_init_fn = iosmart_pd_init,
	.pd_online_fn = iosmart_pd_online,
	.pd_offline_fn = iosmart_pd_offline,
	.pd_free_fn = iosmart_pd_free,
};

static void sg_drain_node(struct iosmart_grp *sg)
{
	WARN_ON(list_empty(&sg->node));

	list_del_init(&sg->node);
	sg->bytes_disp = 0;
	sg->io_disp = 0;

	/*
	 * Set these to max to ensure that all waiting io will be
	 * processed. If caller is sd_change_mode, will update these
	 * to the corresponding values in sg_update_share.
	 */
	sg->bps = U64_MAX;
	sg->iops = UINT_MAX;

	sg->expired = false;
	wake_up_all(&sg->wait);

	blkg_put(sg_to_blkg(sg));
}

void blk_iosmart_drain_queue(struct request_queue *q)
{
	struct iosmart_data *sd;
	struct iosmart_grp *curr, *next;

#ifdef ANDROID_OEM_DATA_IOSMART
	sd = (struct iosmart_data *)q->android_oem_data1;
#else
	sd = q->sd;
#endif

	list_for_each_entry_safe(curr, next, &sd->active, node)
		sg_drain_node(curr);

	list_for_each_entry_safe(curr, next, &sd->expired, node)
		sg_drain_node(curr);
}
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
int blk_iosmart_init_queue(struct request_queue *q)
{
	int ret;
	struct iosmart_data *sd;

	sd = kzalloc_node(sizeof(*sd), GFP_KERNEL, q->node);
#else
int blk_iosmart_init(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	int ret;
	struct iosmart_data *sd;

	sd = kzalloc_node(sizeof(*sd), GFP_KERNEL, disk->node_id);
#endif
	if (!sd)
		return -ENOMEM;

#ifdef ANDROID_OEM_DATA_IOSMART
	q->android_oem_data1 = (u64)sd;
#else
	q->sd = sd;
#endif
	sd->service_queue.share = MAX_SHARE;
	sd->service_queue.parent_sq = NULL;
	sd->queue = q;

	INIT_LIST_HEAD(&sd->active);
	INIT_LIST_HEAD(&sd->expired);

	sd->bw_slice = BW_SLICE_DEF;
	sd->iops_slice = IOPS_SLICE_DEF;
	sd->mode = MODE_WEIGHT_NONE;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	ret = blkcg_activate_policy(q, &blkcg_policy_iosmart);
#else
	ret = blkcg_activate_policy(disk, &blkcg_policy_iosmart);
#endif
	if (ret) {
		kfree(sd);
#ifdef ANDROID_OEM_DATA_IOSMART
		q->android_oem_data1 = 0;
#else
		q->sd = NULL;
#endif
	}

	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
void blk_iosmart_exit_queue(struct request_queue *q)
{
	WARN_ON(!q->sd);
	blkcg_deactivate_policy(q, &blkcg_policy_iosmart);
	kfree(q->sd);
	q->sd = NULL;
}
#else
void blk_iosmart_exit(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;

	WARN_ON(!q->android_oem_data1);
	blkcg_deactivate_policy(disk, &blkcg_policy_iosmart);
	kfree((void *)q->android_oem_data1);
	q->android_oem_data1 = 0;
}
#endif

static int __init iosmart_init(void)
{
	blk_iosmart_weight_onoff = BLK_IOSMART_WEIGHT_OFF;
	return blkcg_policy_register(&blkcg_policy_iosmart);
}

module_init(iosmart_init);
