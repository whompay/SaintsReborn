//! The build toolchain: portable LLVM, CMake, Ninja, Git and the Microsoft C++
//! headers and libraries (fetched by xwin). Nothing is installed system-wide;
//! everything lives in one folder that every Saints Reborn install shares.

use crate::config::{self, Archive, Download};
use crate::fsx;
use crate::platform::{self, EXE};
use crate::report::{self, Env, Reporter, run};
use anyhow::{Result, bail};
use std::fs;
use std::path::{Path, PathBuf};

pub struct Toolchain {
    pub llvm_bin: PathBuf,
    pub cmake: PathBuf,
    pub git: PathBuf,
    pub winsysroot: PathBuf,
    /// CMake toolchain file for building Windows programs on Linux.
    pub cross_file: Option<PathBuf>,
    pub env: Env,
}

impl Toolchain {
    pub fn clang(&self) -> PathBuf {
        self.llvm_bin.join(format!("clang{EXE}"))
    }
    pub fn clangxx(&self) -> PathBuf {
        self.llvm_bin.join(format!("clang++{EXE}"))
    }
}

/// The toolchain lives in the install folder (build/toolchain), on the same
/// drive as the game: the old shared folder under %LOCALAPPDATA% filled up
/// small C: drives.
static ROOT: std::sync::Mutex<Option<PathBuf>> = std::sync::Mutex::new(None);
fn tools_root() -> PathBuf {
    ROOT.lock()
        .unwrap()
        .clone()
        .unwrap_or_else(|| platform::data_dir().join("tools"))
}
pub fn root_for(install_dir: &Path) -> PathBuf {
    install_dir.join("build").join("toolchain")
}

/// The files of LLVM the build uses (the rest is 2 GB of tools like clangd,
/// lldb and clang-tidy).
fn llvm_keep(rel: &Path) -> bool {
    const BIN: &[&str] = &[
        "clang",
        "clang++",
        "clang-cl",
        "clang-cpp",
        "clang-scan-deps",
        "lld",
        "lld-link",
        "ld.lld",
        "llvm-rc",
        "llvm-ar",
        "llvm-lib",
        "llvm-ranlib",
        "llvm-mt",
        "llvm-profdata",
        "llvm-nm",
        "llvm-objcopy",
        "llvm-strip",
        "llvm-dlltool",
        "llvm-readobj",
        "llvm-readelf",
        "llvm-objdump",
        "llvm-cvtres",
        "llvm-symbolizer",
    ];
    let s = rel.to_string_lossy().replace('\\', "/");
    if s == "bin" || s == "lib" || s == "lib/clang" || s.starts_with("lib/clang/") {
        return true;
    }
    if let Some(name) = s.strip_prefix("bin/") {
        let lower = name.to_ascii_lowercase();
        if lower.ends_with(".dll") {
            return !matches!(lower.as_str(), "liblldb.dll" | "libclang.dll" | "lldb.dll");
        }
        let base = name.strip_suffix(".exe").unwrap_or(name);
        // clang-19 is the real binary on Linux (clang / clang++ link to it).
        return BIN.contains(&base)
            || (base.starts_with("clang-") && base[6..].chars().all(|c| c.is_ascii_digit()));
    }
    // Linux: shared libraries next to the tools.
    if let Some(name) = s.strip_prefix("lib/") {
        return !name.contains('/') && name.contains(".so") && !name.contains("lldb");
    }
    false
}

/// Downloads and unpacks one tool unless it is already there.
fn ensure(r: &Reporter, d: &Download) -> Result<PathBuf> {
    let root = tools_root();
    let dir = root.join(d.dir);
    let marker = dir.join(".saintsreborn-complete");
    let key = format!("{}{}", d.sha256, if d.trim { " trim1" } else { "" });
    if fsx::read_text(&marker).as_deref() == Some(key.as_str()) {
        return Ok(dir);
    }
    let downloads = root.join("downloads");
    fs::create_dir_all(&downloads)?;
    let file = downloads.join(d.url.rsplit('/').next().unwrap_or("download"));
    r.status(format!(
        "Downloading {} ({} MB)",
        d.name,
        d.size.div_ceil(1 << 20)
    ));
    report::download(r, d.url, &file, Some(d.sha256))?;
    r.status(format!("Unpacking {}", d.name));
    fsx::remove_dir(&dir)?;
    match d.archive {
        Archive::Tar { strip } => {
            let keep: &dyn Fn(&Path) -> bool = if d.trim { &llvm_keep } else { &|_| true };
            fsx::untar(r, &file, &dir, strip, keep)?
        }
        Archive::Zip { strip } => fsx::unzip(&file, &dir, strip)?,
    }
    fsx::write_text(&marker, &key)?;
    let _ = fs::remove_file(&file);
    Ok(dir)
}

