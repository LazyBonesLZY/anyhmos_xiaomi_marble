// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2024 Huawei Device Co., Ltd.
 * Ported for Xiaomi marble Linux 5.10 kernel with DAC/CAP caller enforcement.
 */

#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>

#include "code_sign_ioctl.h"
#include "code_sign_log.h"

#define MAX_SIGNING_LENGTH 2048
#define OHOS_INSTALLS_UID  3060U

static DEFINE_SPINLOCK(cert_chain_tree_lock);
static struct rb_root cert_chain_tree = RB_ROOT;
static struct rb_root dev_cert_chain_tree = RB_ROOT;
static bool cert_path_activated;

static void free_cert_source(struct cert_source *source)
{
	if (!source)
		return;
	kfree(source->subject);
	kfree(source->issuer);
	kfree(source);
}

static bool code_sign_caller_allowed(void)
{
	uid_t euid = from_kuid(&init_user_ns, current_euid());

	if (euid == 0 || euid == OHOS_INSTALLS_UID || capable(CAP_SYS_ADMIN))
		return true;
	return false;
}

static bool code_sign_is_root_caller(void)
{
	return uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

static struct cert_source *matched_cert_search(struct rb_root *root,
					       const char *subject,
					       const char *issuer)
{
	struct rb_node **cur_node = &(root->rb_node);

	while (*cur_node) {
		struct cert_source *cur_cert = container_of(*cur_node, struct cert_source, node);
		int result = strcmp(subject, cur_cert->subject);

		if (result < 0) {
			cur_node = &((*cur_node)->rb_left);
		} else if (result > 0) {
			cur_node = &((*cur_node)->rb_right);
		} else {
			result = strcmp(issuer, cur_cert->issuer);
			if (result < 0) {
				cur_node = &((*cur_node)->rb_left);
			} else if (result > 0) {
				cur_node = &((*cur_node)->rb_right);
			} else {
				code_sign_log_debug("cert found: subject='%s'", subject);
				return cur_cert;
			}
		}
	}
	return NULL;
}

static struct cert_source *cert_chain_search(struct rb_root *root,
					     const char *subject,
					     const char *issuer,
					     bool has_locked)
{
	struct cert_source *matched_cert;

	if (has_locked)
		return matched_cert_search(root, subject, issuer);

	spin_lock(&cert_chain_tree_lock);
	matched_cert = matched_cert_search(root, subject, issuer);
	spin_unlock(&cert_chain_tree_lock);
	return matched_cert;
}

struct cert_source *find_match(const char *subject, const char *issuer, bool is_dev)
{
	if (is_dev)
		return cert_chain_search(&dev_cert_chain_tree, subject, issuer, false);
	return cert_chain_search(&cert_chain_tree, subject, issuer, false);
}

static int cert_chain_insert(struct rb_root *root, struct cert_source *cert)
{
	struct rb_node **new;
	struct rb_node *parent = NULL;
	bool duplicate = false;

	if (!code_sign_is_root_caller()) {
		if (!(cert->path_type == RELEASE_DEVELOPER_CODE ||
		      cert->path_type == DEBUG_DEVELOPER_CODE)) {
			code_sign_log_error("no permission to insert cert type 0x%x",
					    cert->path_type);
			free_cert_source(cert);
			return -EPERM;
		}
	}

	spin_lock(&cert_chain_tree_lock);
	new = &(root->rb_node);

	while (*new) {
		struct cert_source *this = container_of(*new, struct cert_source, node);
		int result = strcmp(cert->subject, this->subject);

		parent = *new;
		if (result < 0) {
			new = &((*new)->rb_left);
		} else if (result > 0) {
			new = &((*new)->rb_right);
		} else {
			result = strcmp(cert->issuer, this->issuer);
			if (result < 0) {
				new = &((*new)->rb_left);
			} else if (result > 0) {
				new = &((*new)->rb_right);
			} else {
				this->cnt++;
				duplicate = true;
				code_sign_log_info("cert already exists in trust sources (cnt=%u)",
						   this->cnt);
				goto out;
			}
		}
	}

	cert->cnt = 1;
	rb_link_node(&cert->node, parent, new);
	rb_insert_color(&cert->node, root);

	code_sign_log_info("add trusted cert: subject='%s', issuer='%s', depth=%u, type=0x%x",
			   cert->subject, cert->issuer, cert->max_path_depth, cert->path_type);
out:
	spin_unlock(&cert_chain_tree_lock);
	if (duplicate)
		free_cert_source(cert);
	return 0;
}

static int cert_chain_remove(struct rb_root *root, struct cert_source *cert)
{
	struct cert_source *matched_cert;
	struct cert_source *to_free = NULL;
	int ret = 0;

	spin_lock(&cert_chain_tree_lock);
	matched_cert = cert_chain_search(root, cert->subject, cert->issuer, true);
	if (!matched_cert) {
		ret = -EINVAL;
		goto out;
	}

	if (matched_cert->path_type == RELEASE_DEVELOPER_CODE ||
	    matched_cert->path_type == DEBUG_DEVELOPER_CODE) {
		--matched_cert->cnt;
		if (matched_cert->cnt > 0)
			goto out;
		rb_erase(&matched_cert->node, root);
		to_free = matched_cert;
		code_sign_log_info("remove trusted cert: subject='%s', issuer='%s'",
				   cert->subject, cert->issuer);
		goto out;
	}

	code_sign_log_error("cannot remove cert type 0x%x", matched_cert->path_type);
	ret = -EKEYREJECTED;
out:
	spin_unlock(&cert_chain_tree_lock);
	free_cert_source(to_free);
	free_cert_source(cert);
	return ret;
}

int code_sign_open(struct inode *inode, struct file *filp)
{
	return 0;
}

int code_sign_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static int parse_cert_source(unsigned long args, struct cert_source **_source)
{
	struct cert_chain_info info;
	struct cert_source *source;
	int ret;

	BUILD_BUG_ON(sizeof(struct cert_chain_info) != 64);

	source = kzalloc(sizeof(*source), GFP_KERNEL);
	if (!source)
		return -ENOMEM;

	if (copy_from_user(&info, (const void __user *)args, sizeof(info))) {
		code_sign_log_error("cmd copy_from_user failed");
		ret = -EFAULT;
		goto fail;
	}

	if (info.path_len == 0 || info.path_len > CERT_CHAIN_PATH_LEN_MAX ||
	    info.issuer_length == 0 || info.signing_length == 0 ||
	    info.issuer_length > MAX_SIGNING_LENGTH ||
	    info.signing_length > MAX_SIGNING_LENGTH) {
		code_sign_log_error("invalid cert path len=%u signing_len=%u issuer_len=%u",
				    info.path_len, info.signing_length, info.issuer_length);
		ret = -EINVAL;
		goto fail;
	}

	source->subject = kzalloc(info.signing_length + 1, GFP_KERNEL);
	if (!source->subject) {
		ret = -ENOMEM;
		goto fail;
	}
	if (copy_from_user(source->subject, u64_to_user_ptr(info.signing_ptr),
			   info.signing_length)) {
		code_sign_log_error("copy_from_user signing failed");
		ret = -EFAULT;
		goto fail;
	}

	source->issuer = kzalloc(info.issuer_length + 1, GFP_KERNEL);
	if (!source->issuer) {
		ret = -ENOMEM;
		goto fail;
	}
	if (copy_from_user(source->issuer, u64_to_user_ptr(info.issuer_ptr),
			   info.issuer_length)) {
		code_sign_log_error("copy_from_user issuer failed");
		ret = -EFAULT;
		goto fail;
	}

	source->max_path_depth = info.path_len;
	source->path_type = info.cert_type;
	*_source = source;
	return 0;

fail:
	free_cert_source(source);
	return ret;
}

static int code_sign_check_code(int code)
{
	if ((code > RELEASE_CODE_START && code < RELEASE_CODE_END) ||
	    code == RELEASE_RESTRICTED_CODE)
		return 0;

	if ((code > DEBUG_CODE_START && code < DEBUG_CODE_END) ||
	    code == DEBUG_RESTRICTED_CODE)
		return 1;

	code_sign_log_error("cert type 0x%x is invalid", code);
	return -EINVAL;
}

long code_sign_ioctl(struct file *filp, unsigned int cmd, unsigned long args)
{
	struct cert_source *source = NULL;
	int ret = 0;

	if (!code_sign_caller_allowed()) {
		code_sign_log_error("caller euid=%u lacks code_sign permission",
				    from_kuid(&init_user_ns, current_euid()));
		return -EPERM;
	}

	switch (cmd) {
	case ADD_CERT_CHAIN:
		ret = parse_cert_source(args, &source);
		if (ret)
			return ret;

		ret = code_sign_check_code(source->path_type);
		if (ret < 0) {
			free_cert_source(source);
			return ret;
		}
		if (ret == 1)
			ret = cert_chain_insert(&dev_cert_chain_tree, source);
		else
			ret = cert_chain_insert(&cert_chain_tree, source);
		break;

	case REMOVE_CERT_CHAIN:
		ret = parse_cert_source(args, &source);
		if (ret)
			return ret;

		ret = code_sign_check_code(source->path_type);
		if (ret < 0) {
			free_cert_source(source);
			return ret;
		}
		if (ret == 1)
			ret = cert_chain_remove(&dev_cert_chain_tree, source);
		else
			ret = cert_chain_remove(&cert_chain_tree, source);
		break;

	case ADD_ENTERPRISE_CERT:
		ret = parse_cert_source(args, &source);
		if (ret)
			return ret;
		if (source->path_type == 0)
			source->path_type = RELEASE_DEVELOPER_CODE;
		ret = cert_chain_insert(&cert_chain_tree, source);
		break;

	case REMOVE_ENTERPRISE_CERT:
		ret = parse_cert_source(args, &source);
		if (ret)
			return ret;
		ret = cert_chain_remove(&cert_chain_tree, source);
		break;

	case ACTIVATE_CERT: {
		struct activate_cert_info act;

		BUILD_BUG_ON(sizeof(act) != 16);
		if (!code_sign_is_root_caller())
			return -EPERM;
		if (copy_from_user(&act, (const void __user *)args, sizeof(act)))
			return -EFAULT;
		spin_lock(&cert_chain_tree_lock);
		cert_path_activated = true;
		spin_unlock(&cert_chain_tree_lock);
		code_sign_log_info("cert trust store activated");
		ret = 0;
		break;
	}

	default:
		code_sign_log_error("unsupported code_sign ioctl cmd: 0x%x", cmd);
		ret = -EINVAL;
		break;
	}

	return ret;
}
