# Running Trusted Firmware-M on the RP2350 (Pico 2)

A reproducible, hardware-verified recipe for building **Trusted Firmware-M (TF-M)** for the
`rpi/rp2350` platform and running its Secure + Non-Secure regression suite on a real Raspberry Pi
Pico 2 — plus the pitfalls that actually bite along the way.

**Verified result:** on a Pico 2 the TF-M `profile_medium` regression passes **16/16 test suites**
(7 Non-Secure + 9 Secure), confirmed **purely over SWD** — no UART required (see
[Step 8](#step-8-check-the-result-over-swd-no-uart)).

Getting this to pass proves your toolchain, your board, and the whole RP2350 Secure/Non-Secure
boot + provisioning + signing flow work end-to-end. It's the recommended smoke test before writing
your own TrustZone code (for which see [`examples/minimal-tz`](../examples/minimal-tz/)).

---

## Prerequisites

**Hardware**
- Raspberry Pi **Pico 2** (RP2350). Pico 2 W works too.
- A **Raspberry Pi Debug Probe** (or any CMSIS-DAP SWD adapter) wired to the Pico 2's 3-pin debug
  connector (SWCLK / GND / SWDIO). This is all you need — the verdict is read over SWD.
- A USB-C data cable (power + BOOTSEL drag-and-drop flashing).
- *(Optional)* a USB-UART adapter if you also want the serial log; not required.

**Host**
- Linux (this was done on Ubuntu 24.04, arm64 under Parallels; x86-64 is fine too).
- `git cmake ninja-build python3 python3-pip srecord` and the ability to build native tools.

**Workspace convention.** This guide keeps everything under a single workspace directory. Adjust to
taste; all commands below assume:

```bash
export WORK=~/rp2350-tz
mkdir -p "$WORK" && cd "$WORK"
```

---

## Step 1 — Toolchain (not the apt one)

**Pitfall #1: do not use `apt install gcc-arm-none-eabi`.** Ubuntu/Debian ship
`arm-none-eabi-gcc … 13.2.1 20231009`, and TF-M **blacklists that exact version string** in
`config/check_config.cmake` — the build aborts with
`INVALID CONFIG: GCC_VERSION_DETAILED = arm-none-eabi-gcc … 13.2.1 20231009`.

Download the toolchain from Arm instead. `13.3.rel1` (`13.3.1 20240614`) is verified working:

```bash
cd "$WORK"
# pick the tarball matching your host arch (aarch64 shown; use x86_64 on Intel/AMD hosts)
wget "https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-aarch64-arm-none-eabi.tar.xz" -O armgcc.tar.xz
tar xf armgcc.tar.xz
export PATH="$WORK/arm-gnu-toolchain-13.3.rel1-aarch64-arm-none-eabi/bin:$PATH"
arm-none-eabi-gcc --version   # must print 13.3.1, NOT 13.2.1
```

---

## Step 2 — Python deps and TF-M's own tools

```bash
pip3 install --user pyelftools cryptography pyyaml jinja2 cbor2 click imgtool
```

**Pitfall #2: `mcuboot_imagesign_wrapper: not found` at the signing step.** This wrapper is **not**
`imgtool`; it is a console-script that TF-M's own `tools/` package registers on install. You must
install TF-M itself as an editable Python package so the command lands on your `PATH`:

```bash
# after cloning TF-M in Step 3:
pip3 install --user -e "$WORK/trusted-firmware-m"
which mcuboot_imagesign_wrapper          # -> ~/.local/bin/mcuboot_imagesign_wrapper
which mcuboot_imagesign_assemble tfm_gen_armclang_shared_symbols
```

**Pitfall #3: `libclang` missing.** Part of the build parses headers via libclang; install both the
Python binding and the system library:

```bash
pip3 install --user libclang            # verified with 18.1.1
sudo apt install -y libclang-18-dev     # provides libclang-18.so.18
python3 -c "import clang; print('ok')"
```

---

## Step 3 — Clone sources, patch the SDK, tag TF-M

```bash
cd "$WORK"
git clone https://github.com/TrustedFirmware-M/trusted-firmware-m.git
git clone https://github.com/TrustedFirmware-M/tf-m-tests.git
git clone --branch 2.1.1 https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init && cd ..
git clone https://github.com/microsoft/uf2.git

# TF-M's rp2350 port needs a small pico-sdk patch (3 files):
cd pico-sdk
git apply ../trusted-firmware-m/platform/ext/target/rpi/rp2350/pico-sdk.patch
cd ..
```

**Pitfall #4: `check_version` fails on a shallow/tagless clone.** TF-M derives its version from
`git describe --tags`. A clone with no tags aborts with
`fatal: No names found, cannot describe anything`. Add a version tag that matches your checkout:

```bash
cd "$WORK/trusted-firmware-m"
git tag TF-Mv2.3.0
git describe --tags        # now resolves
cd "$WORK"
```

---

## Step 4 — Build an RP2350-aware OpenOCD (jimtcl first)

**Pitfall #5: distro OpenOCD doesn't know the RP2350.** Ubuntu's `openocd 0.12.0` has no
`target/rp2350.cfg`. Build the Raspberry Pi fork (verified at the `sdk-2.3.0` tag). It uses a
bundled **jimtcl** submodule — **forget to init it and configure/make fail** because
`jimtcl/libjim.a` is missing:

```bash
cd "$WORK"
git clone https://github.com/raspberrypi/openocd.git openocd-rpi
cd openocd-rpi
git submodule update --init          # <-- pulls jimtcl; do NOT skip
./bootstrap
./configure --disable-werror --enable-cmsis-dap --enable-cmsis-dap-v2
make -j"$(nproc)"
./src/openocd --version              # 0.12.0+dev-…, knows rp2350
cd "$WORK"
```

Use this `openocd-rpi/src/openocd` for everything below (with `-s openocd-rpi/tcl`).

---

## Step 5 — Build the Secure and Non-Secure images

```bash
cd "$WORK"
export TFM=$WORK/trusted-firmware-m
export TESTS=$WORK/tf-m-tests
export SDK=$WORK/pico-sdk

# 5a. Secure side (includes the provisioning bundle). The trailing `install`
#     matters -- it publishes the signing tools used by later steps.
cmake -S $TESTS/tests_reg/spe -B $TESTS/tests_reg/spe/build_rpi \
  -DTFM_PLATFORM=rpi/rp2350 \
  -DTFM_TOOLCHAIN_FILE=$TFM/toolchain_GNUARM.cmake \
  -DCONFIG_TFM_SOURCE_PATH=$TFM \
  -DTFM_PROFILE=profile_medium \
  -DPLATFORM_DEFAULT_PROVISIONING=OFF \
  -DTFM_DUMMY_PROVISIONING=ON \
  -DPICO_SDK_PATH=$SDK \
  -DTEST_S=ON -DTEST_NS=ON
cmake --build $TESTS/tests_reg/spe/build_rpi -- -j"$(nproc)" install

# 5b. Non-Secure side
cmake -S $TESTS/tests_reg -B $TESTS/tests_reg/build_rpi \
  -DCONFIG_SPE_PATH=$TESTS/tests_reg/spe/build_rpi/api_ns \
  -DTFM_TOOLCHAIN_FILE=$TESTS/tests_reg/spe/build_rpi/api_ns/cmake/toolchain_ns_GNUARM.cmake
cmake --build $TESTS/tests_reg/build_rpi -- -j"$(nproc)"
```

`-DTFM_DUMMY_PROVISIONING=ON` uses default test keys — fine for bring-up; swap for real keys later.

---

## Step 6 — Convert to `.uf2`

```bash
cd "$WORK"
cp uf2/utils/uf2conv.py uf2/utils/uf2families.json .
cp "$(find $TFM -name pico_uf2.sh)" .
chmod +x pico_uf2.sh uf2conv.py
./pico_uf2.sh $TESTS build_rpi
```

Produces (under `tf-m-tests/tests_reg/spe/build_rpi/bin/`): `provisioning_bundle.uf2`,
`tfm_s_ns_signed.uf2`, and `bl2.uf2`.

---

## Step 7 — Flash (BL2 goes last)

BOOTSEL flashing: hold the Pico 2's **BOOTSEL** button while plugging in USB → it mounts as a drive
→ drag one `.uf2`, wait for it to re-enumerate, drag the next.

**Pitfall #6: flash BL2 *last*.** BL2 verifies and jumps into the image behind it the moment it
boots. Flash it first and it finds no valid image → verify failure / boot loop, which looks like a
signing or isolation bug when it isn't. Lay down the booted image first, the bootloader last:

```
1. provisioning_bundle.uf2   (first boot only; dummy keys)
2. tfm_s_ns_signed.uf2        (Secure + Non-Secure signed image)
3. bl2.uf2                    (bootloader — LAST)
```

---

## Step 8 — Check the result over SWD (no UART)

The TF-M test framework leaves a table of `struct test_suite_t` in SRAM; after a run each suite's
result field (`val`) is `0` = `TEST_PASSED`. We halt over SWD, `reset init` (so SRAM keeps the last
run's contents), read the two arrays, and count.

**Physical/permission checks first (pitfall #7):**
- **udev:** install [`99-rpi-debugprobe.rules`](../99-rpi-debugprobe.rules) so a normal user can
  reach CMSIS-DAP and `ttyACM` without `sudo`:
  ```bash
  sudo cp 99-rpi-debugprobe.rules /etc/udev/rules.d/
  sudo udevadm control --reload-rules && sudo udevadm trigger
  # then unplug/replug the Debug Probe
  ```
- **Probe firmware:** the Debug Probe should run the `debugprobe` firmware (CMSIS-DAP v2). Expect
  `SWD DPIDR 0x4c013477` and `Cortex-M33 r1p0 processor detected` on connect.
- **Wiring:** SWCLK / SWDIO / GND must make solid contact. A wiggled jumper is the classic
  `Error connecting DP: cannot read IDR`.

**Read the verdict** with the helper script (edit the env vars at the top of the script, or export
them, to point at your OpenOCD build):

```bash
# from the repo root
./scripts/read_tfm_results.sh
# ...
# Non-Secure: 7/7 passed
# Secure    : 9/9 passed
# TOTAL     : 16/16 passed
# RESULT: ALL PASSED
```

> The full regression (RSA keygen / crypto / attestation suites are slow) takes **~90 s** to finish.
> Let the board `reset run` and settle before reading — reads taken too early show partial counts
> that climb as suites complete (e.g. 6 → 9 → 11 → 16).

### The layout, if you want to read the raw SRAM

`struct test_suite_t` is 5 words / 20 bytes (verified for `rpi/rp2350`, `profile_medium`,
tf-m-tests 2.x, all little-endian):

| offset | field | note |
|---|---|---|
| +0x00 | `freg` | test-registration fn ptr (flash `0x10xxxxxx`; also marks the array end) |
| +0x04 | `test_list` | SRAM ptr `0x20xxxxxx` |
| +0x08 | `list_size` | number of tests (non-zero = populated) |
| +0x0c | `name` | suite name string ptr (flash) |
| +0x10 | `val` | **0 = PASSED** ← the field judged |

Array bases: **Non-Secure `0x2004001c`** (7 suites), **Secure `0x20015008`** (9 suites) → 16 total.

---

## Pitfalls at a glance

| # | Symptom | Fix |
|---|---|---|
| 1 | `INVALID CONFIG … 13.2.1 20231009` | Use Arm's `arm-gnu-toolchain 13.3.rel1`, not apt |
| 2 | `mcuboot_imagesign_wrapper: not found` | `pip install -e trusted-firmware-m` (registers TF-M's tools) |
| 3 | libclang errors early in the build | `pip install libclang` + `apt install libclang-18-dev` |
| 4 | `check_version` / `git describe` aborts | `git tag TF-Mv2.3.0` in the TF-M tree |
| 5 | Distro OpenOCD has no `rp2350.cfg` | Build `raspberrypi/openocd` — `git submodule update --init` (jimtcl) first |
| 6 | Boot loop / verify fail after flashing | Flash order: provisioning → signed image → **BL2 last** |
| 7 | `cannot read IDR`, permission denied | udev rules + probe firmware + solid SWD wiring |

---

## Next: your own TrustZone code

Passing the regression means the platform works. From here,
[`examples/minimal-tz`](../examples/minimal-tz/) is a minimal, SDK-only Secure + Non-Secure example
(SAU + DMA-MPU + NSC veneers, no TF-M framework) — the clean starting point that
[pico-examples#708](https://github.com/raspberrypi/pico-examples/issues/708) asks for.
