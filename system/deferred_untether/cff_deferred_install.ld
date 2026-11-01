ENTRY(cff_deferred_install)

SECTIONS
{
    . = 0;
    .text :
    {
        KEEP(*(.text.entry))
        *(.text*)
        *(.rodata*)
    }
    /DISCARD/ :
    {
        *(.comment)
        *(.ARM.attributes)
        *(.debug*)
    }

    ASSERT(cff_deferred_install == 0,
           "CFF cave publisher entry must be at offset zero")
    ASSERT(cff_deferred_install_end <= 0x60,
           "CFF cave publisher code overlaps captured state")
    ASSERT(cff_saved_stack152 == 0x60,
           "CFF cave publisher capture layout moved")
    ASSERT(cff_install_descriptor == 0x70,
           "CFF cave publisher descriptor layout moved")
    ASSERT(cff_deferred_image_end <= CFF_INSTALL_LIMIT,
           "CFF cave publisher exceeds its reserved image window")
}
