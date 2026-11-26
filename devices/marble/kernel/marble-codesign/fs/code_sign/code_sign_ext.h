/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2023 Huawei Device Co., Ltd.
 */

#ifndef _CODE_SIGN_EXT_H
#define _CODE_SIGN_EXT_H

#include <linux/xpm_types.h>

#define OWNERID_SYSTEM_TAG "SYSTEM_LIB_ID"
#define OWNERID_DEBUG_TAG  "DEBUG_LIB_ID"
#define OWNERID_SHARED_TAG "SHARED_LIB_ID"
#define OWNERID_COMPAT_TAG "COMPAT_LIB_ID"

enum file_ownerid_type {
	FILE_OWNERID_UNINT = 0,
	FILE_OWNERID_SYSTEM,
	FILE_OWNERID_APP,
	FILE_OWNERID_DEBUG,
	FILE_OWNERID_SHARED,
	FILE_OWNERID_COMPAT,
	FILE_OWNERID_MAX
};

void code_sign_init_salt(void);
void code_sign_set_ownerid(struct cs_info *cs_info, uint32_t id_type,
			   const char *id_str, uint32_t id_len);

#endif /* _CODE_SIGN_EXT_H */
