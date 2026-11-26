// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2024 Huawei Device Co., Ltd.
 */

#include <linux/code_sign.h>
#include <linux/cred.h>
#include <linux/key.h>
#include <linux/slab.h>
#include <linux/verification.h>
#include <crypto/pkcs7.h>
#include <../../crypto/asymmetric_keys/pkcs7_parser.h>

#include "code_sign_ext.h"
#include "code_sign_ioctl.h"
#include "code_sign_log.h"
#include "verify_cert_chain.h"

static int pkcs7_find_key(struct pkcs7_message *pkcs7,
			  struct pkcs7_signed_info *sinfo)
{
	struct x509_certificate *cert;
	unsigned int certix = 1;

	code_sign_log_debug("sinfo->index %u", sinfo->index);

	cert = pkcs7->certs;
	while (cert) {
		if (asymmetric_key_id_same(cert->id, sinfo->sig->auth_ids[0])) {
			if (strcmp(cert->pub->pkey_algo, sinfo->sig->pkey_algo) != 0 &&
			    (strncmp(cert->pub->pkey_algo, "ecdsa-", 6) != 0 ||
			     strcmp(sinfo->sig->pkey_algo, "ecdsa") != 0)) {
				code_sign_log_warn("sig %u: X.509 algo '%s' and PKCS#7 sig algo '%s' mismatch",
						   sinfo->index, cert->pub->pkey_algo,
						   sinfo->sig->pkey_algo);
				cert = cert->next;
				certix++;
				continue;
			}
			sinfo->signer = cert;
			return 0;
		}
		cert = cert->next;
		certix++;
	}

	code_sign_log_info("sig %u: issuing X.509 cert not found in pkcs7 (#%*phN)",
			   sinfo->index,
			   sinfo->sig->auth_ids[0]->len, sinfo->sig->auth_ids[0]->data);
	return 0;
}

static void set_file_ownerid(struct cs_info *cs_info, int path_type,
			     struct pkcs7_signed_info *sinfo)
{
	if (!cs_info)
		return;

	if ((path_type > DEBUG_CODE_START) && (path_type < DEBUG_CODE_END)) {
		code_sign_set_ownerid(cs_info, FILE_OWNERID_DEBUG, NULL, 0);
		return;
	}

	if (!sinfo->ownerid) {
		code_sign_set_ownerid(cs_info, FILE_OWNERID_COMPAT, NULL, 0);
		return;
	}

	if ((sinfo->ownerid_len == strlen(OWNERID_SHARED_TAG)) &&
	    !memcmp(sinfo->ownerid, OWNERID_SHARED_TAG, sinfo->ownerid_len)) {
		code_sign_set_ownerid(cs_info, FILE_OWNERID_SHARED, NULL, 0);
		return;
	}

	if ((path_type == MAY_LOCAL_CODE) &&
	    (sinfo->ownerid_len == strlen(OWNERID_DEBUG_TAG)) &&
	    !memcmp(sinfo->ownerid, OWNERID_DEBUG_TAG, sinfo->ownerid_len)) {
		code_sign_set_ownerid(cs_info, FILE_OWNERID_DEBUG, NULL, 0);
		return;
	}

	code_sign_set_ownerid(cs_info, FILE_OWNERID_APP,
			      sinfo->ownerid, sinfo->ownerid_len);
}

static struct cert_source *find_matched_source(const struct x509_certificate *signer,
					       bool is_debug)
{
	int block_type = is_debug ? DEBUG_BLOCK_CODE : RELEASE_BLOCK_CODE;
	struct cert_source *source = find_match(signer->subject, signer->issuer, is_debug);

	if (!source) {
		source = find_match("ALL", signer->issuer, is_debug);
	} else if (source->path_type == block_type) {
		code_sign_log_error("signer certificate '%s' is explicitly blocked",
				    signer->subject);
		return NULL;
	}
	return source;
}

void code_sign_verify_certchain(const void *raw_pkcs7, size_t pkcs7_len,
				struct fsverity_info *vi, int *ret)
{
	struct pkcs7_message *pkcs7;
	struct pkcs7_signed_info *sinfo;
	bool is_dev_mode;

	pkcs7 = pkcs7_parse_message(raw_pkcs7, pkcs7_len);
	if (IS_ERR(pkcs7)) {
		code_sign_log_error("parse pkcs7 message failed: %ld", PTR_ERR(pkcs7));
		*ret = PTR_ERR(pkcs7);
		return;
	}

	if (!pkcs7->signed_infos) {
		code_sign_log_error("signed info not found in pkcs7");
		goto untrusted;
	}

	if (!pkcs7->certs) {
		code_sign_log_warn("no certs in pkcs7, deferring to trust keyring");
		if (vi)
			set_file_ownerid(&vi->fcs_info, MAY_LOCAL_CODE, pkcs7->signed_infos);
		*ret = MAY_LOCAL_CODE;
		goto exit;
	}

	is_dev_mode = READ_ONCE(code_sign_developer_mode) != 0;

	for (sinfo = pkcs7->signed_infos; sinfo; sinfo = sinfo->next) {
		const struct x509_certificate *signer;
		struct cert_source *source;
		int cert_chain_depth_without_root = 1;
		const char *issuer;
		struct x509_certificate *cert;

		*ret = pkcs7_find_key(pkcs7, sinfo);
		if (*ret) {
			code_sign_log_error("key not found in pkcs7");
			goto exit;
		}

		signer = sinfo->signer;
		if (!signer) {
			code_sign_log_error("signer cert not found in pkcs7");
			*ret = -EINVAL;
			goto exit;
		}

		source = find_matched_source(signer, false);
		if (!source && is_dev_mode)
			source = find_matched_source(signer, true);
		if (!source) {
			code_sign_log_error("untrusted signer subject='%s' issuer='%s' dev_mode=%d",
					    signer->subject ? signer->subject : "(null)",
					    signer->issuer ? signer->issuer : "(null)",
					    is_dev_mode ? 1 : 0);
			goto untrusted;
		}

		issuer = signer->issuer;
		cert = pkcs7->certs;
		while (cert && cert_chain_depth_without_root <= CERT_CHAIN_PATH_LEN_MAX) {
			if (cert->subject && issuer && strcmp(cert->subject, issuer) == 0) {
				if (cert->issuer && strcmp(cert->subject, cert->issuer) == 0)
					break;
				cert_chain_depth_without_root++;
				issuer = cert->issuer;
				cert = pkcs7->certs;
			} else {
				cert = cert->next;
			}
		}

		if (cert_chain_depth_without_root == (int)(source->max_path_depth - 1)) {
			code_sign_log_info("cert chain trusted: subject='%s' type=0x%x depth=%d",
					   signer->subject, source->path_type,
					   cert_chain_depth_without_root);
			if (vi)
				set_file_ownerid(&vi->fcs_info, source->path_type,
						 pkcs7->signed_infos);
			*ret = source->path_type;
			goto exit;
		}
		code_sign_log_error("depth mismatch: depth_without_root=%d max_path_depth=%u",
				    cert_chain_depth_without_root, source->max_path_depth);
	}

untrusted:
	code_sign_log_error("cert subject and issuer verify failed");
	*ret = -EKEYREJECTED;
exit:
	pkcs7_free_message(pkcs7);
}
