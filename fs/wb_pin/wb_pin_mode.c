#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/proc_fs.h>
#include <linux/blk_types.h>
#include <linux/list.h>
#include <linux/dcache.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/pagemap.h>
#include <linux/printk.h>
#include <linux/block_honor.h>
#include <../f2fs/f2fs.h>
#include <ufs/pinwb_check.h>
#include <trace/events/f2fs.h>
#include <trace/events/scsi.h>
#include <trace/hooks/ogki_honor.h>

#define MAX_NAME_LEN 512
#define MAX_SUFFIXL_EN 32
#define MIN_WB_PIN_BUF 100 //unit MB

#define wb_pin_err(x...) pr_err(x)
#define wb_pin_dbg(x...) pr_info(x)

#define F2FS_FILE_WBPIN_FL 0x800

#define PINNED_GROUP_NUMBER 0x18
#define PIN_LIST_MAX 32

enum pin_type_e {
	FILE_NAME, //eg. "0 /data/data/com.tencent.mm/MicroMsg/EnMicroMsg.db"
	FILE_SUFFIX, //eg. "1 test.base"
	PIN_TYPE_COUNT
};

/*
	config file:
	1)name
	2)suffix：path or file name suffix
*/
struct wb_pin_file_info {
	char *f_name_list[PIN_LIST_MAX];
	ulong f_list_count;
	ulong f_pinned_map;
};
struct wb_pin_suffix_info {
	char name[MAX_SUFFIXL_EN];
};

struct wb_pin_mode {
	bool enabled;
	uint32_t suf_support;
	uint32_t log_level;
	struct wb_pin_file_info f_info;
	struct wb_pin_suffix_info suffix;
};

struct wb_pin_mode g_wb_pin;

module_param_named(log_level, g_wb_pin.log_level, uint, S_IRUSR | S_IWUSR);
module_param_named(enabled, g_wb_pin.enabled, bool, S_IRUSR | S_IWUSR);

static int parse_user_pinned_file_data(char *input)
{
	char *token;
	int type = -1;
	int name_len;
	int ret;
	uint32_t file_index;
	struct wb_pin_mode *wb_pin = &g_wb_pin;

	if (!input)
		return -EINVAL;

	if (wb_pin->enabled)
		return -EINVAL;

	if (wb_pin->f_info.f_list_count >= PIN_LIST_MAX)
		return -EINVAL;

	token = strsep(&input, " ");
	if (!token)
		return -EINVAL;

	ret = kstrtouint(token, 0, &type);
	if (ret || type >= PIN_TYPE_COUNT) {
		wb_pin_err("%s, line %d, %d\n", __func__, __LINE__, type);
		return -EINVAL;
	}

	name_len = strlen(input);
	if ((name_len > MAX_NAME_LEN) || (name_len == 0)) {
		wb_pin_err("%s, line %d, name len %d\n", __func__, __LINE__,
			   name_len);
		return -EINVAL;
	}
	if (type == FILE_NAME) {
		file_index = wb_pin->f_info.f_list_count;
		wb_pin->f_info.f_name_list[file_index] =
			kstrdup(input, GFP_KERNEL);
		if (wb_pin->f_info.f_name_list[file_index] == NULL) {
			wb_pin_err("%s, line %d, kstrdup name fail\n", __func__,
				   __LINE__);
			return -ENOMEM;
		}

		wb_pin_dbg("%s pin file list[%d] add, %s\n", __func__, type,
			   input);
		wb_pin->f_info.f_list_count++;

	} else if ((type == FILE_SUFFIX) && (name_len > 0) &&
		   (name_len < MAX_SUFFIXL_EN)) {
		strcpy(wb_pin->suffix.name, input);
		wb_pin->suffix.name[name_len] = '\0';
		wb_pin->suf_support = 1;
	}

	return 0;
}

static ssize_t wb_pin_mode_proc_write(struct file *file,
				      const char __user *buffer, size_t count,
				      loff_t *ppos)
{
	char pin_file[MAX_NAME_LEN] = {'\0'};
	int ret;

	if ((count > MAX_NAME_LEN - 1) || (count <= 0)) {
		wb_pin_err("%s, line %d, count err %zu\n", __func__, __LINE__,
			   count);
		return count;
	}

	if (copy_from_user(pin_file, buffer, count)) {
		wb_pin_err("%s, line %d, copy from user err\n", __func__,
			   __LINE__);
		return count;
	}

	ret = parse_user_pinned_file_data(pin_file);
	if (ret)
		wb_pin_err("%s fail, ret %d\n", __func__, ret);

	return count;
}

static int wb_pin_mode_proc_show(struct seq_file *file, void *param)
{
	struct wb_pin_mode *wb_pin = &g_wb_pin;
	int i;

	if (!file)
		return 0;

	for (i = 0; i < wb_pin->f_info.f_list_count; i++) {
		seq_printf(file, "wb_pin list[%d]: %s\n", i,
			   wb_pin->f_info.f_name_list[i]);
	}

	if (wb_pin->f_info.f_list_count > 0)
		seq_printf(file, "wb_pin set pinned file map 0x%lx\n",
			   wb_pin->f_info.f_pinned_map);

	if (wb_pin->suf_support)
		seq_printf(file, "wb_pin suffix name:%s\n",
			   wb_pin->suffix.name);

	return 0;
}

static int wb_pin_mode_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, wb_pin_mode_proc_show, NULL);
}

static const struct proc_ops pin_mode_proc_fops = {
	.proc_write = wb_pin_mode_proc_write,
	.proc_lseek = seq_lseek,
	.proc_read = seq_read,
	.proc_open = wb_pin_mode_proc_open,
};

