/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * minimal-pmp -- RP2350 Hazard3 (RISC-V) memory isolation with PMP.
 *
 * The RISC-V sibling of examples/minimal-tz (ARM TrustZone-M). Same idea, RISC-V
 * primitives:
 *
 *     minimal-tz (M33)          minimal-pmp (Hazard3)
 *     ----------------          ---------------------
 *     Secure world      <->     M-mode
 *     Non-Secure world  <->     U-mode
 *     SAU region        <->     PMP region
 *     NSC veneer (SG)   <->     ecall (U -> M trap)
 *     illegal NS read   <->     illegal U-mode read
 *     -> SecureFault    <->     -> PMP load access fault
 *     Secure handler    <->     M-mode trap handler (mcause/mtval)
 *
 * What this does, all in one run:
 *   1. M-mode puts a secret + a signing key in a PMP-protected region that
 *      U-mode cannot touch, and installs a trap handler.
 *   2. Xh3pmpm demo (Hazard3-specific): using the custom PMPCFGM0 CSR, M-mode
 *      makes the protected region apply to *itself*, reads it, takes its own
 *      PMP fault, then lifts the restriction -- showing M-mode can be sandboxed
 *      reversibly, which base RISC-V can only do by permanently locking a region.
 *   3. M-mode drops to U-mode (mret).
 *   4. U-mode calls M-mode services via `ecall` (get counter, sign a challenge)
 *      and gets results -- but never the key.
 *   5. U-mode reads the protected secret directly -> PMP load access fault ->
 *      the M-mode handler catches it (mcause=5) and halts, with leaked == 0.
 *
 * No UART is wired: results go to a fixed RAM mailbox (g_mb) read over SWD.
 */
#include <stdint.h>
#include "pico/stdlib.h"

/* ---- CSR access ---- */
#define read_csr(csr) ({ uint32_t v_; __asm__ volatile ("csrr %0, " #csr : "=r"(v_)); v_; })
#define write_csr(csr, val) __asm__ volatile ("csrw " #csr ", %0" :: "r"((uint32_t)(val)))
/* Hazard3 custom CSR: PMP M-mode config. Bit i makes PMP entry i apply to
 * M-mode too (like pmpcfg.L) but WITHOUT locking it. */
#define CSR_PMPCFGM0 0xbd0
#define write_csr_num(num, val) __asm__ volatile ("csrw %0, %1" :: "i"(num), "r"((uint32_t)(val)))

/* ---- PMP config byte fields (packed 4-per-word in pmpcfg0) ---- */
#define PMP_R        (1u << 0)
#define PMP_W        (1u << 1)
#define PMP_X        (1u << 2)
#define PMP_A_NAPOT  (3u << 3)   /* address matching = NAPOT */
/* L (lock, bit 7) left 0: M-mode is not subject to the entry by default, so
 * M-mode keeps access to the secret while U-mode is denied. */

/* ---- U -> M service numbers (passed in a7, RISC-V syscall convention) ---- */
#define SVC_GET_COUNTER 0u
#define SVC_SIGN        1u

/* ---- mailbox in RAM, read over SWD (no UART) ---- */
volatile uint32_t g_mb[16] __attribute__((used, section(".uninitialized_data")));
enum {
    MB_MAGIC = 0,     /* 0x504D5002 once M-mode is up ("PMP" v2)        */
    MB_XH3_BEFORE,    /* secret read by M-mode before Xh3pmpm (works)   */
    MB_XH3_MCAUSE,    /* M-mode's own PMP fault cause under Xh3pmpm (=5) */
    MB_XH3_AFTER,     /* secret read by M-mode after lifting Xh3pmpm     */
    MB_U_UP,          /* 0x55 once U-mode is running                    */
    MB_CNT1,          /* ecall SVC_GET_COUNTER -> 1                      */
    MB_CNT2,          /* ecall SVC_GET_COUNTER -> 2                      */
    MB_SIGN0,         /* ecall SVC_SIGN result[0..3], key never exposed  */
    MB_MCAUSE,        /* U-mode illegal read: trap cause (5 = load fault)*/
    MB_MTVAL,         /* faulting address (Hazard3 may report 0)         */
    MB_LEAKED,        /* secret -- written ONLY if isolation failed      */
    MB_STAGE,         /* progress marker; 0x600D = fault caught          */
    MB_N
};

/*
 * The protected region: secret word + M-mode counter + signing key, in one
 * 64-byte NAPOT-aligned block that the PMP denies to U-mode.
 */
struct prot_region {
    volatile uint32_t secret;    /* the word U-mode must never read */
    volatile uint32_t counter;   /* M-mode monotonic counter        */
    uint8_t           key[16];   /* signing key, never leaves M     */
    uint8_t           pad[64 - 4 - 4 - 16];
};
static struct prot_region g_prot __attribute__((aligned(64)));

/* M-mode trap stack (kept out of U-mode's stack via mscratch). */
static uint32_t g_m_trap_stack[128];

/* Stage flag so the trap handler can tell the Xh3pmpm self-test (recover) from
 * the U-mode isolation demo (halt). */
