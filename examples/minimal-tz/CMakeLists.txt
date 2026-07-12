# SPDX-License-Identifier: BSD-3-Clause
#
# minimal-tz -- two-image RP2350 TrustZone-M build (pico-sdk based).
#
#   minimal_tz_s   Secure image @ 0x10000000. Compiled with -mcmse so GCC emits
#                  CMSE secure-gateway veneers for the cmse_nonsecure_entry
#                  functions in secure_service.c. Its link step also writes a
#                  CMSE *import library* (--out-implib) that exports just the
#                  veneer entry points.
#   minimal_tz_ns  Non-Secure image @ 0x10100000. Links against that import
#                  library instead of the Secure objects, so it can call the
#                  veneers but sees none of the Secure implementation.
#
# Build:
#   source ~/rp2350-tz/env.sh          # puts arm-none-eabi-gcc on PATH, sets SDK
#   cmake -S examples/minimal-tz -B examples/minimal-tz/build \
#         -G Ninja -DPICO_SDK_PATH=$SDK
#   cmake --build examples/minimal-tz/build
#
# Outputs (per image): .elf / .bin / .uf2 under build/.
#
# Heads-up: building a Non-Secure pico-sdk image is not officially supported by
# pico-sdk 2.1.1 yet (this example is exactly the pico-examples#708 gap). The
# Secure isolation primitives are the proven part; treat the NS handoff as
# bring-up.

cmake_minimum_required(VERSION 3.13)

# Target the Pico 2 (RP2350) in Arm Secure mode. Both images build for the
# Secure Arm platform (pico-sdk 2.1.1 has no separate NS platform); the NS image
# simply omits -mcmse and uses the NS linker script.
set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform")

# Pull in the SDK. Honours PICO_SDK_PATH (cache var or environment).
if (DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(minimal_tz C CXX ASM)

pico_sdk_init()

# --------------------------------------------------------------------------
# Secure image
# --------------------------------------------------------------------------
add_executable(minimal_tz_s
    secure/main_s.c
    secure/tz_config.c
    secure/secure_service.c
    secure/secret.c
)
target_include_directories(minimal_tz_s PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/secure
)
# -mcmse: enable the CMSE (Security Extension) intrinsics + veneer generation.
target_compile_options(minimal_tz_s PRIVATE -mcmse)
target_link_libraries(minimal_tz_s pico_stdlib hardware_dma cmsis_core)

pico_set_linker_script(minimal_tz_s ${CMAKE_CURRENT_LIST_DIR}/secure/linker_s.ld)

# Emit the CMSE import library the Non-Secure image links against.
set(MINIMAL_TZ_VENEER_LIB ${CMAKE_CURRENT_BINARY_DIR}/minimal_tz_s_cmse.o)
target_link_options(minimal_tz_s PRIVATE
    -mcmse
    -Wl,--cmse-implib
    -Wl,--out-implib,${MINIMAL_TZ_VENEER_LIB}
)
# The import library is a side effect of the Secure link, so tell CMake it is
# produced by building minimal_tz_s. This lets the Non-Secure target depend on
# it as a real file, so a single `cmake --build` orders the two links correctly.
add_custom_command(
    OUTPUT ${MINIMAL_TZ_VENEER_LIB}
    DEPENDS minimal_tz_s
    COMMAND ${CMAKE_COMMAND} -E true
    COMMENT "CMSE import library (produced by minimal_tz_s link)"
)

pico_enable_stdio_uart(minimal_tz_s 1)
pico_enable_stdio_usb(minimal_tz_s 0)
pico_add_extra_outputs(minimal_tz_s)

# --------------------------------------------------------------------------
# Non-Secure image -- FREESTANDING (no pico-sdk, no libc).
#
# On RP2350 (bare boot) a Non-Secure image cannot run pico-sdk's runtime: NS
# reads of XIP flash return 0, and the NS runtime hangs re-initialising hardware
# the Secure world already owns. So the NS side is a tiny bare-metal image: own
# vector table + reset, no crt0, no stdlib. It runs from Non-Secure SRAM (the
# Secure world copies it there before BLXNS -- see main_s.c).
#
# We build it outside the pico-sdk target helpers so none of the SDK runtime is
# pulled in. It links only main_ns.c + the CMSE import library.
# --------------------------------------------------------------------------
set(NS_ARCH_FLAGS -mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mfloat-abi=softfp)

add_executable(minimal_tz_ns
    nonsecure/main_ns.c
)
target_include_directories(minimal_tz_ns PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)
target_compile_options(minimal_tz_ns PRIVATE
    ${NS_ARCH_FLAGS} -Os -ffreestanding -fno-builtin
    -ffunction-sections -fdata-sections -Wall -Wextra
)
# Freestanding link: our own linker script, no startup files, no standard libs.
target_link_options(minimal_tz_ns PRIVATE
    ${NS_ARCH_FLAGS}
    -nostartfiles -nostdlib -Wl,--gc-sections
    -Wl,-T,${CMAKE_CURRENT_LIST_DIR}/nonsecure/linker_ns.ld
)
set_target_properties(minimal_tz_ns PROPERTIES LINK_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/nonsecure/linker_ns.ld)

# The CMSE import library (produced by the minimal_tz_s link, via the custom
# command above) resolves secure_get_counter()/secure_sign() to the Secure SG
# veneers. Added as a generated external object so the build graph orders the
# two links and NS builds after S.
set_source_files_properties(${MINIMAL_TZ_VENEER_LIB} PROPERTIES
    GENERATED TRUE EXTERNAL_OBJECT TRUE)
target_sources(minimal_tz_ns PRIVATE ${MINIMAL_TZ_VENEER_LIB})
add_dependencies(minimal_tz_ns minimal_tz_s)

# Emit the raw RAM image the Secure world copies into NS SRAM.
add_custom_command(TARGET minimal_tz_ns POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:minimal_tz_ns> ${CMAKE_CURRENT_BINARY_DIR}/minimal_tz_ns.bin
    COMMENT "Creating minimal_tz_ns.bin (Non-Secure RAM image)"
)
