/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * minimal-tz -- Secure world entry.
 *
 * Boot order on RP2350:
 *   bootrom -> (BL2 if present) -> THIS Secure image at 0x10000000.
 * The Secure image owns the chip first, then hands the CPU to the Non-Secure
 * image at 0x10100000. Steps:
 *
 *   1. Bring up hardware normally with the SDK (clocks, UART) -- Secure owns
 *      the UART and prints the Secure-side log.
 *   2. tz_isolation_config(): program the SAU + the RP2350 DMA-MPU so the
 *      Secure/Non-Secure boundary exists (see tz_config.c).
 *   3. tz_accessctrl_open_ns(): the SAU decides S/NS for the *core*, but on
 *      RP2350 each peripheral is additionally gated by ACCESSCTRL. Open the
 *      peripherals (and GPIOs) the Non-Secure image needs, or its very first
 *      UART/GPIO access faults before the demo even starts.
 *   4. launch_nonsecure(): copy the Non-Secure image from its flash storage
 *      into Non-Secure SRAM, set the NS vector table + NS stack, then BLXNS
 *      into the NS reset handler. Normally never returns.
 *
 * WHY THE COPY (RP2350 gotcha): a Non-Secure *data* read of XIP flash returns 0
 * on RP2350 in this bare-boot (no BL2 / no secure-boot) setup, even with SAU +
 * ACCESSCTRL correctly granting NS access (measured over SWD). So the NS image
 * cannot run XIP-in-place -- its C runtime would read 0 for the first constant
 * it loads from flash and fault. Instead the NS image is built RAM-resident
 * (nonsecure/linker_ns.ld, a pico-sdk `no_flash` image) and stored as a blob at
 * NS_FLASH_START; the Secure world (which CAN read flash) copies it into
 * NS_RAM_START here, so the Non-Secure world only ever touches SRAM.
 *
 * Booting a full pico-sdk Non-Secure image is not something pico-sdk 2.1.1
 * officially supports yet (this repo/example is exactly the gap that
 * pico-examples#708 asks to fill).
 */
#include <stdint.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "RP2350.h"                     /* CMSIS: SCB_NS, __TZ_set_MSP_NS, ... */
#include "hardware/structs/accessctrl.h"
#include "tz_layout.h"

/* Provided by tz_config.c */
void tz_isolation_config(void);

/* ACCESSCTRL write password: bits [31:16] must be 0xACCE on every write. */
#define ACCESSCTRL_PASSWORD 0xACCE0000u

/* Per-peripheral access bits (see reference/target_cfg.c). */
#define AC_DBG   (1u << 7)
#define AC_DMA   (1u << 6)
#define AC_CORE1 (1u << 5)
#define AC_CORE0 (1u << 4)
#define AC_SP    (1u << 3)              /* Secure, Privileged      */
#define AC_SU    (1u << 2)              /* Secure, Unprivileged    */
#define AC_NSP   (1u << 1)              /* Non-secure, Privileged  */
#define AC_NSU   (1u << 0)              /* Non-secure, Unprivileged*/
#define AC_ALL   (AC_DBG | AC_DMA | AC_CORE1 | AC_CORE0 | \
                  AC_SP | AC_SU | AC_NSP | AC_NSU)

/* The per-peripheral access registers occupy this offset window in ACCESSCTRL
 * (ROM at 0x14 ... XIP_AUX at 0xe8), each a 32-bit register. */
#define AC_PERIPH_FIRST 0x14u
#define AC_PERIPH_LAST  0xe8u

/*
 * Open every peripheral and every GPIO to Non-Secure access. This is the
 * deliberately permissive choice for a *minimal* example: we want the NS image
 * to be able to use UART/GPIO/timer/etc. without enumerating each one. A real
 * product would open only what NS legitimately needs and keep the rest Secure.
 */
static void tz_accessctrl_open_ns(void)
{
    volatile uint8_t *base = (volatile uint8_t *)accessctrl_hw;

    /* Reset ACCESSCTRL config to defaults (keeps LOCK / FORCE_CORE_NS). */
    accessctrl_hw->cfgreset = ACCESSCTRL_PASSWORD | 0x1u;

    /* Allow all GPIOs to be driven from Non-Secure. */
    accessctrl_hw->gpio_nsmask[0] = 0xFFFFFFFFu;
    accessctrl_hw->gpio_nsmask[1] = 0xFFFFFFFFu;

    /* Grant NS (and keep S) access on every peripheral register. */
    for (uint32_t off = AC_PERIPH_FIRST; off <= AC_PERIPH_LAST; off += 4u) {
        *(volatile uint32_t *)(base + off) = ACCESSCTRL_PASSWORD | AC_ALL;
    }
    __DSB();
    __ISB();
}

/*
 * Secure-side result mailbox, in Secure RAM. Filled by the Secure fault handler
 * when it catches the Non-Secure world's illegal read. Read back over SWD
 * (no UART wired) to confirm the demo end-to-end. Kept in a named section so
 * its address is easy to find in the map.
 */
