/* SPDX-License-Identifier: MIT
 * marble local DCM certificate authority.
 *
 * Issues a local-only certificate chain (device cert + root) for the software
 * DCM service. The chain is explicitly NOT Huawei-issued and must never be
 * presented as one; it exists so local callers that only validate structure,
 * cache a chain, or exercise the attestation API get a defined answer.
 */
#ifndef HMOS_DCM_LOCAL_CA_H
#define HMOS_DCM_LOCAL_CA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotently create/load the local CA and device certificate. Returns 0 on
 * success, negative on failure. Safe to call from multiple binder threads. */
int dcm_ca_ensure(void);

/* Serialize the chain as the donor's DcmClientUnpackCertChainFromService
 * format: uint32 certCount, then per cert uint32 length + DER (4-byte aligned).
 * Caller frees *out with dcm_ca_free(). */
int dcm_ca_cert_chain_blob(unsigned char **out, size_t *out_len);

/* Local cert state: 1 when a chain is available, 0 otherwise. */
int dcm_ca_state(void);

/* Per-certificate DER for the anonymous-attest callback (0=device, 1=root).
 * Caller frees *out with dcm_ca_free(). */
int dcm_ca_cert_der(int index, unsigned char **out, size_t *out_len);

void dcm_ca_free(void *ptr);

/* Host self-test: generate/load, verify blob structure and DER, idempotency. */
int dcm_ca_selftest(int verbose);

#ifdef __cplusplus
}
#endif

#endif /* HMOS_DCM_LOCAL_CA_H */
