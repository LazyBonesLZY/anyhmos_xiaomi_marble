// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023 Huawei Device Co., Ltd.
 */

#include <linux/cdev.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include "code_sign_ext.h"
#include "code_sign_ioctl.h"
#include "code_sign_log.h"

static const struct file_operations code_sign_ops = {
	.owner = THIS_MODULE,
	.open = code_sign_open,
	.release = code_sign_release,
	.unlocked_ioctl = code_sign_ioctl,
	.compat_ioctl = code_sign_ioctl,
};

static struct miscdevice code_sign_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "code_sign",
	.fops = &code_sign_ops,
	.mode = 0660,
};

static int __init code_sign_init(void)
{
	int ret;

	code_sign_init_salt();
	ret = misc_register(&code_sign_misc);
	if (ret)
		code_sign_log_error("misc_register failed: %d", ret);
	else
		code_sign_log_info("registered /dev/code_sign (minor=%d)", code_sign_misc.minor);
	return ret;
}

static void __exit code_sign_exit(void)
{
	misc_deregister(&code_sign_misc);
	code_sign_log_info("unregistered /dev/code_sign");
}

module_init(code_sign_init);
module_exit(code_sign_exit);

MODULE_LICENSE("GPL");