volatile uint32_t s_result[8] __attribute__((used, section(".s_result")));
enum {
    SR_MARK = 0,    /* 0x5ECF0000 == SecureFault caught (NS access blocked) */
    SR_SFSR,        /* Secure Fault Status Register (0xE000EDE4)            */
    SR_NS_CNT1,     /* copied from NS mailbox: counter call 1              */
    SR_NS_CNT2,     /* counter call 2                                      */
    SR_NS_SIGN_RC,  /* secure_sign() return code                          */
    SR_NS_SIGN_R0,  /* secure_sign() result[0..3]                         */
    SR_NS_LEAKED,   /* NS "leaked" word -- must stay 0 (isolation held)   */
    SR_NS_STAGE     /* NS progress marker -- 0xBAD0 == faulted on the read */
};

/*
 * Secure fault handler. The Non-Secure illegal read of Secure RAM raises a
 * SecureFault; with SECUREFAULTENA off it escalates to the Secure HardFault,
 * which this overrides (pico-sdk's isr_hardfault is weak). We record the fault
 * and snapshot the Non-Secure mailbox -- proving NS reached the Secure services
 * successfully but was blocked the instant it touched Secure memory.
 */
void isr_hardfault(void)
{
    volatile uint32_t *ns = (volatile uint32_t *)NS_MAILBOX_ADDR;
    s_result[SR_MARK]       = 0x5ECF0000u;
    s_result[SR_SFSR]       = *(volatile uint32_t *)0xE000EDE4u;  /* SFSR */
    s_result[SR_NS_CNT1]    = ns[1];
    s_result[SR_NS_CNT2]    = ns[2];
    s_result[SR_NS_SIGN_RC] = ns[3];
    s_result[SR_NS_SIGN_R0] = ns[4];
    s_result[SR_NS_LEAKED]  = ns[5];
    s_result[SR_NS_STAGE]   = ns[6];

    printf("[S] SecureFault: NS illegal read blocked. "
           "NS counters=%u,%u sign_rc=%d leaked=0x%08x\n",
           (unsigned)ns[1], (unsigned)ns[2], (int)ns[3], (unsigned)ns[5]);
    for (;;) {
        tight_loop_contents();
    }
}

/* A Non-Secure function-pointer type: calling through it emits BLXNS. */
typedef void __attribute__((cmse_nonsecure_call)) (*ns_entry_t)(void);

/* How many bytes of the NS blob to copy flash -> NS RAM. Must cover the NS
 * image's loadable size (code + rodata + .data); it stays well below the NS
 * stack at the top of NS RAM. The NS image is a few tens of KB. */
#define NS_IMAGE_COPY_BYTES 0x00010000u   /* 64 KB */

static void launch_nonsecure(void)
{
    /* Secure copies the RAM-resident NS image from its flash blob into NS RAM.
     * Secure reads of flash work; this is the whole point (see file header). */
    const uint32_t *src = (const uint32_t *)NS_FLASH_START;   /* blob in flash */
    uint32_t       *dst = (uint32_t *)NS_RAM_START;           /* NS exec base  */
    for (uint32_t i = 0; i < NS_IMAGE_COPY_BYTES / 4u; i++) {
        dst[i] = src[i];
    }
    __DSB();
    __ISB();

    /* The NS vector table is now at NS_RAM_START. */
    const uint32_t *ns_vt = (const uint32_t *)NS_RAM_START;
    uint32_t ns_msp   = ns_vt[0];       /* NS initial MSP  (vector[0]) */
    uint32_t ns_reset = ns_vt[1];       /* NS reset handler(vector[1]) */

    /* Point the Non-Secure world at its own vector table and stack. */
    SCB_NS->VTOR = NS_RAM_START;
    __TZ_set_MSP_NS(ns_msp);

    /* Branch into Non-Secure. Clear the LSB: BLXNS takes a non-Thumb address
     * and the cmse_nonsecure_call veneer handles the state switch. */
    ns_entry_t ns_entry = (ns_entry_t)(ns_reset & ~1u);
    ns_entry();
}

int main(void)
{
    stdio_init_all();
    printf("\n[S] secure world up @ 0x%08x\n", (unsigned)S_FLASH_START);

    tz_isolation_config();
    printf("[S] SAU + DMA-MPU configured (S/NS boundary is live)\n");

    tz_accessctrl_open_ns();
    printf("[S] ACCESSCTRL: peripherals + GPIO opened to Non-Secure\n");

    printf("[S] copying NS image 0x%08x -> 0x%08x, then launching ...\n",
           (unsigned)NS_FLASH_START, (unsigned)NS_RAM_START);

    launch_nonsecure();

    /* Reached only if NS returns (it shouldn't in this demo). */
    printf("[S] Non-Secure returned unexpectedly -- halting\n");
    for (;;) {
        tight_loop_contents();
    }
    return 0;
}