enum { STAGE_XH3 = 1, STAGE_U = 2 };
static volatile uint32_t g_stage;

/* NAPOT pmpaddr for [base, base+size): size a power of two >= 8, base aligned. */
static inline uint32_t pmp_napot(uintptr_t base, uint32_t size)
{
    return (uint32_t)((base >> 2) | ((size >> 3) - 1u));
}

/* ------------------------------------------------------------------ */
/* M-mode service implementations (the "Secure services")             */
/* ------------------------------------------------------------------ */

/* Keyed transform in place: buf[i] ^= key[i % 16]. The key stays in the
 * protected region; only the transformed bytes go back to U-mode. */
static void svc_sign(uint8_t *buf, uint32_t len)
{
    /* Basic guard: refuse buffers that overlap the protected region (a
     * malicious U-mode pointer must not trick M-mode into touching secrets).
     * This is the analog of minimal-tz's cmse_check_address_range(). */
    uintptr_t b = (uintptr_t)buf;
    uintptr_t p = (uintptr_t)&g_prot;
    if (len > 64u) {
        return;
    }
    if (b + len > p && b < p + sizeof(g_prot)) {
        return;   /* overlaps the secret region -- reject */
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(buf[i] ^ g_prot.key[i % 16]);
    }
}

/* ------------------------------------------------------------------ */
/* Trap handling                                                      */
/* ------------------------------------------------------------------ */

/* Register frame the assembly trampoline saves for the C dispatcher. */
typedef struct {
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint32_t t0, t1, t2, t3, t4, t5, t6, ra;
} trap_frame_t;

/* C dispatcher. Returns for ecall / recoverable Xh3 self-test (the trampoline
 * then mrets); loops forever for the U-mode isolation fault. */
void trap_dispatch(trap_frame_t *f)
{
    uint32_t cause = read_csr(mcause);
    uint32_t epc   = read_csr(mepc);

    if (cause == 8u) {                       /* environment call from U-mode */
        switch (f->a7) {
        case SVC_GET_COUNTER:
            f->a0 = ++g_prot.counter;
            break;
        case SVC_SIGN:                       /* a0 = buf ptr, a1 = len */
            svc_sign((uint8_t *)f->a0, f->a1);
            f->a0 = 0u;
            break;
        default:
            f->a0 = 0xFFFFFFFFu;
            break;
        }
        write_csr(mepc, epc + 4u);           /* ecall is a 4-byte instruction */
        return;
    }

    if (cause == 5u || cause == 7u) {        /* load / store access fault */
        if (g_stage == STAGE_XH3) {
            /* Hazard3 Xh3pmpm self-test: M-mode faulted on its own region.
             * Record, lift the M-mode restriction, skip the load, resume. */
            g_mb[MB_XH3_MCAUSE] = cause;
            write_csr_num(CSR_PMPCFGM0, 0u); /* M-mode no longer subject to PMP */
            g_stage = STAGE_U;
            /* Skip the faulting load. Read the opcode as a 16-bit halfword --
             * epc may be only 2-byte aligned (compressed), so a 32-bit read
             * here would itself fault. Low 2 bits == 0b11 -> 4-byte instr. */
            uint16_t opcode = *(volatile uint16_t *)epc;
            write_csr(mepc, epc + (((opcode & 3u) == 3u) ? 4u : 2u));
            return;
        }
        /* U-mode isolation demo: the secret read was blocked. Record and halt. */
        g_mb[MB_MCAUSE] = cause;
        g_mb[MB_MTVAL]  = read_csr(mtval);
        g_mb[MB_STAGE]  = 0x0000600Du;       /* caught */
        for (;;) {
        }
    }

    g_mb[MB_STAGE] = 0x0000DEADu;            /* unexpected trap */
    for (;;) {
    }
}

/* Assembly trap trampoline: switch to the M-mode trap stack via mscratch, save
 * caller-saved registers, call the C dispatcher, restore, mret. 4-byte aligned
 * for mtvec direct mode. */
__attribute__((naked, aligned(4)))
void trap_entry(void)
{
    __asm__ volatile (
        "csrrw sp, mscratch, sp   \n"   /* sp <- M trap stack; mscratch <- old sp */
        "addi  sp, sp, -64        \n"
        "sw a0,  0(sp)  \n" "sw a1,  4(sp)  \n" "sw a2,  8(sp)  \n" "sw a3, 12(sp) \n"
        "sw a4, 16(sp)  \n" "sw a5, 20(sp)  \n" "sw a6, 24(sp)  \n" "sw a7, 28(sp) \n"
        "sw t0, 32(sp)  \n" "sw t1, 36(sp)  \n" "sw t2, 40(sp)  \n" "sw t3, 44(sp) \n"
        "sw t4, 48(sp)  \n" "sw t5, 52(sp)  \n" "sw t6, 56(sp)  \n" "sw ra, 60(sp) \n"
        "mv a0, sp                \n"
        "call trap_dispatch       \n"
        "lw a0,  0(sp)  \n" "lw a1,  4(sp)  \n" "lw a2,  8(sp)  \n" "lw a3, 12(sp) \n"
        "lw a4, 16(sp)  \n" "lw a5, 20(sp)  \n" "lw a6, 24(sp)  \n" "lw a7, 28(sp) \n"
        "lw t0, 32(sp)  \n" "lw t1, 36(sp)  \n" "lw t2, 40(sp)  \n" "lw t3, 44(sp) \n"
        "lw t4, 48(sp)  \n" "lw t5, 52(sp)  \n" "lw t6, 56(sp)  \n" "lw ra, 60(sp) \n"
        "addi sp, sp, 64          \n"
        "csrrw sp, mscratch, sp   \n"   /* restore old sp; mscratch <- M trap stack */
        "mret                     \n"
    );
}

