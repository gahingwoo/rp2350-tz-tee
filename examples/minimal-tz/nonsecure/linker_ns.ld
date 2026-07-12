/* SPDX-License-Identifier: BSD-3-Clause
 *
 * minimal-tz -- Non-Secure image linker script (bare metal, RAM-resident).
 *
 * The Non-Secure side is freestanding (no pico-sdk, no libc), so this is a
 * plain minimal script: vector table at the base of the Non-Secure RAM window,
 * then code / rodata / data / bss, with the stack at the top of NS RAM.
 *
 * The image runs from Non-Secure SRAM because on RP2350 (bare boot) a
 * Non-Secure read of XIP flash returns 0. The Secure world copies this whole
 * image (VMA == LMA) into NS_RAM_START before the BLXNS handoff, so .data is
 * already in place; ns_reset() only has to zero .bss.
 *
 * NS RAM: 0x20040000 .. 0x2007FFFF (256 KB). vector[0] = __ns_stack_top,
 * vector[1] = ns_reset -- the two words the Secure side reads to launch it.
 */

__ns_stack_size__ = 0x1000;   /* 4 KB is plenty for this tiny NS */

MEMORY
{
    RAM (rwx) : ORIGIN = 0x20040000, LENGTH = 0x00040000
}

ENTRY(ns_reset)

SECTIONS
{
    .vectors ORIGIN(RAM) : {
        KEEP(*(.vectors))
    } > RAM

    .text : {
        *(.text*)
        *(.glue_7) *(.glue_7t)
        . = ALIGN(4);
    } > RAM

    .rodata : {
        *(.rodata*)
        . = ALIGN(4);
    } > RAM

    .data : {
        . = ALIGN(4);
        __data_start__ = .;
        *(.data*)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    /* Stack at the very top of NS RAM (grows down). vector[0] loads this. */
    __ns_stack_top = ORIGIN(RAM) + LENGTH(RAM);
    __ns_stack_bottom = __ns_stack_top - __ns_stack_size__;

    /* Guard: image + stack must fit, and must stay below the mailbox at
     * 0x2007C000 so the two never overlap. */
    ASSERT(__bss_end__ < 0x2007C000, "NS image grew into the mailbox region")

    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
    }
}
