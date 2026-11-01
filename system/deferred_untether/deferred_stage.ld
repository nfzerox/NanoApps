ENTRY(deferred_action)

SECTIONS
{
    . = LINK_VA;
    .text :
    {
        *(.text*)
        *(.rodata*)
    }
    .match_helper LINK_VA + 0x5a0 :
    {
        *(.match_helper)
    }
    /DISCARD/ :
    {
        *(.comment)
        *(.ARM.attributes)
        *(.debug*)
    }
    ASSERT(deferred_primary_end - LINK_VA <= STAGE_LIMIT,
           "deferred primary stage overlaps the reporter page")
    ASSERT(action_match_helper == LINK_VA + 0x5a0,
           "action matcher moved from the verified post-report gap")
    ASSERT(deferred_stage_end - action_match_helper <= 0x60,
           "action matcher exceeds the 96-byte post-report gap")
    ASSERT(deferred_stage_end - LINK_VA <= 0x600,
           "deferred image overlaps the cave publisher")
}