fn find_git(r: &Reporter) -> Result<PathBuf> {
    #[cfg(windows)]
    {
        let on_path = std::env::var_os("PATH")
            .map(|p| {
                std::env::split_paths(&p)
                    .map(|d| d.join("git.exe"))
                    .find(|p| p.is_file())
            })
            .unwrap_or(None);
        if let Some(g) = on_path {
            return Ok(g);
        }
        for base in ["ProgramW6432", "ProgramFiles"] {
            if let Some(b) = std::env::var_os(base) {
                let g = PathBuf::from(b).join("Git\\cmd\\git.exe");
                if g.is_file() {
                    return Ok(g);
                }
            }
        }
        let dir = ensure(r, &config::MINGIT)?;
        let g = dir.join("cmd\\git.exe");
        if !g.is_file() {
            bail!("Git could not be unpacked.");
        }
        Ok(g)
    }
    #[cfg(not(windows))]
    {
        let _ = r;
        platform::which("git").ok_or_else(|| {
            anyhow::anyhow!(
                "Git was not found. Install it with your package manager (for example \
                 'sudo apt install git' or 'sudo pacman -S git') and press Install again."
            )
        })
    }
}

/// Microsoft's C++ library and Windows SDK headers/libraries, laid out the
/// way clang's -Xmicrosoft-windows-sys-root expects.
fn ensure_winsysroot(r: &Reporter, xwin: &Path, accept_license: bool) -> Result<PathBuf> {
    let root = tools_root();
    let out = root.join(format!(
        "winsysroot-{}-{}",
        config::XWIN_CRT_VERSION,
        config::XWIN_SDK_VERSION
    ));
    let marker = out.join(".saintsreborn-complete");
    if marker.exists() {
        return Ok(out);
    }
    if !accept_license {
        bail!(
            "The Microsoft C++ headers and libraries come under Microsoft's Visual Studio license \
             ({}). Tick the box to accept it and press Install again.",
            config::MS_LICENSE_URL
        );
    }
    r.status("Downloading the Microsoft C++ headers and libraries (about 600 MB)");
    fsx::remove_dir(&out)?;
    let cache = root.join("xwin-cache");
    let attempt = |pinned: bool| -> Result<()> {
        let mut cmd = Env::default().command(xwin);
        cmd.args([
            "--accept-license",
            "--log-level",
            "warn",
            "--http-retry",
            "3",
        ])
        .arg("--cache-dir")
        .arg(&cache);
        if pinned {
            cmd.args([
                "--crt-version",
                config::XWIN_CRT_VERSION,
                "--sdk-version",
                config::XWIN_SDK_VERSION,
            ]);
        }
        cmd.args([
            "splat",
            "--use-winsysroot-style",
            "--preserve-ms-arch-notation",
        ]);
        if cfg!(windows) {
            cmd.arg("--disable-symlinks");
        }
        cmd.arg("--output").arg(&out);
        run(
            r,
            cmd,
            "Downloading the Microsoft C++ headers and libraries",
            |_| {},
        )
    };
    if let Err(e) = attempt(true) {
        r.check()?;
        r.log(format!(
            "Pinned versions not available ({e}); using the newest ones."
        ));
        fsx::remove_dir(&out)?;
        attempt(false)?;
    }
    fsx::remove_dir(&cache)?;
    if !out.join("VC").is_dir() {
        bail!("The Microsoft C++ headers and libraries were not unpacked.");
    }
    fsx::write_text(&marker, config::TOOLCHAIN_ID)?;
    Ok(out)
}

