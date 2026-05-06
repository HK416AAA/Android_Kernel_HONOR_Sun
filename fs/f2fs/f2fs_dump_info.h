// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/f2fs_dump_info.h
 *
 * Copyright (c) 2024 Honor Technologies Co., Ltd.
 */

#ifndef F2FS_DUMP_INFO_H
#define F2FS_DUMP_INFO_H

void f2fs_print_raw_sb_info(struct f2fs_sb_info *sbi);
void f2fs_print_ckpt_info(struct f2fs_sb_info *sbi);
/* ckpt and sb info is far from the panic point,
   add the Real-time disk info, sbi messages */
void f2fs_print_sbi_info(struct f2fs_sb_info *sbi);
void set_f2fs_fill_super_done(int value);
#endif
