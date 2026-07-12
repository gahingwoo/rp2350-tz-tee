# minimal-pmp -- a minimal RP2350 RISC-V (Hazard3) PMP isolation example

The RISC-V sibling of [`../minimal-tz`](../minimal-tz/). The RP2350 has two
Hazard3 RISC-V cores alongside the Arm Cortex-M33 pair; this shows the same
Secure/Non-Secure isolation idea using RISC-V M-mode/U-mode and **PMP** instead
of Arm TrustZone and the SAU.

**Status: working on a Pico 2, verified over SWD.** No UART; results are read
from a RAM mailbox.

## The mapping

| minimal-tz (Cortex-M33)        | minimal-pmp (Hazard3 RISC-V)          |
|--------------------------------|---------------------------------------|
| Secure world                   | M-mode                                |
| Non-Secure world               | U-mode                                |
| SAU region                     | PMP region                            |
| NSC veneer / SG gateway        | `ecall` (U-mode traps into M-mode)    |
| S -> NS via `BLXNS`            | M -> U via `mret`                     |
| illegal NS read -> SecureFault | illegal U-mode read -> PMP load fault |
| Secure handler (SFSR/SFAR)     | M-mode trap handler (`mcause`/`mtval`)|
| `leaked == 0`                  | `leaked == 0`                         |

## What it demonstrates

A single run, all read back from `g_mb` at `0x20000000`:

1. **Isolation** -- M-mode puts a secret + a signing key in a 64-byte
   PMP-protected region (NAPOT). U-mode is denied; M-mode is not.
2. **Xh3pmpm** (Hazard3-specific) -- using the custom `PMPCFGM0` CSR (`0xbd0`),
   M-mode makes the region apply to *itself*, reads it, takes its own PMP fault,
   then lifts the restriction and reads again. Base RISC-V can only subject
   M-mode to a PMP region by permanently *locking* it (until reset); Hazard3's
   `Xh3pmpm` makes that reversible.
3. **Services via `ecall`** -- U-mode calls M-mode services (get a counter, sign
   a challenge with `out[i] = in[i] ^ key[i]`). U-mode gets results; the key
   never leaves M-mode. This is the veneer analog.
4. **Blocked access** -- U-mode reads the protected secret directly. The PMP
   raises a load access fault; the M-mode trap handler catches it and halts,
   with `leaked == 0`.

## Structure

```
minimal-pmp/
  CMakeLists.txt   # pico-sdk RISC-V build (PICO_PLATFORM=rp2350-riscv)
  main.c           # everything: PMP setup, Xh3pmpm demo, M-mode trap handler
                   #   (asm trampoline + C dispatcher), ecall services, U-mode
  README.md        # this file
```

## Build

Needs a RISC-V toolchain that knows the Hazard3 extensions
(`rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb`). The Raspberry Pi
`riscv-toolchain-15` (`riscv32-unknown-elf-gcc`, from the
[pico-sdk-tools](https://github.com/raspberrypi/pico-sdk-tools/releases)
releases) is verified -- the distro `apt` toolchain is too old / lacks the
bitmanip extensions, the RISC-V analog of minimal-tz's "not the apt toolchain".

```bash
export WORK=~/rp2350-tz
export PICO_SDK_PATH=$WORK/pico-sdk
export PICO_TOOLCHAIN_PATH=$WORK/riscv-toolchain/bin   # the RISC-V GCC
cmake -S examples/minimal-pmp -B examples/minimal-pmp/build -G Ninja
cmake --build examples/minimal-pmp/build
# outputs: build/minimal_pmp.{elf,bin,uf2}
```

## Flash + run (SWD)

Use an rp2350-aware OpenOCD (see [`../../docs/tf-m-bringup.md`](../../docs/tf-m-bringup.md)
Step 4) with the **RISC-V** target config. You can flash over SWD (no BOOTSEL):

```bash
OCD=$WORK/openocd-rpi/src/openocd
$OCD -s $WORK/openocd-rpi/tcl -f interface/cmsis-dap.cfg -f target/rp2350-riscv.cfg \
  -c "init" -c "halt" \
  -c "program build/minimal_pmp.elf verify" \
  -c "reset run" -c "sleep 1000" -c "halt" \
  -c "mdw 0x20000000 12" \
  -c "shutdown"
```

## On-board result (captured over SWD)

```
g_mb @0x20000000:
  [0]  MAGIC       0x504d5002
  [1]  XH3_BEFORE  0x5ec00001   M-mode reads the secret: allowed
  [2]  XH3_MCAUSE  0x00000005   M-mode's own PMP fault via Xh3pmpm (load access fault)
  [3]  XH3_AFTER   0x5ec00001   after lifting Xh3pmpm, M-mode reads again
  [4]  U_UP        0x00000055   U-mode is running
  [5]  CNT1        0x00000001   ecall get_counter
  [6]  CNT2        0x00000002
  [7]  SIGN0       0xaca4a4a4   ecall sign: 01 02 03 04 XOR key A5 A6 A7 A8; key never left M
  [8]  MCAUSE      0x00000005   U-mode illegal read -> load access fault
  [9]  MTVAL       0x00000000   (see note)
  [10] LEAKED      0x00000000   the secret was never obtained
  [11] STAGE       0x0000600d   fault caught
```

Live at the halt: `mcause = 5`, `mepc` points at the U-mode `lw` that read the
secret, and `pc` sits in the M-mode trap handler's halt loop.

## RISC-V notes / gotchas

- **`mtval` reads 0.** For PMP access faults Hazard3 does not report the faulting
  address (the spec permits `mtval = 0` here), so unlike Arm's `SFAR` the address
  isn't available -- `mcause = 5` is the definitive signal.
- **Reading the faulting opcode.** To skip the instruction after the Xh3pmpm
  self-test fault, the handler reads the opcode at `mepc` as a **16-bit**
  halfword: `mepc` can be only 2-byte aligned (compressed instruction), and a
  32-bit read there would itself fault (`mcause = 4`, load misaligned).
- **PMP granule is 32 bytes** on this Hazard3, and `pmpaddr` is a limited width,
  so read-back values are masked -- the NAPOT region still covers the secret.
- **Stack guards are off by default** (`PICO_USE_STACK_GUARDS = 0`), so PMP
  entry 0 is free; if you enable them, pico-sdk uses entry 0 + `PMPCFGM0` itself.
- No copy-to-RAM needed (unlike minimal-tz's Non-Secure side): RISC-V U-mode
  reads flash normally once a PMP entry grants it -- the RP2350 XIP "NS data
  reads return 0" wall is an Arm Secure/Non-Secure quirk, not a RISC-V one.
