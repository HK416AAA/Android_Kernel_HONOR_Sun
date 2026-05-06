#ifndef __LINUX_BLOCK_HONOR_H
#define __LINUX_BLOCK_HONOR_H

#include <linux/blk_types.h>

#define __REQ_SW_VIP __REQ_NR_BITS
#define __REQ_HW_VIP (__REQ_NR_BITS + 1)
#define __REQ_PINNED_BIT (__REQ_NR_BITS + 2)

#define REQ_SW_VIP \
	(__force blk_opf_t)(1ULL << __REQ_SW_VIP) /* vip for io scheduler */
#define REQ_HW_VIP           \
	(__force blk_opf_t)( \
		1ULL << __REQ_HW_VIP) /* vip for io scheduler and device */
#define REQ_PINNED_SLC (__force blk_opf_t)(1ULL << __REQ_PINNED_BIT)

#endif
