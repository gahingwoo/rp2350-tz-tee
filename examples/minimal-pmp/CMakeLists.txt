# SPDX-License-Identifier: BSD-3-Clause
#
# minimal-pmp -- RP2350 Hazard3 (RISC-V) PMP isolation example.
#
# Build for the RISC-V (Hazard3) cores. Requires a RISC-V toolchain that knows
# the Hazard3 extensions (rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb); the
# Raspberry Pi "riscv-toolchain-15" (riscv32-unknown-elf-gcc) is verified.
#
#   export WORK=~/rp2350-tz
#   export PICO_SDK_PATH=$WORK/pico-sdk
#   export PICO_TOOLCHAIN_PATH=$WORK/riscv-toolchain    # the RISC-V GCC
#   cmake -S examples/minimal-pmp -B examples/minimal-pmp/build -G Ninja
#   cmake --build examples/minimal-pmp/build

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-riscv CACHE STRING "Pico platform (Hazard3 RISC-V)")

if (DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(minimal_pmp C CXX ASM)

pico_sdk_init()

add_executable(minimal_pmp
    main.c
)
target_link_libraries(minimal_pmp pico_stdlib)

pico_add_extra_outputs(minimal_pmp)