static inline void set_ufscmd_pinned_buffer(struct scsi_cmnd *cmd)
{
	cmd->cmnd[6] |= PINNED_GROUP_NUMBER;
}

static inline void set_bio_pin_mode(struct bio *bio)
{
	bio->bi_opf |= REQ_PINNED_SLC;
}
static inline bool test_request_pin_mode(struct request *rq)
{
	return (rq->cmd_flags & REQ_PINNED_SLC);
}
static inline void set_inode_pin_mode(struct inode *inode)
{
	F2FS_I(inode)->i_flags |= F2FS_FILE_WBPIN_FL;
}
static inline bool test_inode_pin_mode(struct inode *inode)
{
	return (F2FS_I(inode)->i_flags & F2FS_FILE_WBPIN_FL);
}

static void wb_pin_scsi_dispatch_cmd_start_hook(void *ignore,
						struct scsi_cmnd *cmd)
{
	struct request *rq = scsi_cmd_to_rq(cmd);

	if (!g_wb_pin.enabled)
		return;

	if ((cmd->cmnd[0] != WRITE_16) && (cmd->cmnd[0] != WRITE_10))
		return;

	if (test_request_pin_mode(rq))
		set_ufscmd_pinned_buffer(cmd);
}

static void wb_pin_f2fs_submit_page_write_hook(void *ignore, struct page *page,
					       struct bio *bio)
{
	struct inode *inode;

	if (page == NULL || bio == NULL)
		return;

	inode = page_file_mapping(page)->host;
	if (inode == NULL)
		return;

	if (test_inode_pin_mode(inode))
		set_bio_pin_mode(bio);
}

static bool wb_pin_inode_check(struct inode *inode, struct wb_pin_mode *wb_pin)
{
	uint32_t list_count = wb_pin->f_info.f_list_count;

	if (!wb_pin->enabled)
		return false;

	if (inode == NULL || S_ISDIR(inode->i_mode))
		return false;

	if (test_inode_pin_mode(inode))
		return false;

	if (!wb_pin->suf_support) {
		if (S_ISDIR(inode->i_mode))
			return false;

		if (!list_count)
			return false;

		if (list_count <=
		    bitmap_weight(&wb_pin->f_info.f_pinned_map, PIN_LIST_MAX))
			return false;
	}

	return true;
}

static void wb_pin_android_vh_f2fs_create(void *ignore, struct inode *inode,
					  struct dentry *dentry)
{
	struct wb_pin_mode *wb_pin = &g_wb_pin;
	const char *name = NULL;
	char *tmp;
	uint32_t list_count = wb_pin->f_info.f_list_count;
	int i;

	if (!inode || !dentry)
		return;

	name = (const char *)dentry->d_name.name;
	if (!name)
		return;

	if (!wb_pin_inode_check(inode, wb_pin))
		return;

	if (wb_pin->suf_support) {
		tmp = strstr(name, wb_pin->suffix.name);
		if (tmp != NULL) {
			set_inode_pin_mode(inode);
			wb_pin_dbg("%s set ino %lu pin mode\n", __func__,
				   inode->i_ino);
			return;
		}
	}

	for_each_clear_bit(i, &wb_pin->f_info.f_pinned_map, list_count)
		if (!strcmp(kbasename(wb_pin->f_info.f_name_list[i]), name)) {
			set_inode_pin_mode(inode);
			bitmap_set(&wb_pin->f_info.f_pinned_map, i, 1);
			wb_pin_dbg("%s set ino %lu pin mode\n", __func__,
				   inode->i_ino);
			return;
		}

	return;
}

bool wb_pin_should_enable(void)
{
	unsigned int size_mb;

	//check ufs device is support wb pinned mode
	if (!ufswb_check_support_pinned_condition_init()) {
		wb_pin_err("%s ufswb not support", __func__);
		return false;
	}
	size_mb = ufswb_get_available_curr_pinned_buffer_size();
	if (size_mb < MIN_WB_PIN_BUF) {
		wb_pin_err("%s ufswb available buf %u MB", __func__, size_mb);
		return false;
	}

	return true;
}

static int __init wb_pin_mode_init(void)
{
	int ret;
	g_wb_pin.log_level = 1;
	g_wb_pin.f_info.f_list_count = 0;
	g_wb_pin.f_info.f_pinned_map = 0;
	g_wb_pin.suf_support = 0;

	if (!wb_pin_should_enable())
		return 0;

	/* sys init config true */
	g_wb_pin.enabled = false;
	memset(g_wb_pin.suffix.name, 0, MAX_SUFFIXL_EN);

	proc_create("wb_pin_file_list", 0660, NULL, &pin_mode_proc_fops);

	ret = register_trace_android_vh_ogki_f2fs_create(
		wb_pin_android_vh_f2fs_create, NULL);
	WARN_ON(ret);

	ret = register_trace_android_vh_ogki_f2fs_submit_write_page(
		wb_pin_f2fs_submit_page_write_hook, NULL);
	WARN_ON(ret);

	ret = register_trace_scsi_dispatch_cmd_start(
		wb_pin_scsi_dispatch_cmd_start_hook, NULL);
	WARN_ON(ret);

	return 0;
}

static void __exit wb_pin_mode_exit(void)
{
	unregister_trace_android_vh_ogki_f2fs_create(
		wb_pin_android_vh_f2fs_create, NULL);
	unregister_trace_android_vh_ogki_f2fs_submit_write_page(
		wb_pin_f2fs_submit_page_write_hook, NULL);
	unregister_trace_scsi_dispatch_cmd_start(
		wb_pin_scsi_dispatch_cmd_start_hook, NULL);
}

module_init(wb_pin_mode_init);
module_exit(wb_pin_mode_exit);

MODULE_LICENSE("GPL");