/* ------------------------------------------------------------------ */
/* U-mode                                                             */
/* ------------------------------------------------------------------ */

static inline uint32_t u_ecall(uint32_t svc, uint32_t arg0, uint32_t arg1)
{
    register uint32_t a0 __asm__("a0") = arg0;
    register uint32_t a1 __asm__("a1") = arg1;
    register uint32_t a7 __asm__("a7") = svc;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

__attribute__((used))
static void u_mode_entry(void)
{
    g_mb[MB_U_UP] = 0x55u;                        /* U-mode is alive */

    /* Legitimate path: call M-mode services through ecall. */
    g_mb[MB_CNT1] = u_ecall(SVC_GET_COUNTER, 0, 0);
    g_mb[MB_CNT2] = u_ecall(SVC_GET_COUNTER, 0, 0);

    static uint8_t challenge[16];
    for (int i = 0; i < 16; i++) {
        challenge[i] = (uint8_t)(i + 1);
    }
    u_ecall(SVC_SIGN, (uint32_t)(uintptr_t)challenge, 16);
    g_mb[MB_SIGN0] = (uint32_t)challenge[0]
                   | ((uint32_t)challenge[1] << 8)
                   | ((uint32_t)challenge[2] << 16)
                   | ((uint32_t)challenge[3] << 24);

    /* Illegal path: read the protected secret directly. The PMP denies U-mode,
     * so this load traps to the M-mode handler and never completes. */
    g_mb[MB_STAGE] = 0x0000BAD0u;
    uint32_t leaked = g_prot.secret;

    /* Only reached if isolation FAILED. */
    g_mb[MB_LEAKED] = leaked;
    g_mb[MB_STAGE]  = 0x0000FA11u;
    for (;;) {
    }
}

/* ------------------------------------------------------------------ */
/* M-mode setup + demo                                                */
/* ------------------------------------------------------------------ */

static void pmp_isolate_secret(void)
{
    /* Entry 0 (highest priority): the protected region, NO permissions ->
     * any U-mode access faults. */
    write_csr(pmpaddr0, pmp_napot((uintptr_t)&g_prot, sizeof(g_prot)));
    /* Entry 1: everything else, RWX -> U-mode can run/read/write its own code,
     * stack, and the mailbox. All-ones pmpaddr = NAPOT covering the whole map. */
    write_csr(pmpaddr1, 0xFFFFFFFFu);
    uint32_t cfg = ((PMP_A_NAPOT | PMP_R | PMP_W | PMP_X) << 8)   /* entry1 allow */
                 |  (PMP_A_NAPOT);                                 /* entry0 deny  */
    write_csr(pmpcfg0, cfg);
    write_csr_num(CSR_PMPCFGM0, 0u);   /* entries do NOT apply to M-mode (yet) */
}

int main(void)
{
    for (int i = 0; i < MB_N; i++) {
        g_mb[i] = 0;
    }
    g_mb[MB_MAGIC] = 0x504D5002u;      /* "PMP" v2: M-mode up */

    g_prot.secret  = 0x5EC00001u;
    g_prot.counter = 0u;
    for (int i = 0; i < 16; i++) {
        g_prot.key[i] = (uint8_t)(0xA5u + i);
    }

    /* Trap vector (direct mode) + M-mode trap stack. */
    write_csr(mscratch, (uintptr_t)&g_m_trap_stack[128]);
    write_csr(mtvec, (uintptr_t)trap_entry);

    pmp_isolate_secret();

    /* --- Xh3pmpm demo (Hazard3-specific) --- */
    g_mb[MB_XH3_BEFORE] = g_prot.secret;           /* M reads the secret: works */
    g_stage = STAGE_XH3;
    write_csr_num(CSR_PMPCFGM0, 0x1u);             /* make entry 0 apply to M-mode */
    { volatile uint32_t t = g_prot.secret; (void)t; }  /* M read -> PMP fault -> handler recovers */
    g_mb[MB_XH3_AFTER] = g_prot.secret;            /* handler lifted it: M reads again */

    /* --- drop to U-mode --- */
    g_stage = STAGE_U;
    g_mb[MB_STAGE] = 0x00000A11u;
    uint32_t ms = read_csr(mstatus);
    ms &= ~(3u << 11);                             /* mstatus.MPP = U */
    write_csr(mstatus, ms);
    write_csr(mepc, (uintptr_t)u_mode_entry);
    __asm__ volatile ("mret");

    for (;;) {
    }
    return 0;
}
