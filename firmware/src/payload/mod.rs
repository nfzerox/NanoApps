pub mod bootstrap;
pub mod code_exec;
pub mod exploit_config;
pub mod payload_builder;

use crate::payload::exploit_config::ExploitConfig;
use crate::payload::payload_builder::CffPayloadBuilder;

pub trait Payload {
    fn build_cff<Cfg: ExploitConfig>(&self, builder: &mut CffPayloadBuilder);

    /// Optional bytes stored after endchar.
    ///
    /// The Nano 7 boot payload deliberately leaves this empty: cff_slot_load
    /// can release a callback-backed charstring frame before post-return code
    /// runs, so every persistent byte is written directly during decoding.
    fn trailing_data(&self) -> &[u8] {
        &[]
    }
}
