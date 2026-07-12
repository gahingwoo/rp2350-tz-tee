/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * minimal-tz -- Secure-only interface to the guarded secret.
 *
 * This header is compiled ONLY into the Secure image. The Non-Secure world
 * never sees these symbols; it can reach the secret's *effect* solely through
 * the NSC veneers in secure_service.c. The secret itself lives in Secure RAM,
 * which the SAU marks Secure, so any direct NS access faults.
 */
#ifndef MINIMAL_TZ_SECRET_H
#define MINIMAL_TZ_SECRET_H

#include <stdint.h>

/* Length of the guarded key, in bytes. */
#define SECRET_KEY_LEN 16u

/*
 * Return a monotonically increasing counter. Each call bumps it by one.
 * Demonstrates Secure-owned mutable state that NS can advance but not read
 * directly.
 */
uint32_t secret_next_counter(void);

/*
 * Compute a trivial keyed transform of `in[0..len)` into `out[0..len)`:
 *   out[i] = in[i] ^ key[i % SECRET_KEY_LEN]
 * This is a stand-in for "sign with a key that never leaves Secure world".
 * The key is never copied out; only the transformed bytes are returned.
 */
void secret_keyed_transform(const uint8_t *in, uint8_t *out, uint32_t len);

#endif /* MINIMAL_TZ_SECRET_H */
