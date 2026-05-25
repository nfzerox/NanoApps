/*
 * Shared linker script for homebrew apps.
 *
 * Apps link with LINK_VA pinned to the upload+exec target address.
 * Default 0x0867f8e4 (the 4 KB freespace at end of OS .text — RX safe,
 * battle-tested from 1.0.4 IRQ-trampoline work).
 *
 * Override via `make LINK_VA=0xADDR` if a future scratch space appears.
 *
 * payload_entry MUST be the first function in .text — placed via
 * .text.entry section attribute (see HB_APP_ENTRY macro in hb_sdk.h).
 */

ENTRY(payload_entry)

/* Provide a default for LINK_VA if not passed via --defsym.
   PROVIDE() is the linker-script-side default; the build system
   supplies LINK_VA via --defsym=LINK_VA=0x... per Makefile.
   DEFINED() does NOT see --defsym symbols, so we use PROVIDE
   to assign a default that --defsym overrides. */
PROVIDE(LINK_VA = 0x0867f8e4);

SECTIONS {
    . = LINK_VA;
    .text : {
        *(.text.entry)
        *(.text*)
        *(.rodata*)
        *(.data*)
        *(.bss*)
        *(COMMON)
    }
    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.note*)
        *(.comment)
    }
}
