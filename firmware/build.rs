use std::process::Command;

fn run(command: &mut Command, label: &str) {
    let status = command
        .status()
        .unwrap_or_else(|error| panic!("could not run {label}: {error}"));
    assert!(status.success(), "{label} failed with {status}");
}

fn main() {
    println!("cargo:rerun-if-changed=decrypt_shellcode/");
    println!("cargo:rerun-if-changed=scsi_shellcode/");

    run(
        Command::new("cargo")
            .arg("build")
            .arg("--locked")
            .arg("--release")
            .arg("--target")
            .arg("thumbv6m-none-eabi")
            .current_dir("./decrypt_shellcode/"),
        "decrypt shellcode Cargo build",
    );

    run(
        Command::new("arm-none-eabi-objcopy")
            .arg("-O")
            .arg("binary")
            .arg("target/thumbv6m-none-eabi/release/scsi_shellcode")
            .arg("scsi-stub.bin")
            .current_dir("./decrypt_shellcode/"),
        "decrypt shellcode objcopy",
    );

    run(
        Command::new("cargo")
            .arg("build")
            .arg("--locked")
            .arg("--release")
            .arg("--target")
            .arg("thumbv6m-none-eabi")
            .current_dir("./scsi_shellcode/"),
        "SCSI shellcode Cargo build",
    );

    run(
        Command::new("arm-none-eabi-objcopy")
            .arg("-O")
            .arg("binary")
            .arg("target/thumbv6m-none-eabi/release/scsi_shellcode")
            .arg("scsi-stub.bin")
            .current_dir("./scsi_shellcode/"),
        "SCSI shellcode objcopy",
    );
}
