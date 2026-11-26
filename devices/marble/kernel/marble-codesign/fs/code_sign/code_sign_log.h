/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2023 Huawei Device Co., Ltd.
 */

#ifndef _CODE_SIGN_LOG_H
#define _CODE_SIGN_LOG_H

#include <linux/printk.h>

#define CODE_SIGN_LOG_TAG "CODE_SIGN"

#define code_sign_log_error(fmt, ...) \
	pr_err(CODE_SIGN_LOG_TAG ": [%s:%d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

#define code_sign_log_warn(fmt, ...) \
	pr_warn(CODE_SIGN_LOG_TAG ": [%s:%d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

#define code_sign_log_info(fmt, ...) \
	pr_info(CODE_SIGN_LOG_TAG ": [%s:%d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

#define code_sign_log_debug(fmt, ...) \
	pr_debug(CODE_SIGN_LOG_TAG ": [%s:%d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

#endif /* _CODE_SIGN_LOG_H */
