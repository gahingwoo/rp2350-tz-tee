/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * minimal-tz -- Non-Secure world, bare metal (no pico-sdk runtime).
 *
 * WHY BARE METAL: on RP2350, in this SDK-only bare-boot setup (no BL2 / no
 * secure-boot), a Non-Secure image cannot run pico-sdk's full C runtime. NS
 * reads of XIP flash return 0, and even after relocating to RAM the NS runtime
 * hangs re-initialising the chip (clocks/resets) that the Secure world already
 * owns -- measured on hardware. So the Non-Secure side here is deliberately
 * tiny and freestanding: its own vector table + reset, no libc, no clock/reset
 * setup, no UART. It only does what a Non-Secure client must:
 *
 *   1. call the Secure services through the NSC veneers, and
 *   2. attempt a direct read of Secure memory, which must fault.
 *
 * The whole image is copied into Non-Secure SRAM by the Secure world before the
 * BLXNS handoff (main_s.c), so NS only ever touches SRAM.
 *
 * No UART is wired, so results are published to a fixed NS-RAM mailbox
 * (NS_MAILBOX_ADDR). The Secure fault handler and an SWD debugger both read it.
 */
#include <stdint.h>
#include "tz_layout.h"

/* Resolved to the Secure SG veneers (0x100FF000 region) via the CMSE import
 * library the Secure build produced. NS gets the gateways, not the code. */
extern uint32_t secure_get_counter(void);
extern int      secure_sign(const uint8_t *challenge, uint8_t *out, uint32_t len);

/* Shared mailbox (see tz_layout.h). Index meanings: */
#define NS_MB ((volatile uint32_t *)NS_MAILBOX_ADDR)
enum {
    MB_MAGIC = 0,   /* 0x4E530001 once NS is alive                      */
    MB_CNT1,        /* secure_get_counter() call 1                      */
    MB_CNT2,        /* secure_get_counter() call 2                      */
    MB_SIGN_RC,     /* secure_sign() return code (0 = ok)               */
    MB_SIGN_R0,     /* result[0..3] packed little-endian                */
    MB_LEAKED,      /* secret word -- written ONLY if isolation FAILED  */
    MB_STAGE,       /* progress marker                                  */
    MB_N
};

static void ns_main(void)
{
    for (int i = 0; i < MB_N; i++) {
        NS_MB[i] = 0;
    }
    NS_MB[MB_MAGIC] = 0x4E530001u;   /* "NS" v1 up */
    NS_MB[MB_STAGE] = 1u;

    /* 1) Legitimate path: call Secure services through the veneers. NS gets the
     *    results but never sees the key or the counter storage itself. */
    NS_MB[MB_CNT1] = secure_get_counter();
    NS_MB[MB_CNT2] = secure_get_counter();
    NS_MB[MB_STAGE] = 2u;

    uint8_t challenge[16];
    uint8_t result[16];
    for (int i = 0; i < 16; i++) {
        challenge[i] = (uint8_t)(i + 1);
        result[i]    = 0;
    }
    int rc = secure_sign(challenge, result, sizeof(challenge));
    NS_MB[MB_SIGN_RC] = (uint32_t)rc;
    NS_MB[MB_SIGN_R0] = (uint32_t)result[0]
                      | ((uint32_t)result[1] << 8)
                      | ((uint32_t)result[2] << 16)
                      | ((uint32_t)result[3] << 24);
    NS_MB[MB_STAGE] = 3u;

    /* 2) Illegal path: read Secure RAM directly. The SAU marks it Secure, so a
     *    Non-Secure load raises a SecureFault -- taken in the Secure world (see
     *    main_s.c isr_hardfault). Execution must not continue past this load. */
    NS_MB[MB_STAGE] = 0x0000BAD0u;
    volatile uint32_t leaked = *(volatile uint32_t *)S_RAM_START;

    /* Only reached if isolation FAILED (should never happen). */
    NS_MB[MB_LEAKED] = leaked;
    NS_MB[MB_STAGE] = 0x0000FA11u;
    for (;;) {
    }
}

/* ---------------------------------------------------------------------------
 * Bare-metal startup. Entered via BLXNS from the Secure world at vector[1];
 * the Secure side has already set MSP_NS to vector[0]. No crt0: we just zero
 * .bss and jump to ns_main. .data initial values are already in place because
 * the Secure side copies the whole linked image into RAM (VMA == LMA).
 * ------------------------------------------------------------------------- */
extern uint32_t __bss_start__;
extern uint32_t __bss_end__;
extern uint32_t __ns_stack_top;

void ns_reset(void)
{
    for (uint32_t *p = &__bss_start__; p < &__bss_end__; p++) {
        *p = 0u;
    }
    ns_main();
    for (;;) {
    }
}

/* Default handler for any Non-Secure-state exception (not expected in this
 * demo -- the illegal read faults into the Secure world, not here). */
static void ns_default_handler(void)
{
    NS_MB[MB_STAGE] = 0x0000DEADu;
    for (;;) {
    }
}

/* Minimal Non-Secure vector table, placed at the image base by linker_ns.ld.
 * word[0] = initial MSP, word[1] = reset -- the two words the Secure side
 * reads to launch this image. Entries are function pointers (the Thumb bit is
 * already set in a function's address), so this is a valid constant table. */
typedef void (*ns_vector_t)(void);

__attribute__((section(".vectors"), used))
const ns_vector_t ns_vectors[] = {
    (ns_vector_t)&__ns_stack_top,   /* 0: initial SP */
    ns_reset,                       /* 1: Reset      */
    ns_default_handler,             /* 2: NMI        */
    ns_default_handler,             /* 3: HardFault  */
};
