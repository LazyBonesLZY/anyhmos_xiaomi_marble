/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2023-2024 Huawei Device Co., Ltd.
 * Ported for Xiaomi marble Linux 5.10 kernel without HCK indirection.
 */

#ifndef LINUX_INCLUDE_CODE_SIGN_H
#define LINUX_INCLUDE_CODE_SIGN_H

#include <asm/byteorder.h>
#include <linux/types.h>

struct inode;
struct fsverity_info;

/*
 * Merkle tree properties.  The file measurement is the hash of this structure
 * excluding the signature and with the sig_size field set to 0, while version
 * is replaced by code sign version for legacy v1 descriptors.
 */
struct code_sign_descriptor {
	__u8 version;		/* must be 1 */
	__u8 hash_algorithm;	/* Merkle tree hash algorithm */
	__u8 log_blocksize;	/* log2 of size of data and tree blocks */
	__u8 salt_size;		/* size of salt in bytes; 0 if none */
	__le32 sig_size;	/* size of signature in bytes; 0 if none */
	__le64 data_size;	/* size of file the Merkle tree is built over */
	__u8 root_hash[64];	/* Merkle tree root hash */
	__u8 salt[32];		/* salt prepended to each hashed block */
	__le32 flags;
	__le32 pgtypeinfo_size;	/* size of page type info (in number of bits) */
	__le64 tree_offset;	/* merkle tree offset in file */
	__le64 pgtypeinfo_off;	/* offset of page type info */
	__u8 __reserved2[119];	/* must be 0's */
	__u8 cs_version;	/* code sign version */
	__u8 signature[];	/* optional PKCS#7 signature */
};

enum {
	RELEASE_CODE_START = 0x0,
	RELEASE_PLATFORM_CODE,
	RELEASE_AUTHED_CODE,
	RELEASE_DEVELOPER_CODE,
	RELEASE_BLOCK_CODE,
	RELEASE_CODE_END,
	RELEASE_RESTRICTED_CODE = 0xff,

	DEBUG_CODE_START = 0x100,
	DEBUG_PLATFORM_CODE,
	DEBUG_AUTHED_CODE,
	DEBUG_DEVELOPER_CODE,
	DEBUG_BLOCK_CODE,
	DEBUG_DEBUG_CODE,
	DEBUG_CODE_END,
	DEBUG_RESTRICTED_CODE = 0x1ff,

	MAY_LOCAL_CODE = 0x201,
};

#define FLAG_INSIDE_TREE	(1U << 0) /* Merkle tree in file */
#define IS_INSIDE_TREE(desc)	(le32_to_cpu((desc)->flags) & FLAG_INSIDE_TREE)

#define CONST_CAST_CODE_SIGN_DESC(desc) ((const struct code_sign_descriptor *)(desc))
#define CAST_CODE_SIGN_DESC(desc) ((struct code_sign_descriptor *)(desc))

static inline u64 get_tree_offset_compact(const void *desc)
{
	return le64_to_cpu(CONST_CAST_CODE_SIGN_DESC(desc)->tree_offset);
}

static inline bool is_inside_tree_compact(const void *_desc)
{
	const struct code_sign_descriptor *desc = CONST_CAST_CODE_SIGN_DESC(_desc);

	return desc->cs_version && IS_INSIDE_TREE(desc);
}

#ifdef CONFIG_SECURITY_CODE_SIGN

extern int code_sign_developer_mode;

void code_sign_check_descriptor(const struct inode *inode, const void *desc, int *ret);
void code_sign_before_measurement(void *desc, int *ret);
void code_sign_after_measurement(void *desc, int version);
void code_sign_verify_certchain(const void *raw_pkcs7, size_t pkcs7_len,
				struct fsverity_info *vi, int *ret);

static inline int code_sign_check_descriptor_hook(const struct inode *inode, const void *desc)
{
	int ret = 0;

	code_sign_check_descriptor(inode, desc, &ret);
	return ret;
}

static inline int code_sign_before_measurement_hook(void *desc)
{
	int ret = 0;

	code_sign_before_measurement(desc, &ret);
	return ret;
}

static inline void code_sign_after_measurement_hook(void *desc, int version)
{
	code_sign_after_measurement(desc, version);
}

#else /* !CONFIG_SECURITY_CODE_SIGN */

static inline int code_sign_check_descriptor_hook(const struct inode *inode, const void *desc)
{
	return 0;
}

static inline int code_sign_before_measurement_hook(void *desc)
{
	return 0;
}

static inline void code_sign_after_measurement_hook(void *desc, int version)
{
}

static inline void code_sign_verify_certchain(const void *raw_pkcs7, size_t pkcs7_len,
					      struct fsverity_info *vi, int *ret)
{
	*ret = 0;
}

#endif /* !CONFIG_SECURITY_CODE_SIGN */

#endif /* LINUX_INCLUDE_CODE_SIGN_H */
