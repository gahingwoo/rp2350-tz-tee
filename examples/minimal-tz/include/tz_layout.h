/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * minimal-tz -- RP2350 TrustZone-M memory layout (SDK-only, no BL2/TF-M).
 * Simplified from TF-M's flash_layout.h/region_defs.h (see ../../reference/):
 * no MCUboot (BL2), no image headers. Secure image boots from the start of
 * flash via the RP2350 bootrom; the Non-Secure image is a second binary the
 * Secure world jumps into.
 *
 * Flash (2 MB XIP @ 0x10000000):
 *   0x10000000  Secure code / rodata / NSC veneers   (S_FLASH, 1 MB)
 *     last VENEER_SIZE bytes = NSC veneer region (SAU marks NSC)
 *   0x10100000  Non-Secure code                       (NS_FLASH, 1 MB)
 * SRAM (520 KB @ 0x20000000):
 *   0x20000000  Secure RAM      (S_RAM, 256 KB)
 *   0x20040000  Non-Secure RAM  (NS_RAM, 256 KB)
 */
#ifndef MINIMAL_TZ_LAYOUT_H
#define MINIMAL_TZ_LAYOUT_H

#define S_FLASH_START      0x10000000u
#define S_FLASH_SIZE       0x00100000u
#define S_FLASH_LIMIT      (S_FLASH_START + S_FLASH_SIZE - 1u)

#define VENEER_SIZE        0x00001000u
#define VENEER_START       (S_FLASH_START + S_FLASH_SIZE - VENEER_SIZE)
#define VENEER_LIMIT       (S_FLASH_START + S_FLASH_SIZE - 1u)

#define NS_FLASH_START     0x10100000u
#define NS_FLASH_SIZE      0x00100000u
#define NS_FLASH_LIMIT     (NS_FLASH_START + NS_FLASH_SIZE - 1u)

#define S_RAM_START        0x20000000u
#define S_RAM_SIZE         0x00040000u
#define S_RAM_LIMIT        (S_RAM_START + S_RAM_SIZE - 1u)

#define NS_RAM_START       0x20040000u
#define NS_RAM_SIZE        0x00040000u
#define NS_RAM_LIMIT       (NS_RAM_START + NS_RAM_SIZE - 1u)

/*
 * Fixed mailbox in Non-Secure RAM, shared by both worlds at a hard-coded
 * address (no symbol sharing needed). The Non-Secure side writes its results
 * here; with no UART wired, both the Secure fault handler and an SWD debugger
 * read them back. Sits below the NS stack (top of NS RAM) and above the NS
 * image (copied to NS_RAM_START), so it collides with neither.
 */
#define NS_MAILBOX_ADDR    0x2007C000u

#endif /* MINIMAL_TZ_LAYOUT_H */
