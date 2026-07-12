# rp2350-tz-tee

Memory isolation on the **RP2350** (Raspberry Pi Pico 2), on both of its core types:
a hardware-verified recipe for running **Trusted Firmware-M (TF-M)** on the Cortex-M33, a
**minimal SDK-only Arm TrustZone-M example**, and its **RISC-V (Hazard3) PMP sibling** — all
validated on real hardware over SWD.

## Why this exists

The RP2350 has full **TrustZone-M** hardware — Secure/Non-Secure worlds, SAU, secure boot, OTP,
TRNG, and a redundancy coprocessor (RCP). But through Pico SDK 2.x, the SDK **does not yet support
building a Non-Secure binary that runs under a Secure one** (release notes: *"will be added in a
subsequent release"*), and there is **no clean minimal Secure + Non-Secure example** — see
[pico-examples#708](https://github.com/raspberrypi/pico-examples/issues/708) and the recurring
forum threads where people are stuck at exactly this starting point.

This repo fills that gap two ways:

1. A **reproducible, pitfall-documented recipe** to build and run TF-M on the RP2350, verified to
   pass the regression suite on a Pico 2.
2. A **minimal, SDK-only TrustZone-M example** (SAU + DMA-MPU + NSC veneers, no TF-M framework) that
   actually runs on the board — the thing #708 is asking for.

## Status — both verified on a Pico 2 (over SWD, no UART)

| Piece | State |
|---|---|
| TF-M `rpi/rp2350` build + run | **regression 16/16 PASSED** (7 NS + 9 S), read over SWD |
| The real bring-up pitfalls | 7 found & documented ([`docs/tf-m-bringup.md`](docs/tf-m-bringup.md)) |
| Minimal SDK-only TZ example (Arm M33) | **working end-to-end** ([`examples/minimal-tz/`](examples/minimal-tz/)): veneer calls succeed, illegal NS read is blocked as a SecureFault, secret never leaks |
| Minimal PMP example (RISC-V Hazard3) | **working end-to-end** ([`examples/minimal-pmp/`](examples/minimal-pmp/)): U-mode `ecall` services succeed, illegal U-mode read is blocked as a PMP fault, plus the Hazard3 `Xh3pmpm` M-mode-PMP feature |
| RP2350 DMA-MPU mirroring | documented & implemented (RP2350-specific; most M33 tutorials miss it) |

## Layout

```
docs/
  tf-m-bringup.md        # build & run TF-M on RP2350: verified recipe + 7 pitfalls + SWD verdict
scripts/
  read_tfm_results.sh    # judge the TF-M regression purely over SWD (no serial)
99-rpi-debugprobe.rules  # udev: non-root CMSIS-DAP + ttyACM access for the RPi Debug Probe
examples/
  minimal-tz/            # minimal SDK-only Arm TrustZone-M Secure + Non-Secure example (verified)
  minimal-pmp/           # RISC-V (Hazard3) sibling: M-mode/U-mode isolation with PMP (verified)
reference/               # key files extracted from TF-M's rp2350 port, for study (BSD-3, attributed)
  target_cfg.c           #   SAU/IDAU config + DMA-MPU mirroring
  flash_layout.h         #   S/NS flash partition map
  region_defs.h
  linker_s.ld            #   Secure linker script
  linker_ns.ld           #   Non-Secure linker script
  tfm_hal_isolation_rp2350.c  # MPU isolation (Level 1/2)
  pico-sdk.patch         #   the pico-sdk patch TF-M's rp2350 port needs
```

## Where to start

- **Just want a known-good board + toolchain?** Follow [`docs/tf-m-bringup.md`](docs/tf-m-bringup.md)
  to build and run TF-M and confirm **16/16** with `scripts/read_tfm_results.sh`.
- **Want to write your own TrustZone code?** Read [`examples/minimal-tz/`](examples/minimal-tz/) — a
  ~self-contained Secure world that guards a secret behind NSC veneers, a bare-metal Non-Secure
  world that calls in and gets blocked when it reaches across the boundary, and a writeup of the two
  RP2350-specific walls that shape the design.
- **On the RISC-V cores instead?** Read [`examples/minimal-pmp/`](examples/minimal-pmp/) — the same
  isolation with M-mode/U-mode + PMP + `ecall`, including the Hazard3-specific `Xh3pmpm` M-mode-PMP
  feature. A side-by-side Arm-vs-RISC-V mapping is in its README.

## The RP2350 gotcha worth knowing

Most Cortex-M33 TrustZone tutorials stop at "SAU partitions Secure vs Non-Secure." On the RP2350
that is **not enough**: the DMA has **its own MPU** (`dma_hw->mpu_region[]`). Configure only the SAU
and Non-Secure code can program the DMA to read Secure memory and bypass TrustZone entirely. The DMA
MPU must be programmed to **mirror** the SAU partitioning — see
[`examples/minimal-tz/secure/tz_config.c`](examples/minimal-tz/secure/tz_config.c) and
[`reference/target_cfg.c`](reference/target_cfg.c).

## Hardware

- Raspberry Pi **Pico 2** (RP2350); Pico 2 W also fine.
- A **Raspberry Pi Debug Probe** (or any CMSIS-DAP SWD adapter) — everything here is verified over
  SWD, so a UART is optional.
- USB-C data cable (power + BOOTSEL drag-and-drop flashing).

## Reference material

- RP2350 datasheet — security chapter (SAU, ACCESSCTRL, XIP, QMI).
- ARMv8-M Architecture Reference Manual — Security Extension (SAU, NSC, CMSE veneers).
- TF-M rp2350 platform port:
  <https://trustedfirmware-m.readthedocs.io/en/latest/platform/rpi/rp2350/readme.html>
- [pico-examples#708](https://github.com/raspberrypi/pico-examples/issues/708) — the gap this repo targets.

## License

Original notes, scripts, and example code: **BSD-3-Clause** (see [`LICENSE`](LICENSE)).
Files under `reference/` are extracted from Trusted Firmware-M and retain their original
`BSD-3-Clause` headers / copyright (The TrustedFirmware-M Contributors), included for study
convenience — see each file's SPDX header.
