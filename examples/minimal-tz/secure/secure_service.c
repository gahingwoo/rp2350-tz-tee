/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * minimal-tz -- the Non-Secure Callable (NSC) gate.
 *
 * Each function tagged `cmse_nonsecure_entry` becomes a *veneer*: the linker
 * emits an SG (Secure Gateway) stub into the .gnu.sgstubs section, which
 * linker_s.ld pins to the veneer region at the top of Secure flash. The SAU
 * marks exactly that region Non-Secure-Callable (tz_config.c, SAU region 0).
 * NS code branches to the veneer, the SG instruction switches the CPU to
 * Secure state, and only then does the real Secure body run.
 *
 * When the Secure build links, GCC's `-Wl,--cmse-implib --out-implib=...`
 * produces an import library exporting just these veneer addresses. The
 * Non-Secure image links against that import lib instead of this object, so
 * NS gets the SG entry points but none of the Secure implementation.
 *
 * SECURITY RULE for any NSC entry: never trust a pointer handed in by NS.
 * NS could pass a Secure address to trick us into reading/writing across the
 * boundary on its behalf. cmse_check_address_range() verifies the range is
 * genuinely accessible to the *Non-Secure* side before we touch it.
 */
#include <arm_cmse.h>
#include <stdint.h>
#include "secret.h"

/* Max bytes we are willing to transform in one call (bounds the loop). */
#define SECURE_SIGN_MAX 256u

/*
 * Advance and return the Secure-owned counter. No NS pointers involved, so no
 * range check needed. NS learns the value but cannot read the counter storage.
 */
__attribute__((cmse_nonsecure_entry))
uint32_t secure_get_counter(void)
{
    return secret_next_counter();
}

/*
 * Keyed transform of an NS-provided challenge into an NS-provided buffer.
 * Returns 0 on success, negative on a rejected/invalid request.
 *
 * Both buffers come from NS, so both must be validated to be NS-accessible.
 * We copy through small Secure stack buffers so the Secure transform never
 * dereferences the NS pointers directly during the keyed step.
 */
__attribute__((cmse_nonsecure_entry))
int secure_sign(const uint8_t *challenge, uint8_t *out, uint32_t len)
{
    if (len == 0u || len > SECURE_SIGN_MAX) {
        return -1;
    }

    /* CMSE_NONSECURE | CMSE_MPU_READ: the range must be readable from NS. */
    if (cmse_check_address_range((void *)challenge, len,
                                 CMSE_NONSECURE | CMSE_MPU_READ) == NULL) {
        return -2;
    }
    /* Output must be writable from NS. */
    if (cmse_check_address_range(out, len,
                                 CMSE_NONSECURE | CMSE_MPU_READWRITE) == NULL) {
        return -3;
    }

    uint8_t tmp_in[SECURE_SIGN_MAX];
    uint8_t tmp_out[SECURE_SIGN_MAX];

    for (uint32_t i = 0; i < len; i++) {
        tmp_in[i] = challenge[i];
    }

    secret_keyed_transform(tmp_in, tmp_out, len);

    for (uint32_t i = 0; i < len; i++) {
        out[i] = tmp_out[i];
    }

    return 0;
}
