/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * minimal-tz -- RP2350 TrustZone-M isolation setup (SAU + DMA-MPU + ACCESSCTRL).
 *
 * This is the heart of the example, distilled from TF-M's
 * platform/ext/target/rpi/rp2350/target_cfg.c (see ../../reference/target_cfg.c).
 * Three things must be done together:
 *
 *   1) SAU       -- partition the address map into Secure / Non-Secure / NSC
 *                   regions (the CPU's view).
 *   2) DMA-MPU   -- the RP2350 DMA has its OWN MPU; it must mirror the SAU, or
 *                   Non-Secure code can use the DMA to read Secure memory and
 *                   bypass TrustZone entirely (the RP2350-specific hazard).
 *   3) ACCESSCTRL-- RP2350-specific; gates S/NS/privileged access per peripheral
 *                   and bus manager.
 *
 * Scope: this does only the minimum needed to "let NS run, call Secure through
 * a veneer, and not touch Secure memory". TF-M's original also configures the
 * bootrom NSC region, the MPU (PMSAv8 privileged isolation), per-peripheral
 * ACCESSCTRL, etc.; those are trimmed here for clarity.
 */
#include <stdint.h>
#include "RP2350.h"                 /* CMSIS header (SAU->, SCB->, ...) from pico-sdk */
#include "hardware/structs/dma.h"
#include "hardware/regs/addressmap.h"
#include "tz_layout.h"

/* SAU RLAR enable/NSC bits (CMSIS names: SAU_RLAR_ENABLE_Msk / SAU_RLAR_NSC_Msk). */
#ifndef SAU_RLAR_ENABLE_Msk
#define SAU_RLAR_ENABLE_Msk (1u << 0)
#endif
#ifndef SAU_RLAR_NSC_Msk
#define SAU_RLAR_NSC_Msk    (1u << 1)
#endif
#define SAU_RBAR_BADDR_Msk  (~0x1Fu)   /* base limit is 32-byte aligned */
#define SAU_RLAR_LADDR_Msk  (~0x1Fu)   /* limit is 32-byte aligned (incl. last 32B) */

/*
 * SAU config: carve the address map into S / NS / NSC blocks. See
 * target_cfg.c sau_and_idau_cfg(). The RP2350 IDAU treats the whole map as
 * Secure by default, so we only use the SAU to "carve out" the parts we give to
 * NS, and carve the veneer region as NSC.
 */
void tz_sau_config(void)
{
    /* Region 0: NSC veneer region (top of Secure flash). NS can only enter
     * Secure through the SG stubs here. */
    SAU->RNR  = 0;
    SAU->RBAR = (VENEER_START & SAU_RBAR_BADDR_Msk);
    SAU->RLAR = (VENEER_LIMIT & SAU_RLAR_LADDR_Msk)
                | SAU_RLAR_ENABLE_Msk | SAU_RLAR_NSC_Msk;   /* <- NSC bit */

    /* Region 1: Non-Secure flash partition (where NS code lives). */
    SAU->RNR  = 1;
    SAU->RBAR = (NS_FLASH_START & SAU_RBAR_BADDR_Msk);
    SAU->RLAR = (NS_FLASH_LIMIT & SAU_RLAR_LADDR_Msk) | SAU_RLAR_ENABLE_Msk;

    /* Region 2: Non-Secure RAM. */
    SAU->RNR  = 2;
    SAU->RBAR = (NS_RAM_START & SAU_RBAR_BADDR_Msk);
    SAU->RLAR = (NS_RAM_LIMIT & SAU_RLAR_LADDR_Msk) | SAU_RLAR_ENABLE_Msk;

    /* Region 3: peripherals + the rest of the map up to PPB, given to NS, so
     * NS can use UART/GPIO/etc. A stricter design opens only the peripherals
     * that are needed and gates them via ACCESSCTRL. */
    SAU->RNR  = 3;
    SAU->RBAR = (0x40000000u & SAU_RBAR_BADDR_Msk);          /* peripheral base */
    SAU->RLAR = ((PPB_BASE - 1u) & SAU_RLAR_LADDR_Msk) | SAU_RLAR_ENABLE_Msk;

    /* Enable the SAU. */
    SAU->CTRL = 1u; /* ENABLE=1, ALLNS=0 */

    /* Route faults so the NS world can receive BusFault/HardFault, and so a NS
     * SecureFault enters the Secure fault handler. Configure SCB->AIRCR's
     * BFHFNMINS/PRIS as needed. */
    SCB->AIRCR = (0x05FAu << 16)
               | (SCB->AIRCR & 0x0000FFFFu)
               | SCB_AIRCR_BFHFNMINS_Msk;   /* BusFault/HardFault/NMI available to NS */

    __DSB();
    __ISB();
}

/*
 * DMA-MPU config: mirror the SAU. This block is the RP2350-specific part that
 * generic M33 tutorials omit. See target_cfg.c dma_security_config(). Without
 * it, Non-Secure code can program the DMA to move Secure memory, making
 * TrustZone useless.
 */
#define DMA_MPU_BAR_ADDR_Msk   (~0x1Fu)
#define DMA_MPU_LAR_ADDR_Msk   (~0x1Fu)
#define DMA_MPU_LAR_P_Msk      (1u << 1)   /* Privileged */
#define DMA_MPU_LAR_EN_Msk     (1u << 0)   /* Enable */

void tz_dma_mpu_config(void)
{
    /* Unmapped regions default to Secure+Privileged (mpu_ctrl S/P bits). */
    dma_hw->mpu_ctrl = (1u << 0) | (1u << 1) | (1u << 2);
    /* Note: the SDK bit names are DMA_MPU_CTRL_P/S/NS_HIDE_ADDR; values here
     * follow target_cfg.c. */

    /* region 0: NSC veneer (matches SAU region 0). */
    dma_hw->mpu_region[0].bar = (VENEER_START & DMA_MPU_BAR_ADDR_Msk);
    dma_hw->mpu_region[0].lar = (VENEER_LIMIT & DMA_MPU_LAR_ADDR_Msk)
                              | DMA_MPU_LAR_P_Msk | DMA_MPU_LAR_EN_Msk;

    /* region 1: NS flash partition. */
    dma_hw->mpu_region[1].bar = (NS_FLASH_START & DMA_MPU_BAR_ADDR_Msk);
    dma_hw->mpu_region[1].lar = (NS_FLASH_LIMIT & DMA_MPU_LAR_ADDR_Msk)
                              | DMA_MPU_LAR_P_Msk | DMA_MPU_LAR_EN_Msk;

    /* region 2: NS RAM + peripherals (SRAM4 up to PPB). */
    dma_hw->mpu_region[2].bar = (NS_RAM_START & DMA_MPU_BAR_ADDR_Msk);
    dma_hw->mpu_region[2].lar = ((PPB_BASE - 1u) & DMA_MPU_LAR_ADDR_Msk)
                              | DMA_MPU_LAR_P_Msk | DMA_MPU_LAR_EN_Msk;

    /* regions 3..7 disabled. */
    for (int i = 3; i < 8; i++) {
        dma_hw->mpu_region[i].bar = 0;
        dma_hw->mpu_region[i].lar = 0;
    }
}

/*
 * One-shot config: SAU + DMA-MPU. Fine-grained per-peripheral ACCESSCTRL is
 * kept simple here (after reset RP2350 ACCESSCTRL lets Secure access
 * everything; to strictly isolate DMA/peripherals for NS, configure each one
 * per target_cfg.c peripheral_secure_cfg()).
 */
void tz_isolation_config(void)
{
    tz_sau_config();
    tz_dma_mpu_config();
}
