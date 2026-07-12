# minimal-tz — a minimal RP2350 TrustZone-M example

**Goal:** the clean Secure + Non-Secure example that
[pico-examples#708](https://github.com/raspberrypi/pico-examples/issues/708) is asking for —
SDK-only, no full TF-M framework, small enough to actually understand.

> **Status (2026-07-12): working on a Pico 2, verified over SWD.** Secure boots, programs the
> isolation, copies the Non-Secure image into NS SRAM and `BLXNS`-hands off; the Non-Secure world
> calls the Secure services through the CMSE veneers, then its illegal read of Secure RAM is caught
> as a SecureFault by the Secure world — with the secret never leaked. All confirmed by reading the
> result mailboxes over SWD (no UART needed). See **On-board result** below for the exact captured
> values, and **How it got here** for the two RP2350-specific walls this had to route around (which
> are exactly why pico-sdk 2.1.1 can't yet build a stock NS image — the #708 gap).

## What it demonstrates

A Secure world that owns a secret, and a Non-Secure world that can only touch it through a
gate:

- **Secure side** holds a secret (a key + a monotonic counter) in Secure RAM. It exposes two
  **Non-Secure Callable (NSC)** veneers: `secure_get_counter()` and `secure_sign()`
  (`out[i] = in[i] ^ key[i]`). The key never leaves the Secure world.
- **Non-Secure side** (a tiny bare-metal image) calls the veneers and gets *results* — the counter
  values and the signed bytes — then deliberately does a direct read of Secure RAM, which faults.
- **No UART is wired**, so results are published to fixed mailboxes (one in NS RAM, one in Secure
  RAM) and read over SWD. The Secure fault handler snapshots the NS mailbox when it catches the
  illegal access, giving a single consolidated record: NS called in, got its results, and was
  blocked the instant it touched Secure memory — `leaked == 0`.

## Why it's more than a toy

Most Cortex-M33 TrustZone tutorials stop at "S and NS partitions." This example additionally
points out a **RP2350-specific hazard almost everyone misses**:

> The RP2350 DMA has **its own MPU** (`dma_hw->mpu_region[]`). If you only configure the SAU,
> Non-Secure code can program the DMA to read Secure memory and bypass TrustZone entirely.
> The DMA MPU must be configured to **mirror** the SAU partitioning.

(See `../../reference/target_cfg.c`, "Configure DMA MPU to mirror SAU settings".)

## Structure

```
minimal-tz/
  CMakeLists.txt          # Secure = pico-sdk image; Non-Secure = freestanding image + CMSE implib
  include/
    tz_layout.h           # S/NS/NSC flash + RAM addresses + shared mailbox (one place)
  secure/
    main_s.c              # Secure entry: SAU+DMA-MPU+ACCESSCTRL, copy NS to RAM, BLXNS; SecureFault handler
    tz_config.c           # the SAU + RP2350 DMA-MPU programming (the isolation heart)
    secure_service.c      # the NSC veneers (cmse_nonsecure_entry) + NS-pointer checks
    secret.c / secret.h   # the guarded key + counter, Secure-only
    linker_s.ld           # Secure map; pins .gnu.sgstubs veneers to the SAU NSC region
  nonsecure/
    main_ns.c             # Non-Secure: bare metal (own vectors/reset, no libc); calls veneers, illegal read
    linker_ns.ld          # Non-Secure map: RAM-resident (0x20040000), minimal, no pico-sdk
  README.md               # this file
```

## Key techniques (with the reference pointers)

| Technique | Where to look |
|---|---|
| SAU region setup (S/NS/NSC) | `reference/target_cfg.c` → `sau_and_idau_cfg()` |
| Flash S/NS partition addresses | `reference/flash_layout.h` |
| NSC veneer / `cmse_nonsecure_entry` | ARMv8-M ARM, Security Extension; `arm_cmse.h` |
| Reserving NS region in linker | `reference/linker_s.ld` / `linker_ns.ld` |
| DMA-MPU mirroring (RP2350 gotcha) | `reference/target_cfg.c` DMA MPU block |
| Booting NS from S | bootrom `secure_call()` / setting NS vector table + MSP_NS, then `BLXNS` |

## Build (verified)

Standard pico-sdk out-of-tree build; produces two images (`minimal_tz_s`, `minimal_tz_ns`) plus a
CMSE import library that links them.

```bash
# WORK = the workspace where you set up the toolchain + pico-sdk
# (see ../../docs/tf-m-bringup.md, Steps 1 and 3)
export WORK=~/rp2350-tz
export PATH="$WORK/arm-gnu-toolchain-13.3.rel1-aarch64-arm-none-eabi/bin:$PATH"
export PICO_SDK_PATH="$WORK/pico-sdk"

cmake -S examples/minimal-tz -B examples/minimal-tz/build -G Ninja
cmake --build examples/minimal-tz/build
```

Outputs: `minimal_tz_s.{elf,bin,uf2}` (Secure, boots at `0x10000000`) and `minimal_tz_ns.bin`
(the ~200-byte Non-Secure RAM image; the Secure world copies it into NS SRAM at runtime).

What the build proves (checkable with `arm-none-eabi-nm`/`objdump`):

- The Secure image's `.gnu.sgstubs` veneer section is pinned at `0x100FF000` — exactly the region
  `tz_config.c` marks Non-Secure-Callable (SAU region 0): `secure_sign`@`0x100FF000`,
  `secure_get_counter`@`0x100FF008`; the real bodies are `__acle_se_*` in Secure `.text`.
- The Non-Secure image resolves `secure_get_counter`/`secure_sign` to those SG addresses via the
  `--out-implib` import library — it links against the gateway, never the Secure implementation.
- NS is linked RAM-resident: vector[0] = `0x20080000` (top of NS RAM), vector[1] = `ns_reset`.

## Flash + run (SWD)

```bash
# an OpenOCD build that knows the rp2350 (see ../../docs/tf-m-bringup.md, Step 4)
OCD=$WORK/openocd-rpi/src/openocd
$OCD -s $WORK/openocd-rpi/tcl -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "init" -c "reset halt" \
  -c "program build/minimal_tz_s.elf verify" \
  -c "program build/minimal_tz_ns.bin 0x10100000 verify" \
  -c "reset run" -c "sleep 1500" -c "halt" \
  -c "mdw 0x200006dc 8"   `# Secure result mailbox (s_result)` \
  -c "mdw 0x2007c000 7"   `# Non-Secure mailbox` \
  -c "shutdown"
```

## On-board result (captured over SWD)

```
s_result @0x200006dc:  5ecf0000 00000048 00000001 00000002 00000000 a9dd58a4 00000000 0000bad0
                        MARK     SFSR     CNT1     CNT2     SIGN_RC  SIGN_R0  LEAKED   NS_STAGE
NS mailbox @0x2007c000: 4e530001 00000001 00000002 00000000 a9dd58a4 00000000 0000bad0
                        MAGIC    CNT1     CNT2     SIGN_RC  SIGN_R0  LEAKED   STAGE
```

Reading it out:

- **Handoff:** Secure booted, programmed SAU+DMA-MPU+ACCESSCTRL, copied the NS image into SRAM
  and `BLXNS`-ed in; NS came up (`MAGIC = 0x4E530001`).
- **Veneers work:** NS called `secure_get_counter()` twice → **1, 2**; `secure_sign()` → **rc 0**,
  result `a9dd58a4` = challenge `01 02 03 04` XOR key `A5 5A DE AD` (little-endian). The key never
  left Secure.
- **Isolation holds:** NS's direct read of Secure RAM raised a SecureFault, caught by the Secure
  world (`MARK = 0x5ECF0000`, `SFSR = 0x48` = **AUVIOL | SFARVALID** — an attribution-unit
  violation). **`LEAKED = 0`** — the secret word was never obtained; NS stopped at `NS_STAGE 0xBAD0`
  (right at the illegal read). The Secure PC halts in its fault handler.

## How it got here (two RP2350 walls)

The obvious design — a stock pico-sdk NS image executing in place from flash — does **not** work on
RP2350 in this bare-boot (no BL2 / no secure-boot) setup. Two walls, both found on hardware over SWD,
both the same shape (**the debugger reads the real value; the Non-Secure CPU reads something else**):

1. **NS data reads of XIP flash return 0.** With SAU + ACCESSCTRL verified correct, a full pico-sdk
   NS image still faults immediately in `runtime_run_initializers`: it loads an init-array pointer
   from flash and gets `0` (the debug-AP reads the real pointer at the same address). Fix: link NS
   **RAM-resident** and have Secure (which *can* read flash) copy the image into NS SRAM before
   `BLXNS`. NS then only ever reads SRAM.
2. **NS reads of peripherals mismatch too.** Running from RAM, the pico-sdk NS runtime next hangs in
   `runtime_init_early_resets`, polling `RESETS_RESET_DONE` forever — the debugger sees the bits set,
   the NS CPU doesn't. I.e. it isn't only flash; the full pico-sdk runtime re-initialising hardware
   the Secure world already owns is not viable here.

So the Non-Secure side is deliberately **bare metal**: its own vector table + reset, no libc, no
clock/reset setup, no UART. It touches only NS SRAM + the veneers + the one illegal read. That is
enough to demonstrate the whole TrustZone boundary, and it sidesteps both walls. (Booting a *stock*
pico-sdk NS image is the pico-sdk 2.1.1 gap that #708 is about.)

The "does my board + toolchain work" smoke test remains the TF-M regression in
[`../../docs/tf-m-bringup.md`](../../docs/tf-m-bringup.md) (16/16 PASSED, judged over pure SWD).

## Toolchain reminder

Use `arm-gnu-toolchain` **from Arm's website**, not `apt` (see
[`../../docs/tf-m-bringup.md`](../../docs/tf-m-bringup.md) Step 1 — TF-M blacklists the Ubuntu
13.2.1 build; for a pure pico-sdk example the apt one *may* work, but staying on the Arm build
avoids surprises).