/// Every clang call that targets Windows reads these: where the Microsoft
/// headers/libraries are and that lld does the linking. On Windows clang.cfg
/// and clang++.cfg apply to every call; the triple-named files also cover
/// llvm-rc's preprocessor and the cross build on Linux.
/// Linux: Microsoft's headers and libraries assume a file system that ignores
/// case (the SDK includes <ObjBase.h>, the file is objbase.h). clang and
/// lld-link read this overlay, which lists every file and ignores case.
#[cfg(not(windows))]
fn write_case_overlay(winsysroot: &Path) -> Result<PathBuf> {
    fn esc(s: &str) -> String {
        s.replace('\\', "\\\\").replace('"', "\\\"")
    }
    fn walk(dir: &Path, out: &mut String) -> Result<()> {
        let mut first = true;
        for e in fs::read_dir(dir)? {
            let e = e?;
            let meta = fs::symlink_metadata(e.path())?;
            if meta.file_type().is_symlink() {
                continue; // xwin's case symlinks; the overlay replaces them
            }
            let name = esc(&e.file_name().to_string_lossy());
            if !first {
                out.push(',');
            }
            first = false;
            if meta.is_dir() {
                out.push_str(&format!(
                    "{{\"name\":\"{name}\",\"type\":\"directory\",\"contents\":["
                ));
                walk(&e.path(), out)?;
                out.push_str("]}");
            } else {
                let full = esc(&e.path().to_string_lossy());
                out.push_str(&format!(
                    "{{\"name\":\"{name}\",\"type\":\"file\",\"external-contents\":\"{full}\"}}"
                ));
            }
        }
        Ok(())
    }
    let root = esc(&winsysroot.to_string_lossy());
    let mut s = format!(
        "{{\"version\":0,\"case-sensitive\":\"false\",\"roots\":[{{\"name\":\"{root}\",\"type\":\"directory\",\"contents\":["
    );
    walk(winsysroot, &mut s)?;
    s.push_str("]}]}");
    let name = format!(
        "{}.vfs.json",
        winsysroot.file_name().unwrap_or_default().to_string_lossy()
    );
    let path = winsysroot.parent().unwrap_or(winsysroot).join(name);
    fsx::write_text(&path, &s)?;
    Ok(path)
}

// LLVM config files have their own quoting rules, separate from shell argv.
fn clang_config_quote(value: &str) -> String {
    format!("\"{}\"", value.replace('\\', "\\\\").replace('"', "\\\""))
}

fn write_clang_config(llvm_bin: &Path, winsysroot: &Path) -> Result<()> {
    let root = clang_config_quote(&winsysroot.to_string_lossy().replace('\\', "/"));
    #[cfg_attr(windows, allow(unused_mut))]
    let mut cfg = format!(
        "-Xmicrosoft-windows-sys-root {root}\n-fuse-ld=lld\n-Wno-unused-command-line-argument\n"
    );
    #[cfg(not(windows))]
    {
        let overlay = write_case_overlay(winsysroot)?;
        let o = clang_config_quote(&overlay.to_string_lossy());
        let link_overlay = clang_config_quote(&format!("/vfsoverlay:{}", overlay.display()));
        cfg += &format!("-ivfsoverlay {o}\n-Xlinker {link_overlay}\n");
    }
    let mut names = vec![
        "x86_64-pc-windows-msvc.cfg",
        "x86_64-pc-windows-msvc-clang.cfg",
        "x86_64-pc-windows-msvc-clang++.cfg",
    ];
    if cfg!(windows) {
        names.extend(["clang.cfg", "clang++.cfg"]);
    }
    for n in names {
        fsx::write_text(&llvm_bin.join(n), &cfg)?;
    }
    Ok(())
}

