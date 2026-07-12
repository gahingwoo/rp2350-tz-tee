/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * minimal-tz -- the guarded secret, and the Secure-only logic that uses it.
 *
 * Everything here is linked into the Secure image and lands in Secure flash /
 * Secure RAM (see linker_s.ld). Because the SAU (tz_config.c) marks these
 * addresses Secure, a Non-Secure load/store to them raises a SecureFault.
 * NS can only observe the *results* returned through the NSC veneers.
 */
#include "secret.h"

/*
 * The key. Marked so it is placed in initialised .data in Secure RAM (not
 * .rodata in shared-readable flash), reinforcing that it lives behind the
 * Secure boundary. In a real design this would come from OTP / a KDF, never a
 * literal -- this is a teaching stand-in.
 */
static volatile uint8_t s_key[SECRET_KEY_LEN] = {
    0xA5, 0x5A, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11,
    0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
};

/* Secure-owned mutable state. */
static volatile uint32_t s_counter = 0u;

uint32_t secret_next_counter(void)
{
    return ++s_counter;
}

void secret_keyed_transform(const uint8_t *in, uint8_t *out, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(in[i] ^ s_key[i % SECRET_KEY_LEN]);
    }
}
