/* SPDX-License-Identifier: MIT
 * Local device-token derivation for the marble DCM software-TEE replacement.
 *
 * The token is derived from values that exist on this device (serial number,
 * model, optional UDID) and signed with a per-device key derived from the same
 * values. It is NOT a Huawei-issued attestation and must never be presented as
 * one; see docs/MARBLE_DCM_SOFT_TEE.md for the evidence boundary.
 */
#ifndef HMOS_DCM_TOKEN_H
#define HMOS_DCM_TOKEN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DCM_TOKEN_PREFIX "hmosdcm1."
#define DCM_TOKEN_VERSION 1

/* Build "hmosdcm1.<base64url(payload)>.<hex hmac-sha256>" into out.
 * sn/model/udid may be NULL or empty; they are sanitised to [A-Za-z0-9._:-].
 * Returns 0 on success, negative errno-style value on failure. */
int dcm_build_token(const char *sn, const char *model, const char *udid,
                    uint64_t request_id, int64_t issued_at,
                    char *out, size_t out_cap);

/* First 16 hex chars of SHA-256(token); for logs that must not carry the token. */
int dcm_token_digest(const char *token, char hex[17]);

/* Self-test incl. SHA-256 / HMAC-SHA256 known-answer vectors and a token round
 * trip. Returns 0 when every check passes, 1 otherwise. */
int dcm_token_selftest(int verbose);

#ifdef __cplusplus
}
#endif

#endif /* HMOS_DCM_TOKEN_H */
