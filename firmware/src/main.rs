#![allow(unused_macros)]
#![allow(dead_code)]
mod cff;
mod exploit;
mod img1;
mod mse;
mod payload;

use crate::exploit::create_cff;
use crate::payload::bootstrap::BootstrapPayload;
use crate::payload::code_exec::InitialCodeExecPayload;
use crate::payload::exploit_config::{ExploitConfigN6G, ExploitConfigN7G};
use anyhow::anyhow;
use clap::{Parser, ValueEnum};
use isahc::ReadResponseExt;
use std::fs::File;
use std::io::{BufWriter, Seek, Write};
use std::io::{Cursor, Read};
use std::path::{Path, PathBuf};
use std::process::Command;
use tracing::{info, Level};
use walkdir::WalkDir;
use zip::write::FileOptions;
use zip::{DateTime, ZipArchive};

// Searches for `pattern` in `data` and replaces the last byte of the found sequence with `new_last_byte`.
// Returns `Some(offset)` where offset is the position of the replaced byte (index within data), or `None` if not found.
fn patch_pattern_in_vec(data: &mut [u8], pattern: &[u8], new_last_byte: u8) -> Option<usize> {
    if pattern.is_empty() || data.len() < pattern.len() {
        return None;
    }
    if let Some(pos) = data.windows(pattern.len()).position(|w| w == pattern) {
        let target_index = pos + pattern.len() - 1;
        data[target_index] = new_last_byte;
        Some(target_index)
    } else {
        None
    }
}

#[derive(Debug, ValueEnum, Copy, Clone)]
pub enum Device {
    #[value(skip)]
    Nano6,
    Nano7Refresh,
}

/// Simple program to greet a person
#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
struct Args {
    /// Which device to build a payload for
    #[arg(short, long)]
    device: Device,


    /// Install a fixed Nano 7 deferred stage from verified OS-image padding.
    /// The legacy --bootstrap spelling remains as an alias.
    #[arg(long, visible_alias = "bootstrap", value_name = "BIN")]
    deferred_stage: Option<PathBuf>,

    /// Cave publisher blob (defaults to cff_deferred_install.bin beside stage).
    #[arg(long, value_name = "BIN", requires = "deferred_stage")]
    deferred_installer: Option<PathBuf>,

    /// Write the generated raw CFF and stop before changing an MSE or IPSW.
    #[arg(long, value_name = "PATH")]
    cff_output: Option<PathBuf>,
}

fn zip_dir<T>(src_dir: &str, target: T) -> zip::result::ZipResult<()>
where
    T: Write + Seek,
{
    #![allow(deprecated)]
    let mut zip_writer = zip::ZipWriter::new(target);
    let options = FileOptions::default()
        .compression_method(zip::CompressionMethod::Stored)
        // zip 0.6 otherwise stamps every entry with wall-clock time. A fixed
        // DOS epoch makes identical reviewed firmware inputs byte-identical.
        .last_modified_time(
            DateTime::from_date_and_time(1980, 1, 1, 0, 0, 0)
                .expect("the DOS epoch is a valid ZIP timestamp"),
        )
        .unix_permissions(0o755);

    let walkdir = WalkDir::new(src_dir).sort_by_file_name();
    let mut buffer = Vec::new();

    for entry in walkdir.into_iter().filter_map(|e| e.ok()) {
        let path = entry.path();
        let name = path.strip_prefix(Path::new(src_dir)).unwrap();

        if path.is_file() {
            println!("Adding file {:?} as {:?}", path, name);
            zip_writer.start_file_from_path(name, options)?;
            let mut f = File::open(path)?;
            f.read_to_end(&mut buffer)?;
            zip_writer.write_all(&buffer)?;
            buffer.clear();
        } else if !name.as_os_str().is_empty() {
            println!("Adding directory {:?}", name);
            zip_writer.add_directory_from_path(name, options)?;
        }
    }
    zip_writer.finish()?;
    Ok(())
}

fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_max_level(Level::DEBUG)
        .init();

    let args = Args::parse();
    let python = std::env::var_os("PYTHON").unwrap_or_else(|| "python3".into());

    // Generate exploit font
    info!("Building CFF exploit");
    let bytes = match (&args.device, &args.deferred_stage) {
        (Device::Nano6, Some(_)) => {
            return Err(anyhow!(
                "the fixed deferred stage is only supported for Nano 7 / 39A10023"
            ));
        }
        (Device::Nano6, None) => {
            create_cff::<ExploitConfigN6G, _>(&InitialCodeExecPayload::default())?
        }
        (Device::Nano7Refresh, Some(path)) => {
            let installer = args
                .deferred_installer
                .clone()
                .unwrap_or_else(|| path.with_file_name("cff_deferred_install.bin"));
            let payload = BootstrapPayload::new(std::fs::read(path)?, std::fs::read(installer)?)?;
            create_cff::<ExploitConfigN7G, _>(&payload)?
        }
        (Device::Nano7Refresh, None) => {
            create_cff::<ExploitConfigN7G, _>(&InitialCodeExecPayload::default())?
        }
    };

    if let Some(path) = &args.cff_output {
        std::fs::write(path, bytes)?;
        info!(
            "Wrote raw exploit CFF to {:?}; firmware was not modified",
            path
        );
        return Ok(());
    }

    std::fs::write("./in-cff.bin", bytes)?;

    info!("Converting font to OTF");
    let status = Command::new(&python)
        .arg("./helpers/cff_to_otf.py")
        .status()?;
    if !status.success() {
        return Err(anyhow!("CFF-to-OTF conversion failed with {status}"));
    }

    std::fs::remove_file("./in-cff.bin")?;
    let otf_bytes = std::fs::read("./out-otf.bin")?;
    std::fs::remove_file("./out-otf.bin")?;

    info!("Unpacking MSE");
    let mut mse = if let Device::Nano6 = args.device {
        if !Path::new("./Firmware-36B10147.MSE").try_exists()? {
            let mut ipsw = isahc::get("http://appldnld.apple.com/iPod/SBML/osx/bundles/041-1920.20111004.CpeEw/iPod_1.2_36B10147.ipsw")?;
            let mut zip = ZipArchive::new(Cursor::new(ipsw.bytes().unwrap()))?;
            let mut mse = zip.by_name("Firmware.MSE")?;
            let mut out = Vec::new();
            mse.read_to_end(&mut out)?;

            // For 36B10147, we don't make a patch (because probably only n7g have logic flaws in rsrc parsing) - just save it
            std::fs::write("./Firmware-36B10147.MSE", &out)?;
        }

        mse::unpack("./Firmware-36B10147.MSE", &args.device)
    } else {
        if !Path::new("./Firmware-39A10023.MSE").try_exists()? {
            let mut ipsw = isahc::get("http://appldnld.apple.com/ipod/sbml/osx/bundles/031-59796-20160525-8E6A5D46-21FF-11E6-89D1-C5D3662719FC/iPod_1.1.2_39A10023.ipsw")?;
            let mut zip = ZipArchive::new(Cursor::new(ipsw.bytes().unwrap()))?;
            let mut mse = zip.by_name("Firmware.MSE")?;
            let mut out = Vec::new();
            mse.read_to_end(&mut out)?;

            // Patch: Looking for signature 38 37 34 30 32 2E 30 04 (ASCII "87402.0" + 0x04) (it is the only one in the entire file)
            let pattern: &[u8] = b"87402.0\x04";
            if let Some(idx) = patch_pattern_in_vec(&mut out, pattern, 0x03) {
                println!("Patched Firmware-39A10023.MSE at offset 0x{:X}", idx);
            } else {
                println!("Pattern not found in Firmware-39A10023.MSE!");
            }

            // We save the already modified binary
            std::fs::write("./Firmware-39A10023.MSE", &out)?;
        }

        mse::unpack("./Firmware-39A10023.MSE", &args.device)
    };

    let rsrc = mse
        .sections
        .iter_mut()
        .find(|s| &s.name == b"crsr")
        .ok_or(anyhow!("Failed to find rsrc section in MSE"))?;
    {
        info!("Unpacking RSRC Img1");
        let mut img1 = img1::img1_parse(&rsrc.body, &args.device);
        {
            info!("Patching FATFS");
            std::fs::write("rsrc.bin", &img1.body)?;
            std::fs::write("in-otf.bin", otf_bytes)?;

            let status = Command::new(&python)
                .arg("./helpers/fat_patch.py")
                .status()?;
            if !status.success() {
                return Err(anyhow!("resource FAT patch failed with {status}"));
            }

            let rsrc_data = std::fs::read("./rsrc.bin")?;
            std::fs::remove_file("./rsrc.bin")?;
            std::fs::remove_file("./in-otf.bin")?;
            img1.body = rsrc_data;
        }
        info!("Repacking RSRC Img1");
        rsrc.body.clear();
        img1.write(&mut rsrc.body);
    }

    info!("Repacking MSE");
    let mut mse_out = Vec::new();
    mse.write(&mut mse_out);
    let output_tag = if args.deferred_stage.is_some() {
        "nanoapps-deferred"
    } else {
        "repack"
    };

    // Disk swap
    if let Device::Nano6 = args.device {
        info!("Doing disk swap");
        mse_out[0x5004..][..4].copy_from_slice(b"soso");
        mse_out[0x5144..][..4].copy_from_slice(b"ksid");
        std::fs::write("./iPod_1.2_36B10147/Firmware.MSE", &mse_out)?;
        let src_dir = "./iPod_1.2_36B10147";
        let zip_file_path = format!("./iPod_1.2_36B10147_{output_tag}.ipsw");

        let file = File::create(zip_file_path)?;
        let writer = BufWriter::new(file);

        match zip_dir(src_dir, writer) {
            Ok(_) => println!("Successfully zipped the directory!"),
            Err(e) => println!("Error zipping the directory: {:?}", e),
        }
    } else {
        std::fs::write("./iPod_1.1.2_39A10023_2012/Firmware.MSE", &mse_out)?;
        let src_dir = "./iPod_1.1.2_39A10023_2012";
        let zip_file_path = format!("./iPod_1.1.2_39A10023_2012_{output_tag}.ipsw");

        let file = File::create(zip_file_path)?;
        let writer = BufWriter::new(file);

        match zip_dir(src_dir, writer) {
            Ok(_) => println!("Successfully zipped the directory!"),
            Err(e) => println!("Error zipping the directory: {:?}", e),
        }

        /* The nano7-refresh release targets the 2012 device. A clean build has
         * only that IPSW template; emit the 2015 package only when its distinct
         * manifest/template was explicitly supplied. */
        let src_dir15 = "./iPod_1.1.2_39A10023_2015";
        if Path::new(src_dir15).is_dir() {
            std::fs::write("./iPod_1.1.2_39A10023_2015/Firmware.MSE", &mse_out)?;
            let zip_file_path15 = format!("./iPod_1.1.2_39A10023_2015_{output_tag}.ipsw");
            let file15 = File::create(zip_file_path15)?;
            let writer15 = BufWriter::new(file15);

            match zip_dir(src_dir15, writer15) {
                Ok(_) => println!("Successfully zipped the directory!"),
                Err(e) => println!("Error zipping the directory: {:?}", e),
            }
        }
    }

    Ok(())
}