#[cfg(not(windows))]
fn write_cross_file(llvm_bin: &Path) -> Result<PathBuf> {
    let b = llvm_bin
        .to_string_lossy()
        .replace('"', "\\\"")
        .replace('$', "\\$");
    let text = format!(
        "# Builds Windows x64 programs with clang + lld + the xwin headers/libraries.\n\
         set(CMAKE_SYSTEM_NAME Windows)\n\
         set(CMAKE_SYSTEM_PROCESSOR AMD64)\n\
         set(CMAKE_C_COMPILER \"{b}/clang\")\n\
         set(CMAKE_CXX_COMPILER \"{b}/clang++\")\n\
         set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)\n\
         set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)\n\
         set(CMAKE_RC_COMPILER \"{b}/llvm-rc\")\n\
         set(CMAKE_AR \"{b}/llvm-ar\")\n\
         set(CMAKE_RANLIB \"{b}/llvm-ranlib\")\n\
         set(CMAKE_MT \"{b}/llvm-mt\")\n\
         set(CMAKE_LINKER_TYPE LLD)\n\
         # Only the release C runtime is downloaded (no debug libraries).\n\
         set(CMAKE_TRY_COMPILE_CONFIGURATION Release)\n"
    );
    let path = tools_root().join("windows-x64.cmake");
    fsx::write_text(&path, &text)?;
    Ok(path)
}

pub fn ensure_all(r: &Reporter, accept_license: bool, install_dir: &Path) -> Result<Toolchain> {
    let root = root_for(install_dir);
    fs::create_dir_all(&root)?;
    *ROOT.lock().unwrap() = Some(root.clone());
    r.log(format!("Build tools folder: {}", root.display()));
    let git = find_git(r)?;
    r.log(format!("Git: {}", git.display()));
    let mut dirs = Vec::new();
    for d in config::TOOLS {
        dirs.push(ensure(r, d)?);
    }
    let llvm = &dirs[0];
    let cmake_dir = &dirs[1];
    let ninja_dir = &dirs[2];
    let xwin_dir = &dirs[3];
    let llvm_bin = llvm.join("bin");
    let xwin = xwin_dir.join(format!("xwin{EXE}"));
    let winsysroot = ensure_winsysroot(r, &xwin, accept_license)?;
    write_clang_config(&llvm_bin, &winsysroot)?;

    #[cfg(not(windows))]
    let cross_file = Some(write_cross_file(&llvm_bin)?);
    #[cfg(windows)]
    let cross_file = None;

    let env = Env {
        path_prefix: vec![
            llvm_bin.clone(),
            cmake_dir.join("bin"),
            ninja_dir.clone(),
            git.parent().map(Path::to_path_buf).unwrap_or_default(),
        ],
        vars: vec![],
    };
    let tc = Toolchain {
        cmake: cmake_dir.join("bin").join(format!("cmake{EXE}")),
        llvm_bin,
        git,
        winsysroot,
        cross_file,
        env,
    };
    r.log(format!(
        "Microsoft headers and libraries: {}",
        tc.winsysroot.display()
    ));
    let v = report::capture(tc.env.command(tc.clang()).arg("--version")).unwrap_or_default();
    r.log(format!("Compiler: {}", v.lines().next().unwrap_or("?")));
    if !v.contains(config::LLVM_VERSION) {
        bail!(
            "The downloaded clang does not run ({}).",
            tc.clang().display()
        );
    }
    #[cfg(not(windows))]
    {
        r.status("Checking the LLVM linker runtime");
        let linker = tc.llvm_bin.join("lld-link");
        let version = report::capture(tc.env.command(&linker).arg("--version"));
        if version
            .as_deref()
            .is_none_or(|v| !v.contains(config::LLVM_VERSION))
        {
            bail!(
                "The LLVM linker cannot start ({}). Reopen the Linux Setup launcher to install its private runtime libraries, including libxml2.so.2.",
                linker.display()
            );
        }
    }
    // The old shared toolchain folder (%LOCALAPPDATA%\SaintsReborn\tools)
    // is no longer used: free its space.
    let old = platform::data_dir().join("tools");
    if old.exists() && old != root {
        match fsx::remove_dir(&old) {
            Ok(()) => r.log(format!(
                "Removed the old build tools folder {}",
                old.display()
            )),
            Err(e) => r.log(format!("Could not remove the old build tools folder: {e}")),
        }
    }
    Ok(tc)
}
