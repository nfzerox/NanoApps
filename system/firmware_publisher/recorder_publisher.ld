ENTRY(cff_deferred_install)

SECTIONS
{
    . = 0;
    .text :
    {
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
           "publisher entry must be offset zero")
    ASSERT(cff_deferred_install_end <= 0x60,
           "publisher entry overlaps install lock")
    ASSERT(cff_install_lock == 0x60,
           "atomic install lock moved from offset 0x60")
    ASSERT(cff_install_descriptor == 0x70,
           "descriptor moved from offset 0x70")
    ASSERT(scsi_stub == 0x168,
           "C6/98 handler moved from offset 0x168")
    ASSERT(scsi_stub_end - scsi_stub == 140,
           "C6/98 handler size changed")
    ASSERT(0x600 + cff_deferred_image_end <= 0x7f4,
           "combined NanoApps image exceeds verified cave")
}
