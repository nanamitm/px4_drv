fn main() {
    println!("cargo:rerun-if-changed=frontend.rc");
    println!("cargo:rerun-if-changed=../../src/common/version.h");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        let file =
            std::path::PathBuf::from(std::env::var_os("OUT_DIR").unwrap()).join("frontend.res");
        let status = std::process::Command::new("rc.exe")
            .arg("/nologo")
            .arg("/fo")
            .arg(&file)
            .arg("frontend.rc")
            .status()
            .expect("Run the build in a Visual Studio developer environment with rc.exe");
        assert!(status.success(), "Frontend resource compilation failed");
        println!("cargo:rustc-link-arg={}", file.display());
    }
}
