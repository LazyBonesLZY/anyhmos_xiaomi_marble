/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2023-2024 Huawei Device Co., Ltd.
 */

#ifndef _CODE_SIGN_IOCTL_H
#define _CODE_SIGN_IOCTL_H

#include <linux/code_sign.h>
#include <linux/ioctl.h>
#include <linux/rbtree.h>
#include <linux/types.h>

struct cert_chain_info {
	__u32 signing_length;
	__u32 issuer_length;
	__u64 signing_ptr;
	__u64 issuer_ptr;
	__u32 path_len;
	__s32 cert_type;
	__u64 app_id_ptr;
	__u32 app_id_length;
	__u8 __reserved[20];
};

struct activate_cert_info {
	__u64 data[2];
};

struct cert_source {
	char *subject;
	char *issuer;
	unsigned int max_path_depth;
	int path_type;
	unsigned int cnt;
	struct rb_node node;
};

#define ADD_CERT_CHAIN		_IOW('k', 1, struct cert_chain_info)
#define REMOVE_CERT_CHAIN	_IOW('k', 2, struct cert_chain_info)
#define ADD_ENTERPRISE_CERT	_IOW('k', 4, struct cert_chain_info)
#define REMOVE_ENTERPRISE_CERT	_IOW('k', 5, struct cert_chain_info)
#define ACTIVATE_CERT		_IOW('k', 16, struct activate_cert_info)

#define CERT_CHAIN_PATH_LEN_MAX 3

struct cert_source *find_match(const char *subject, const char *issuer, bool is_dev);
int code_sign_open(struct inode *inode, struct file *filp);
int code_sign_release(struct inode *inode, struct file *filp);
long code_sign_ioctl(struct file *filp, unsigned int cmd, unsigned long args);

#endif /* _CODE_SIGN_IOCTL_H */
