//! Install / update: get the source, the game files, build the ReXGlue SDK,
//! recompile default.xex and build the game, then put everything in dist.
//! Every step is skipped when its inputs have not changed, so pressing
//! Install again after a failure (or an update) only redoes what is needed.

use crate::config;
use crate::fsx;
use crate::online;
use crate::platform::{self, EXE};
use crate::report::{Reporter, capture, ninja_progress, run};
use crate::tools::{self, Toolchain};
use anyhow::{Context, Result, bail};
use sha1::{Digest, Sha1};
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug, PartialEq)]
pub enum GameSource {
    /// Game files already in dist/game (or the update case).
    None,
    Iso(PathBuf),
    /// A folder with default.xex and the packfiles in it.
    Folder(PathBuf),
}

#[derive(Clone, Debug)]
pub struct Options {
    pub dir: PathBuf,
    pub game: GameSource,
    pub update: bool,
    pub desktop_shortcut: bool,
    /// Start menu / application menu entries and remembering the folder.
    pub integrate: bool,
    pub accept_license: bool,
    /// Rebuild even when the source is unchanged.
    pub force: bool,
    /// Optional Epic Online Services folder (developers only).
    pub eos: Option<PathBuf>,
}

pub fn is_install_folder(d: &Path) -> bool {
    d.join("scripts").join("setup.ps1").is_file()
        || d.join("setup.bat").is_file()
        || d.join("project").join("CMakeLists.txt").is_file()
}

fn has_other_files(d: &Path) -> bool {
    fs::read_dir(d)
        .map(|rd| {
            rd.flatten().any(|e| {
                let n = e.file_name().to_string_lossy().to_lowercase();
                n != "setup-installer.log" && n != ".download" && n != ".saintsreborn-setup"
            })
        })
        .unwrap_or(false)
}

fn sha1_file(p: &Path) -> Result<String> {
    let bytes = fs::read(p).with_context(|| format!("could not read {}", p.display()))?;
    Ok(crate::report::hex(&Sha1::digest(&bytes)).to_uppercase())
}

struct Stamps(PathBuf);
impl Stamps {
    fn done(&self, name: &str, key: &str) -> bool {
        fsx::read_text(&self.0.join(name)).as_deref() == Some(key)
    }
    fn mark(&self, name: &str, key: &str) -> Result<()> {
        fsx::write_text(&self.0.join(name), key)
    }
    fn clear(&self, name: &str) {
        let _ = fs::remove_file(self.0.join(name));
    }
}

/// Checks done before anything starts; returns a message for the user.
pub fn validate(o: &Options) -> Result<()> {
    let dir = &o.dir;
    if !dir.is_absolute() {
        bail!(
            "Choose a full install folder path, for example {}.",
            platform::default_install_dir().display()
        );
    }
    if o.update && !is_install_folder(dir) {
        bail!(
            "That folder is not a Saints Reborn folder. Choose the folder that contains the dist folder."
        );
    }
    if !o.update && dir.exists() && !is_install_folder(dir) && has_other_files(dir) {
        bail!("The install folder already contains other files. Choose an empty or new folder.");
    }
    let game_there = dir.join("dist").join("game").join("default.xex").is_file();
    match &o.game {
        GameSource::Iso(p) if !p.is_file() => {
            bail!("The disc image was not found: {}", p.display())
        }
        GameSource::Folder(p) if !p.join("default.xex").is_file() => bail!(
            "That game folder has no default.xex in it. Choose the folder that contains default.xex \
             and the packfiles folder."
        ),
        GameSource::None if !game_there => {
            bail!(
                "Choose your Saints Row disc image (.iso) or the folder with your game files first."
            )
        }
        _ => {}
    }
    Ok(())
}

/// Returns a summary line for the end.
pub fn run_all(r: &Reporter, o: &Options) -> Result<String> {
    let dir = o.dir.clone();
    fs::create_dir_all(&dir).with_context(|| format!("could not create {}", dir.display()))?;
    r.open_log(&dir.join("setup-installer.log"));
    r.log(format!(
        "Saints Reborn Setup {} - install folder {}",
        config::APP_VERSION,
        dir.display()
    ));
    if let Some(free) = platform::free_gb(&dir) {
        r.log(format!("Free space on the install drive: {free} GB"));
        if free < config::MIN_FREE_GB {
            bail!(
                "Not enough free space on the drive of {}: {free} GB free. Setup needs about {} GB \
                 (build tools and the build). Free some space, or choose a folder on another drive.",
                dir.display(),
                config::NEEDED_FREE_GB
            );
        }
        if free < config::NEEDED_FREE_GB {
            r.log(format!(
                "Warning: only {free} GB free; a first build needs about {} GB.",
                config::NEEDED_FREE_GB
            ));
        }
    }
    #[cfg(target_os = "linux")]
    crate::linux::preflight(&dir)?;
    r.progress(None);

    let tc = tools::ensure_all(r, o.accept_license, &dir)?;
    let downloads = tools::root_for(&dir).join("downloads");
    fs::create_dir_all(&downloads)?;
    #[cfg(windows)]
    platform::ensure_vc_runtime(r, &downloads)?;

    let updated = get_source(r, &tc, &dir)?;
    let dist = dir.join("dist");
    let build = dir.join("build");
    let stamps = Stamps(build.join("stamps"));
    let revision = capture(
        tc.env
            .command(&tc.git)
            .arg("-C")
            .arg(&dir)
            .args(["rev-parse", "HEAD"]),
    )
    .context("Could not identify the downloaded source revision")?;
    let install_key = format!("{} {}", config::TOOLCHAIN_ID, revision.trim());
    if !updated && installed(&stamps, &dist, &install_key) && !o.force && o.game == GameSource::None
    {
        r.status("Checking the online play files");
        let commit = sdk_commit(&dir);
        if let Err(e) = online::install(r, &dir, &dist, &commit, &downloads) {
            r.log(format!("Online play: could not be checked ({e})."));
        }
        finish(r, o, &dir);
        return Ok("Saints Reborn is already up to date.".into());
    }
    stamps.clear("installed");
    build_game(r, &tc, o, &dir, &downloads)?;
    stamps.mark("installed", &install_key)?;
    finish(r, o, &dir);
    Ok(if o.update {
        "Saints Reborn is updated."
    } else {
        "Saints Reborn is installed."
    }
    .into())
}

// A toolchain stamp is written before compilation and cannot prove success.
fn installed(stamps: &Stamps, dist: &Path, key: &str) -> bool {
    stamps.done("installed", key)
        && dist.join("saintsrow.exe").is_file()
        && dist.join("WhompaysModLoader.exe").is_file()
}

#[cfg(test)]
mod completion_tests {
    use super::*;

    #[test]
    fn failed_build_with_old_executables_is_not_an_installed_revision() {
        let root = std::env::temp_dir().join(format!("sr-completion-{}", std::process::id()));
        fs::create_dir_all(root.join("stamps")).unwrap();
        let stamps = Stamps(root.join("stamps"));
        fs::write(root.join("saintsrow.exe"), b"old build").unwrap();
        fs::write(root.join("WhompaysModLoader.exe"), b"old build").unwrap();
        stamps.mark("toolchain", config::TOOLCHAIN_ID).unwrap();
        assert!(!installed(&stamps, &root, "current revision"));
        stamps.mark("installed", "current revision").unwrap();
        assert!(installed(&stamps, &root, "current revision"));
        assert!(!installed(&stamps, &root, "next revision"));
        stamps.clear("installed");
        assert!(!installed(&stamps, &root, "current revision"));
        fs::remove_dir_all(root).unwrap();
    }
}

/// For CI: the toolchain, the SDK and the recompiler only (no game files
/// needed). `root` is a Saints Reborn source folder.
pub fn sdk_only(r: &Reporter, root: &Path, accept_license: bool) -> Result<String> {
    let tc = tools::ensure_all(r, accept_license, root)?;
    let build = root.join("build");
    let stamps = Stamps(build.join("stamps"));
    fs::create_dir_all(&stamps.0)?;
    let jobs = jobs().to_string();
    let (sdk_src, sdk_install) = (build.join("rexglue-sdk"), build.join("sdk"));
    build_sdk(r, &tc, root, &stamps, &sdk_src, &sdk_install, &jobs, None)?;
    let rexglue = host_rexglue(r, &tc, &sdk_src, &sdk_install, &build, &jobs)?;
    let mut c = tc.env.command(&rexglue);
    c.arg("--help");
    let _ = crate::report::run_code(r, &mut c, &mut |_: &str| {});
    for dll in ["rexruntime.dll", "rexgpu-xenos.dll"] {
        fsx::require(&sdk_install.join("bin").join(dll), dll)?;
    }
    Ok(format!("SDK and recompiler built: {}", rexglue.display()))
}

fn finish(r: &Reporter, o: &Options, dir: &Path) {
    if !o.integrate {
        r.progress(Some(1.0));
        return;
    }
    r.status("Creating shortcuts");
    let setup_copy = dir.join(format!("SaintsReborn-Setup{EXE}"));
    let copied = match std::env::current_exe() {
        Ok(me) if me != setup_copy => fs::copy(&me, &setup_copy).is_ok(),
        Ok(_) => true,
        Err(_) => false,
    };
    let _ = fs::remove_file(dir.join("SaintsRowPC-Setup.exe"));
    platform::create_shortcuts(
        r,
        dir,
        o.desktop_shortcut,
        copied.then_some(setup_copy.as_path()),
    );
    platform::remember_install_dir(dir);
    r.progress(Some(1.0));
}

/// Returns true when the source changed (new install or new patches).
fn get_source(r: &Reporter, tc: &Toolchain, dir: &Path) -> Result<bool> {
    let git = |args: &[&str]| {
        let mut c = tc.env.command(&tc.git);
        c.arg("-C").arg(dir).args(args);
        c
    };
    if is_install_folder(dir) {
        r.status("Downloading the latest Saints Reborn patches");
        let is_repo = dir.join(".git").is_dir();
        if is_repo {
            clear_git_locks(r, dir);
        }
        let before = if is_repo {
            capture(&mut git(&["rev-parse", "HEAD"])).unwrap_or_default()
        } else {
            String::new()
        };
        if !is_repo {
            run(
                r,
                git(&["init", "-q"]),
                "Preparing the folder for updates (git init)",
                |_| {},
            )?;
        }
        run(
            r,
            git(&["fetch", "--depth", "1", config::REPO_URL, config::BRANCH]),
            "Downloading the update",
            |_| {},
        )
        .context("Check your internet connection.")?;
        let _ = capture(&mut git(&["config", "core.autocrlf", "false"]));
        let after = capture(&mut git(&["rev-parse", "FETCH_HEAD"])).unwrap_or_default();
        if is_repo && !after.is_empty() && after == before {
            r.log(format!(
                "Source code is already the latest version ({}).",
                short(&after)
            ));
            return Ok(false);
        }
        run(
            r,
            git(&["checkout", "-f", "-B", config::BRANCH, "FETCH_HEAD"]),
            "Applying the update",
            |_| {},
        )?;
        let _ = capture(&mut git(&["remote", "remove", "origin"]));
        let _ = capture(&mut git(&["remote", "add", "origin", config::REPO_URL]));
        r.log(format!(
            "Updated {} -> {}",
            if before.is_empty() {
                "download".into()
            } else {
                short(&before)
            },
            short(&after)
        ));
        return Ok(true);
    }
    r.status("Downloading the Saints Reborn source code");
    let tmp = dir.join(".download");
    fsx::remove_dir(&tmp)?;
    let mut c = tc.env.command(&tc.git);
    c.args([
        "clone",
        "-c",
        "core.autocrlf=false",
        "--depth",
        "1",
        "--branch",
        config::BRANCH,
        config::REPO_URL,
    ])
    .arg(&tmp);
    run(r, c, "Downloading the source code", |_| {}).context("Check your internet connection.")?;
    for e in fs::read_dir(&tmp)? {
        let e = e?;
        fs::rename(e.path(), dir.join(e.file_name()))?;
    }
    fsx::remove_dir(&tmp)?;
    Ok(true)
}

fn short(sha: &str) -> String {
    sha.chars().take(7).collect()
}

/// The SDK commit the downloaded source wants (config/sdk_commit.txt, or the
/// line in scripts/setup.ps1).
pub fn sdk_commit(root: &Path) -> String {
    if let Some(c) = fsx::read_text(&root.join("config").join("sdk_commit.txt"))
        && c.len() == 40
    {
        return c;
    }
    if let Ok(s) = fs::read_to_string(root.join("scripts").join("setup.ps1")) {
        for l in s.lines() {
            if let Some(rest) = l.trim().strip_prefix("$SdkCommit = \"")
                && let Some(c) = rest.split('"').next()
                && c.len() == 40
            {
                return c.to_string();
            }
        }
    }
    config::SDK_COMMIT_FALLBACK.to_string()
}

fn jobs() -> u64 {
    let cpus = std::thread::available_parallelism()
        .map(|n| n.get() as u64)
        .unwrap_or(4);
    // The recompiled files are huge: at most one compile per ~2 GB of RAM.
    cpus.min(platform::ram_gb() / 2).max(1)
}

fn build_game(
    r: &Reporter,
    tc: &Toolchain,
    o: &Options,
    root: &Path,
    downloads: &Path,
) -> Result<()> {
    let build = root.join("build");
    let dist = root.join("dist");
    let game_dir = dist.join("game");
    let sdk_src = build.join("rexglue-sdk");
    let sdk_install = build.join("sdk");
    let game_build = build.join("game");
    let generated = build.join("generated");
    fs::create_dir_all(&build)?;
    fs::create_dir_all(&dist)?;
    let stamps = Stamps(build.join("stamps"));
    fs::create_dir_all(&stamps.0)?;
    let jobs = jobs().to_string();
    let cpu = platform::cpu_level();
    r.log(format!(
        "Build: {jobs} parallel jobs, CPU level {cpu}, {} GB RAM",
        platform::ram_gb()
    ));

    // A different compiler: start the build folders over.
    if !stamps.done("toolchain", config::TOOLCHAIN_ID) {
        r.status("Preparing the build folders for the new build tools");
        fsx::remove_dir(&game_build)?;
        fsx::remove_dir(&sdk_src.join("out"))?;
        fsx::remove_dir(&sdk_install)?;
        fsx::remove_dir(&build.join("host"))?;
        stamps.clear("sdk");
        stamps.clear("codegen");
        stamps.mark("toolchain", config::TOOLCHAIN_ID)?;
    }

    // ---- game files
    get_game_files(r, tc, o, root, &game_dir)?;
    let xex = sha1_file(&game_dir.join("default.xex"))?;
    if xex != config::KNOWN_XEX_SHA1 {
        r.log(format!(
            "WARNING: your default.xex (SHA-1 {xex}) is not the version this port was made for. The build may \
             fail or the game may not run correctly."
        ));
    }

    // ---- ReXGlue SDK
    let commit = sdk_commit(root);
    let pgo = root.join("pgo");
    let game_pgo = pgo.join("saintsrow.profdata");
    let eos = o
        .eos
        .clone()
        .filter(|e| e.join("SDK").join("Include").join("eos_sdk.h").is_file());
    let sdk_key = build_sdk(
        r,
        tc,
        root,
        &stamps,
        &sdk_src,
        &sdk_install,
        &jobs,
        eos.as_deref(),
    )?;

    // ---- recompile default.xex
    let manifest = root.join("config").join("saintsrow_manifest.toml");
    let codegen_key = format!("{sdk_key} {}", sha1_file(&manifest)?);
    if !stamps.done("codegen", &codegen_key) {
        let rexglue = host_rexglue(r, tc, &sdk_src, &sdk_install, &build, &jobs)?;
        r.status("Recompiling default.xex (PowerPC -> C++)");
        let mut c = tc.env.command(&rexglue);
        c.arg("codegen")
            .arg(&manifest)
            .arg("--ignore-stamp")
            .current_dir(root);
        run(r, c, "Recompiling default.xex", |_| {})?;
        stamps.mark("codegen", &codegen_key)?;
    } else {
        r.log("Recompiled code already generated - skipping.");
    }

    // ---- the game
    r.status("Building Saints Reborn (compiles ~34,000 functions; 10-60 minutes)");
    let mut c = tc.env.command(&tc.cmake);
    c.arg("-S")
        .arg(root.join("project"))
        .arg("-B")
        .arg(&game_build)
        .args(["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]);
    c.args(common_cmake_args(tc));
    c.arg(format!("-DSR_GENERATED_DIR={}", cm(&generated)));
    c.arg(format!("-DSR_CPU_LEVEL={cpu}"));
    if game_pgo.exists() {
        c.arg("-DSR_PGO=use")
            .arg(format!("-DSR_PGO_PROFILE={}", cm(&game_pgo)));
    }
    if let Some(e) = &eos {
        c.arg(format!("-DSR_EOS={}", cm(e)));
    }
    c.env("REXSDK", &sdk_install);
    run(r, c, "Configuring the game build", |_| {})?;
    let mut c = tc.env.command(&tc.cmake);
    c.arg("--build").arg(&game_build).args(["-j", &jobs]);
    c.env("REXSDK", &sdk_install);
    run(r, c, "Building the game", |l| progress_line(r, l))?;

    // ---- dist
    r.status("Copying the game to dist");
    copy_to_dist(r, root, &game_build, &sdk_install, &dist, eos.as_deref())
        .context("Could not copy the game to dist. If Saints Reborn is running, close it and press Install again")?;

    r.status("Making the keyboard/mouse button pictures");
    if let Err(e) = make_glyphs(r, tc, root, &game_build, &game_dir, &dist) {
        r.log(format!(
            "Warning: the keyboard/mouse button pictures could not be made ({e}); the game will show controller buttons."
        ));
    }

    if eos.is_none() {
        r.status("Online play (Epic)");
        let _ = fs::remove_file(dist.join("online_pack.txt"));
        match online::install(r, root, &dist, &commit, downloads) {
            Ok(_) => {}
            Err(e) => r.log(format!(
                "Online play: the online pack could not be installed ({e}). System Link on a LAN and co-op by IP \
                 still work; starting the game from the mod loader tries again."
            )),
        }
    }
    if !dist.join("saintsrow.exe").exists() {
        bail!("The build finished but dist/saintsrow.exe is missing.");
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn build_sdk(
    r: &Reporter,
    tc: &Toolchain,
    root: &Path,
    stamps: &Stamps,
    sdk_src: &Path,
    sdk_install: &Path,
    jobs: &str,
    eos: Option<&Path>,
) -> Result<String> {
    let commit = sdk_commit(root);
    let patch = root.join("patches").join("rexglue-sdk.patch");
    let gpu_pgo = root.join("pgo").join("rexgpu.profdata");
    let mut sdk_key = format!("{commit} {} {}", sha1_file(&patch)?, config::TOOLCHAIN_ID);
    if gpu_pgo.exists() {
        sdk_key += &format!(" {}", sha1_file(&gpu_pgo)?);
    }
    if let Some(e) = eos {
        sdk_key += &format!(" eos={}", e.display());
    }
    if stamps.done("sdk", &sdk_key) {
        r.log("ReXGlue SDK already built - skipping.");
        return Ok(sdk_key);
    }
    prepare_sdk_source(r, tc, sdk_src, &commit, &patch)?;
    r.status("Building the ReXGlue SDK (about 5-20 minutes)");
    let bdir = sdk_src.join("out").join("build").join("saintsreborn");
    let mut c = tc.env.command(&tc.cmake);
    c.arg("-S")
        .arg(sdk_src)
        .arg("-B")
        .arg(&bdir)
        .args(["-G", "Ninja Multi-Config"]);
    c.args(common_cmake_args(tc));
    c.args([
        "-DCMAKE_C_FLAGS=-march=x86-64-v2",
        "-DCMAKE_CXX_FLAGS=-march=x86-64-v2",
        "-DCMAKE_CXX_STANDARD=23",
        "-DCMAKE_CONFIGURATION_TYPES=Debug;Release;RelWithDebInfo",
        "-DCMAKE_DEFAULT_BUILD_TYPE=Release",
    ]);
    c.arg(format!("-DCMAKE_INSTALL_PREFIX={}", cm(sdk_install)));
    if gpu_pgo.exists() {
        c.arg("-DREX_GPU_PGO=use")
            .arg(format!("-DREX_GPU_PGO_PROFILE={}", cm(&gpu_pgo)));
    }
    if let Some(e) = eos {
        c.arg(format!(
            "-DREX_EOS_SDK_INCLUDE={}",
            cm(&e.join("SDK").join("Include"))
        ));
    }
    run(r, c, "Configuring the ReXGlue SDK", |_| {})?;
    let mut c = tc.env.command(&tc.cmake);
    c.arg("--build")
        .arg(&bdir)
        .args(["--target", "install", "--config", "Release", "-j", jobs]);
    run(r, c, "Building the ReXGlue SDK", |l| progress_line(r, l))?;
    stamps.mark("sdk", &sdk_key)?;
    stamps.clear("codegen");
    Ok(sdk_key)
}

fn progress_line(r: &Reporter, line: &str) {
    if let Some((a, b)) = ninja_progress(line)
        && b > 0
    {
        r.progress(Some(a as f32 / b as f32));
    }
}

/// CMake path: forward slashes.
fn cm(p: &Path) -> String {
    p.to_string_lossy().replace('\\', "/")
}

/// Compiler settings every Windows-target configure gets.
fn common_cmake_args(tc: &Toolchain) -> Vec<String> {
    let mut v = Vec::new();
    match &tc.cross_file {
        Some(f) => v.push(format!("-DCMAKE_TOOLCHAIN_FILE={}", cm(f))),
        None => {
            v.push(format!("-DCMAKE_C_COMPILER={}", cm(&tc.clang())));
            v.push(format!("-DCMAKE_CXX_COMPILER={}", cm(&tc.clangxx())));
            v.push(format!(
                "-DCMAKE_RC_COMPILER={}",
                cm(&tc.llvm_bin.join(format!("llvm-rc{EXE}")))
            ));
            v.push("-DCMAKE_LINKER_TYPE=LLD".into());
        }
    }
    v.push(format!("-DCMAKE_MAKE_PROGRAM={}", cm(&ninja_path(tc))));
    // Only the release C runtime is downloaded: CMake's compiler checks must not
    // link a Debug program (msvcrtd.lib). Without Visual Studio on the PC the
    // first configure failed with "could not open 'msvcrtd.lib'".
    v.push("-DCMAKE_TRY_COMPILE_CONFIGURATION=Release".into());
    v
}

fn ninja_path(tc: &Toolchain) -> PathBuf {
    tc.env.path_prefix[2].join(format!("ninja{EXE}"))
}

fn prepare_sdk_source(
    r: &Reporter,
    tc: &Toolchain,
    sdk_src: &Path,
    commit: &str,
    patch: &Path,
) -> Result<()> {
    let git = |args: &[&str]| {
        let mut c = tc.env.command(&tc.git);
        c.arg("-C").arg(sdk_src).args(args);
        c
    };
    if !sdk_src.join(".git").is_dir() {
        r.status("Downloading the ReXGlue SDK");
        fsx::remove_dir(sdk_src)?;
        let mut c = tc.env.command(&tc.git);
        c.args(["clone", "-c", "core.autocrlf=false", config::SDK_REPO])
            .arg(sdk_src);
        run(r, c, "Downloading the ReXGlue SDK", |_| {})
            .context("Check your internet connection.")?;
    }
    clear_git_locks(r, sdk_src);
    r.status(format!(
        "Checking out ReXGlue SDK {} and its dependencies",
        short(commit)
    ));
    if run(
        r,
        git(&[
            "-c",
            "advice.detachedHead=false",
            "checkout",
            "--force",
            commit,
        ]),
        "git checkout",
        |_| {},
    )
    .is_err()
    {
        run(
            r,
            git(&["fetch", "origin"]),
            "Downloading the ReXGlue SDK update",
            |_| {},
        )?;
        run(
            r,
            git(&[
                "-c",
                "advice.detachedHead=false",
                "checkout",
                "--force",
                commit,
            ]),
            "git checkout",
            |_| {},
        )?;
    }
    // Files an older patch added (checkout --force leaves them).
    let _ = capture(&mut git(&[
        "clean",
        "-fdq",
        "--",
        "include",
        "src",
        "resources",
        "cmake",
    ]));
    if run(
        r,
        git(&[
            "submodule",
            "update",
            "--init",
            "--recursive",
            "--force",
            "--depth",
            "1",
        ]),
        "git submodule",
        |_| {},
    )
    .is_err()
    {
        run(
            r,
            git(&["submodule", "update", "--init", "--recursive", "--force"]),
            "Downloading the SDK dependencies",
            |_| {},
        )?;
    }
    #[cfg(windows)]
    fix_mspack_links(sdk_src);

    r.status("Applying the Saints Row patch to the SDK");
    let patch_s = patch.to_string_lossy().into_owned();
    if capture(&mut git(&["apply", "--check", &patch_s])).is_some() {
        run(
            r,
            git(&["apply", "--whitespace=nowarn", &patch_s]),
            "Applying the SDK patch",
            |_| {},
        )?;
    } else if capture(&mut git(&["apply", "--reverse", "--check", &patch_s])).is_some() {
        r.log("Patch already applied.");
    } else {
        bail!("The SDK patch does not apply. Delete the build folder and press Install again.");
    }
    Ok(())
}

/// Clears locks a killed or crashed git run left in `repo` (see
/// fsx::remove_git_locks) and says so in the log.
fn clear_git_locks(r: &Reporter, repo: &Path) {
    for p in fsx::remove_git_locks(repo) {
        r.log(format!("Removed a leftover git lock: {}", p.display()));
    }
}

/// Git on Windows checks libmspack's symlinked sources out as small text
/// files containing the link target; replace them with the real files.
#[cfg(windows)]
fn fix_mspack_links(sdk_src: &Path) {
    let dir = sdk_src
        .join("thirdparty")
        .join("libmspack")
        .join("cabextract")
        .join("mspack");
    let Ok(rd) = fs::read_dir(&dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if e.metadata()
            .map(|m| m.is_file() && m.len() < 300)
            .unwrap_or(false)
            && let Ok(t) = fs::read_to_string(&p)
        {
            let t = t.trim();
            if t.starts_with("../") && !t.contains('\n') {
                let target = dir.join(t);
                if target.is_file() {
                    let _ = fs::copy(&target, &p);
                }
            }
        }
    }
}

/// The recompiler has to run on this PC: on Windows the SDK build installs
/// it; on Linux a native copy is built once.
fn host_rexglue(
    r: &Reporter,
    tc: &Toolchain,
    sdk_src: &Path,
    sdk_install: &Path,
    build: &Path,
    jobs: &str,
) -> Result<PathBuf> {
    if cfg!(windows) {
        let p = sdk_install.join("bin").join("rexglue.exe");
        fsx::require(&p, "The recompiler (rexglue.exe)")?;
        return Ok(p);
    }
    #[cfg(unix)]
    let source = prepare_host_source(r, &tc.env, &tc.git, sdk_src, build)?;
    #[cfg(not(unix))]
    let source = sdk_src.to_path_buf();
    let host = build.join("host").join("sdk");
    r.status("Building the recompiler for this PC");
    let mut c = tc.env.command(&tc.cmake);
    // An exported SDK tree must not inherit the parent game's Git tags.
    if let Some(parent) = source.parent() {
        c.env("GIT_CEILING_DIRECTORIES", parent);
    }
    c.arg("-S").arg(&source).arg("-B").arg(&host).args([
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
    ]);
    c.arg(format!("-DCMAKE_C_COMPILER={}", cm(&tc.clang())));
    c.arg(format!("-DCMAKE_CXX_COMPILER={}", cm(&tc.clangxx())));
    c.args([
        "-DCMAKE_C_FLAGS=-march=x86-64-v2",
        "-DCMAKE_CXX_FLAGS=-march=x86-64-v2",
        "-DCMAKE_CXX_STANDARD=23",
    ]);
    let native = native_flags()?;
    if !native.is_empty() {
        c.arg(format!("-DCMAKE_C_FLAGS=-march=x86-64-v2 {native}"));
        c.arg(format!("-DCMAKE_CXX_FLAGS=-march=x86-64-v2 {native}"));
    }
    c.arg("-DCMAKE_LINKER_TYPE=LLD");
    c.arg(format!("-DCMAKE_MAKE_PROGRAM={}", cm(&ninja_path(tc))));
    // The SDK configures its UI targets even when only rexglue is built.
    // Supply the private X11/Wayland metadata and avoid Conda's shell wrapper,
    // which expects CC even though Env intentionally removes compiler flags.
    if let Some(prefix) = std::env::var_os("SR_NATIVE_PREFIX") {
        let prefix = PathBuf::from(prefix);
        // Conda's iconv.h maps iconv_* to libiconv_*. Link the matching ABI
        // explicitly, and reject unresolved native shared-library symbols.
        c.args([
            "-DCMAKE_C_STANDARD_LIBRARIES=-Wl,--no-as-needed -liconv -Wl,--as-needed",
            "-DCMAKE_CXX_STANDARD_LIBRARIES=-Wl,--no-as-needed -liconv -Wl,--as-needed",
            "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--no-undefined",
        ]);
        c.arg(format!(
            "-DPKG_CONFIG_EXECUTABLE={}",
            cm(&prefix.join("bin/pkg-config.bin"))
        ));
        c.env(
            "PKG_CONFIG_PATH",
            std::env::join_paths([prefix.join("lib/pkgconfig"), prefix.join("share/pkgconfig")])?,
        );
    }
    // Keep SDL itself console-only; the SDK still needs UI headers above.
    c.args([
        "-DSDL_UNIX_CONSOLE_BUILD=ON",
        "-DSDL_X11=OFF",
        "-DSDL_WAYLAND=OFF",
        "-DSDL_KMSDRM=OFF",
        "-DSDL_OPENGL=OFF",
        "-DSDL_OPENGLES=OFF",
        "-DSDL_ALSA=OFF",
        "-DSDL_JACK=OFF",
        "-DSDL_PIPEWIRE=OFF",
        "-DSDL_PULSEAUDIO=OFF",
        "-DSDL_SNDIO=OFF",
        "-DSDL_DBUS=OFF",
        "-DSDL_IBUS=OFF",
        "-DSDL_HIDAPI_LIBUSB=OFF",
    ]);
    run(r, c, "Configuring the recompiler", |_| {})?;
    let mut c = tc.env.command(&tc.cmake);
    if let Some(parent) = source.parent() {
        c.env("GIT_CEILING_DIRECTORIES", parent);
    }
    c.arg("--build")
        .arg(&host)
        .args(["--target", "rexglue", "-j", jobs]);
    run(r, c, "Building the recompiler", |l| progress_line(r, l))
        .context("The native recompiler build failed; see the compiler or linker error above")?;
    let rexglue = find_file(&source.join("out"), "rexglue")
        .context("The recompiler was built but not found")?;
    r.status("Checking the native recompiler runtime symbols");
    let mut check = tc.env.command(&rexglue);
    check.arg("--version").env("LD_BIND_NOW", "1");
    run(r, check, "Checking the native recompiler runtime", |_| {})?;
    Ok(rexglue)
}

/// The game patch includes Windows-only networking and identity code. The
/// native recompiler must use pristine SDK runtime sources at the same commit,
/// plus the game's code-generation template so generated output still matches.
#[cfg(unix)]
fn prepare_host_source(
    r: &Reporter,
    env: &crate::report::Env,
    git: &Path,
    sdk_src: &Path,
    build: &Path,
) -> Result<PathBuf> {
    let root = build.join("host");
    let source = root.join("source");
    let stamp = root.join("source.stamp");
    let revision = capture(
        env.command(git)
            .arg("-C")
            .arg(sdk_src)
            .args(["rev-parse", "HEAD"]),
    )
    .context("Could not identify the SDK source revision")?;
    let template = Path::new("resources/templates/codegen/pch_h.inja");
    let key = format!(
        "host-source-v1 {} {}",
        revision.trim(),
        sha1_file(&sdk_src.join(template))?
    );
    if fsx::read_text(&stamp).as_deref() == Some(key.as_str())
        && source.join("CMakeLists.txt").is_file()
    {
        return Ok(source);
    }
    r.status("Preparing the native recompiler source (without Windows game runtime patches)");
    fs::create_dir_all(&root)?;
    // A CMake cache cannot be reused after changing its source directory.
    fsx::remove_dir(&root.join("sdk"))?;
    fsx::remove_dir(&source)?;
    let archive = root.join("source.tar");
    let mut cmd = env.command(git);
    cmd.arg("-C")
        .arg(sdk_src)
        .args(["archive", "--format=tar"])
        .arg("--output")
        .arg(&archive)
        .arg(revision.trim());
    run(r, cmd, "Exporting the native SDK source", |_| {})?;
    fsx::untar(r, &archive, &source, false, &|_| true)?;
    fs::remove_file(archive)?;
    let modules = capture(env.command(git).arg("-C").arg(sdk_src).args([
        "config",
        "--file",
        ".gitmodules",
        "--get-regexp",
        "^submodule.*[.]path$",
    ]))
    .context("Could not read the SDK dependency paths")?;
    for line in modules.lines() {
        let (_, path) = line
            .split_once(char::is_whitespace)
            .context("Invalid SDK dependency path")?;
        let relative = Path::new(path.trim());
        if !relative.starts_with("thirdparty")
            || relative
                .components()
                .any(|c| !matches!(c, std::path::Component::Normal(_)))
        {
            bail!("Invalid SDK dependency path: {}", relative.display());
        }
        let target = sdk_src.join(relative);
        if !target.is_dir() {
            bail!("SDK dependency missing: {}", target.display());
        }
        let link = source.join(relative);
        fsx::remove_dir(&link)?;
        fs::create_dir_all(link.parent().unwrap())?;
        std::os::unix::fs::symlink(&target, &link)?;
    }
    fs::copy(sdk_src.join(template), source.join(template))?;
    fsx::write_text(&stamp, &key)?;
    Ok(source)
}

#[cfg(all(test, unix))]
mod host_source_tests {
    use super::*;

    #[test]
    fn native_snapshot_excludes_windows_runtime_patches_but_keeps_codegen_template() {
        let root = std::env::temp_dir().join(format!("sr-host-source-{}", std::process::id()));
        let sdk = root.join("sdk");
        let template = Path::new("resources/templates/codegen/pch_h.inja");
        for dir in [
            "src/system",
            "resources/templates/codegen",
            "thirdparty/fixture",
        ] {
            fs::create_dir_all(sdk.join(dir)).unwrap();
        }
        fs::write(sdk.join("CMakeLists.txt"), "native SDK").unwrap();
        fs::write(sdk.join("src/system/xsocket.cpp"), "native sockets").unwrap();
        fs::write(sdk.join(template), "original template").unwrap();
        fs::write(sdk.join("thirdparty/fixture/data"), "dependency").unwrap();
        fs::write(sdk.join(".gitmodules"),
            "[submodule \"fixture\"]\npath = thirdparty/fixture\nurl = https://example.invalid/fixture\n").unwrap();
        let env = crate::report::Env::default();
        let git = Path::new("git");
        for args in [
            vec!["init", "-q"],
            vec!["add", "."],
            vec![
                "-c",
                "user.name=Test",
                "-c",
                "user.email=test@example.invalid",
                "commit",
                "-qm",
                "base",
            ],
        ] {
            assert!(
                env.command(git)
                    .arg("-C")
                    .arg(&sdk)
                    .args(args)
                    .status()
                    .unwrap()
                    .success()
            );
        }
        fs::write(sdk.join("src/system/xsocket.cpp"), "Windows socket changes").unwrap();
        fs::write(
            sdk.join("src/system/online_identity.cpp"),
            "#include <windows.h>",
        )
        .unwrap();
        fs::write(sdk.join(template), "game-specific generated code").unwrap();
        let r = Reporter::new(
            None,
            std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false)),
            false,
        );
        let build = root.join("build");
        let source = prepare_host_source(&r, &env, git, &sdk, &build).unwrap();
        assert_eq!(
            fs::read_to_string(source.join("src/system/xsocket.cpp")).unwrap(),
            "native sockets"
        );
        assert!(!source.join("src/system/online_identity.cpp").exists());
        assert_eq!(
            fs::read_to_string(source.join(template)).unwrap(),
            "game-specific generated code"
        );
        assert_eq!(
            fs::read_to_string(source.join("thirdparty/fixture/data")).unwrap(),
            "dependency"
        );
        let cache = build.join("host/sdk/keep-cache");
        fs::create_dir_all(cache.parent().unwrap()).unwrap();
        fs::write(&cache, "compiled objects").unwrap();
        prepare_host_source(&r, &env, git, &sdk, &build).unwrap();
        assert!(cache.exists());
        fs::write(sdk.join(template), "new game template").unwrap();
        prepare_host_source(&r, &env, git, &sdk, &build).unwrap();
        assert!(!cache.exists());
        assert_eq!(
            fs::read_to_string(source.join(template)).unwrap(),
            "new game template"
        );
        assert_eq!(
            fs::read_to_string(sdk.join("thirdparty/fixture/data")).unwrap(),
            "dependency"
        );
        fs::remove_dir_all(root).unwrap();
    }
}

// The plug-and-play launcher supplies a private native compiler sysroot.
// Keep these options out of Windows cross-builds.
fn native_args() -> Result<Vec<String>> {
    let Some(prefix) = std::env::var_os("SR_NATIVE_PREFIX") else {
        return Ok(Vec::new());
    };
    let prefix = PathBuf::from(prefix);
    let sysroot = prefix.join("x86_64-conda-linux-gnu/sysroot");
    let gcc = prefix.join("lib/gcc/x86_64-conda-linux-gnu");
    let mut versions: Vec<PathBuf> = fs::read_dir(&gcc)?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.join("crtbegin.o").is_file())
        .collect();
    versions.sort();
    let gcc = versions
        .last()
        .context("Private GCC installation is incomplete")?;
    Ok(vec![
        format!("--sysroot={}", sysroot.display()),
        format!("--gcc-install-dir={}", gcc.display()),
        format!("-L{}", prefix.join("lib").display()),
        format!("-Wl,-rpath,{}", prefix.join("lib").display()),
    ])
}

fn native_flags() -> Result<String> {
    Ok(native_args()?.join(" "))
}

const LINUX_DEPS: &str =
    "A native helper could not be compiled; see the compiler or linker error above";

fn find_file(dir: &Path, name: &str) -> Option<PathBuf> {
    for e in fs::read_dir(dir).ok()?.flatten() {
        let p = e.path();
        if p.is_dir() {
            if let Some(f) = find_file(&p, name) {
                return Some(f);
            }
        } else if p.file_name().map(|n| n == name).unwrap_or(false) {
            return Some(p);
        }
    }
    None
}

/// Compiles a small helper program for this PC (not for the game).
fn host_tool(
    r: &Reporter,
    tc: &Toolchain,
    out: &Path,
    sources: &[PathBuf],
    c_sources: &[PathBuf],
    includes: &[PathBuf],
) -> Result<()> {
    fs::create_dir_all(out.parent().unwrap())?;
    let objdir = out.with_extension("obj.d");
    fs::create_dir_all(&objdir)?;
    let mut objs = Vec::new();
    for (i, s) in c_sources.iter().enumerate() {
        let o = objdir.join(format!("c{i}.o"));
        let mut c = tc.env.command(tc.clang());
        c.args(native_args()?);
        c.args([
            "-O2",
            "-c",
            "-D_CRT_SECURE_NO_WARNINGS",
            "-D_CRT_NONSTDC_NO_DEPRECATE",
        ]);
        for inc in includes {
            c.arg("-I").arg(inc);
        }
        c.arg(s).arg("-o").arg(&o);
        run(r, c, "Compiling a helper", |_| {})?;
        objs.push(o);
    }
    let mut c = tc.env.command(tc.clangxx());
    c.args(native_args()?);
    c.args([
        "-std=c++20",
        "-O2",
        "-fuse-ld=lld",
        "-D_CRT_SECURE_NO_WARNINGS",
    ]);
    for inc in includes {
        c.arg("-I").arg(inc);
    }
    c.args(sources).args(&objs).arg("-o").arg(out);
    let res = run(r, c, "Compiling a helper", |_| {});
    if cfg!(windows) {
        res
    } else {
        res.context(LINUX_DEPS)
    }
}

/// The players' saves live inside dist/game: `545107D1/profile/<name>` (the
/// multiplayer character with its cash, profile settings) and the content
/// folders named by a 16-digit profile id (single-player saves). Replacing the
/// game files (a disc image or game folder chosen during an update) used to
/// delete them with the rest of dist/game. They are moved aside first and put
/// back afterwards, also when copying or extracting fails.
struct KeptSaves {
    game_dir: PathBuf,
    keep: PathBuf,
    names: Vec<std::ffi::OsString>,
}

impl KeptSaves {
    fn take(r: &Reporter, game_dir: &Path) -> Result<Self> {
        let keep = game_dir.with_file_name("game_saves_kept");
        let mut names = Vec::new();
        if let Ok(rd) = fs::read_dir(game_dir) {
            for e in rd.flatten() {
                let name = e.file_name();
                let n = name.to_string_lossy();
                let save_dir =
                    (n.len() == 8 || n.len() == 16) && n.chars().all(|c| c.is_ascii_hexdigit());
                if !save_dir || !e.path().is_dir() {
                    continue;
                }
                fs::create_dir_all(&keep)?;
                let to = keep.join(&name);
                if to.exists() {
                    // Left over from an earlier run that stopped: that copy is the player's.
                    fsx::remove_dir(&e.path())?;
                } else {
                    fs::rename(e.path(), &to).with_context(|| {
                        format!("could not keep your saves ({})", e.path().display())
                    })?;
                }
                names.push(name);
            }
        }
        if let Ok(rd) = fs::read_dir(&keep) {
            for e in rd.flatten() {
                if !names.contains(&e.file_name()) {
                    names.push(e.file_name());
                }
            }
        }
        if !names.is_empty() {
            r.log("Keeping your saves (online character and money, profile, story saves).");
        }
        Ok(Self {
            game_dir: game_dir.to_path_buf(),
            keep,
            names,
        })
    }

    fn put_back(&self, r: &Reporter) -> Result<()> {
        if self.names.is_empty() {
            return Ok(());
        }
        fs::create_dir_all(&self.game_dir)?;
        for name in &self.names {
            let from = self.keep.join(name);
            if !from.exists() {
                continue;
            }
            let to = self.game_dir.join(name);
            if to.exists() {
                fsx::remove_dir(&to)?;
            }
            fs::rename(&from, &to).with_context(|| {
                format!(
                    "your saves are in {} - copy them back to {}",
                    from.display(),
                    to.display()
                )
            })?;
        }
        let _ = fs::remove_dir(&self.keep);
        r.log("Saves put back.");
        Ok(())
    }
}

fn get_game_files(
    r: &Reporter,
    tc: &Toolchain,
    o: &Options,
    root: &Path,
    game_dir: &Path,
) -> Result<()> {
    let replacing = match &o.game {
        GameSource::None => false,
        GameSource::Folder(src) => !same_dir(src, game_dir),
        GameSource::Iso(_) => !game_dir.join("default.xex").is_file(),
    };
    if !replacing {
        return replace_game_files(r, tc, o, root, game_dir);
    }
    let kept = KeptSaves::take(r, game_dir)?;
    let result = replace_game_files(r, tc, o, root, game_dir);
    let back = kept.put_back(r);
    result?;
    back
}

fn replace_game_files(
    r: &Reporter,
    tc: &Toolchain,
    o: &Options,
    root: &Path,
    game_dir: &Path,
) -> Result<()> {
    let have = game_dir.join("default.xex").is_file();
    match &o.game {
        GameSource::None => {
            if !have {
                bail!("The game files are missing. Choose your disc image (.iso) or game folder.");
            }
            r.log("Game files already in dist/game - skipping.");
        }
        GameSource::Folder(src) => {
            if have && same_dir(src, game_dir) {
                return Ok(());
            }
            r.status(format!("Copying the game files from {}", src.display()));
            fsx::remove_dir(game_dir)?;
            let mut n = 0u64;
            fsx::copy_tree(
                r,
                src,
                game_dir,
                &|name, _| name.eq_ignore_ascii_case("$SystemUpdate"),
                &mut n,
            )?;
            r.log(format!("Copied {n} files."));
        }
        GameSource::Iso(iso) => {
            if have {
                r.log("Game files already in dist/game - the disc image is not needed.");
                return Ok(());
            }
            let extractor = root
                .join("build")
                .join("host")
                .join(format!("xiso_extract{EXE}"));
            r.status("Building the disc image extractor");
            host_tool(
                r,
                tc,
                &extractor,
                &[root
                    .join("tools")
                    .join("xiso_extract")
                    .join("xiso_extract.cpp")],
                &[],
                &[],
            )?;
            r.status(format!(
                "Extracting the game from {} (about 6 GB)",
                iso.display()
            ));
            fsx::remove_dir(game_dir)?;
            let mut c = tc.env.command(&extractor);
            c.arg(iso).arg(game_dir);
            for s in ["$SystemUpdate", "layer1filler.bin", "layer2filler.bin"] {
                c.arg("--skip-name").arg(s);
            }
            run(r, c, "Extracting the disc image", |_| {})?;
        }
    }
    if !game_dir.join("default.xex").is_file() {
        bail!("The game files have no default.xex - is this an Xbox 360 Saints Row disc?");
    }
    Ok(())
}

fn same_dir(a: &Path, b: &Path) -> bool {
    match (fs::canonicalize(a), fs::canonicalize(b)) {
        (Ok(x), Ok(y)) => x == y,
        _ => false,
    }
}

fn copy_to_dist(
    r: &Reporter,
    root: &Path,
    game_build: &Path,
    sdk_install: &Path,
    dist: &Path,
    eos: Option<&Path>,
) -> Result<()> {
    fs::copy(game_build.join("saintsrow.exe"), dist.join("saintsrow.exe"))?;
    for e in fs::read_dir(sdk_install.join("bin"))? {
        let p = e?.path();
        if p.extension()
            .map(|x| x.eq_ignore_ascii_case("dll"))
            .unwrap_or(false)
        {
            fs::copy(&p, dist.join(p.file_name().unwrap()))?;
        }
    }
    fs::copy(
        game_build.join("WhompaysModLoader.exe"),
        dist.join("WhompaysModLoader.exe"),
    )?;

    // Whompay's Mod Loader: the mods folder, bundled mods and examples (off by default).
    let mods = dist.join("mods");
    fs::create_dir_all(&mods)?;
    let mod_skip = |name: &str, is_dir: bool| {
        !is_dir
            && ["*.c", "*.cpp", "*.ps1"]
                .iter()
                .any(|p| fsx::glob_match(name, p))
    };
    let mut n = 0;
    for parent in [
        root.join("modding").join("examples"),
        root.join("modding").join("mods"),
    ] {
        let Ok(rd) = fs::read_dir(&parent) else {
            continue;
        };
        for e in rd.flatten() {
            if e.file_type()?.is_dir() {
                fsx::copy_tree(r, &e.path(), &mods.join(e.file_name()), &mod_skip, &mut n)?;
            }
        }
    }
    fs::create_dir_all(mods.join("ExampleNative"))?;
    fs::copy(
        game_build.join("ExampleNative.dll"),
        mods.join("ExampleNative").join("ExampleNative.dll"),
    )?;
    fs::create_dir_all(mods.join("WhompaysTrainer"))?;
    fs::copy(
        game_build.join("WhompaysTrainer.dll"),
        mods.join("WhompaysTrainer").join("WhompaysTrainer.dll"),
    )?;

    // Built-in parts: always on, not in the mod list.
    let core = dist.join("core");
    let core_skip = |name: &str, is_dir: bool| {
        !is_dir
            && (["*.png", "*.py", "*.c", "*.cpp", "*.h", "*.ps1", "*.bak*"]
                .iter()
                .any(|p| fsx::glob_match(name, p))
                || name.eq_ignore_ascii_case("join_ip.txt"))
    };
    for e in fs::read_dir(root.join("core"))?.flatten() {
        if e.file_type()?.is_dir() {
            fsx::copy_tree(r, &e.path(), &core.join(e.file_name()), &core_skip, &mut n)?;
        }
    }
    let coop = core.join("WhompaysCoop");
    fs::create_dir_all(&coop)?;
    fs::copy(
        game_build.join("WhompaysCoop.dll"),
        coop.join("WhompaysCoop.dll"),
    )?;
    let join = coop.join("join_ip.txt");
    if !join.exists() {
        // Co-op was a mod before it was built in: keep an older install's join code / IP.
        let old = mods.join("WhompaysCoop").join("join_ip.txt");
        let src = if old.exists() {
            old
        } else {
            root.join("core").join("WhompaysCoop").join("join_ip.txt")
        };
        if src.exists() {
            fs::copy(src, &join)?;
        }
    }
    if let Some(e) = eos {
        fs::create_dir_all(coop.join("eos"))?;
        fs::copy(
            e.join("SDK").join("Bin").join("EOSSDK-Win64-Shipping.dll"),
            coop.join("eos").join("EOSSDK-Win64-Shipping.dll"),
        )?;
        if e.join("eos.ini").exists() {
            fs::copy(e.join("eos.ini"), coop.join("eos.ini"))?;
        }
    }
    Ok(())
}

/// Keyboard/mouse button pictures, made from the player's own game files
/// (they contain parts of the game's textures, so they are not downloaded).
fn make_glyphs(
    r: &Reporter,
    tc: &Toolchain,
    root: &Path,
    game_build: &Path,
    game_dir: &Path,
    dist: &Path,
) -> Result<()> {
    let exe = if cfg!(windows) {
        game_build.join("glyphgen.exe")
    } else {
        // The game build made a Windows glyphgen; build one for this PC.
        let out = root.join("build").join("host").join("glyphgen");
        let zlib = game_build.join("_deps").join("zlib-src");
        let zc: Vec<PathBuf> = [
            "adler32", "compress", "crc32", "deflate", "inffast", "inflate", "inftrees", "trees",
            "uncompr", "zutil",
        ]
        .iter()
        .map(|n| zlib.join(format!("{n}.c")))
        .collect();
        host_tool(
            r,
            tc,
            &out,
            &[
                root.join("tools").join("glyphgen").join("glyphgen.cpp"),
                root.join("project")
                    .join("src")
                    .join("wml")
                    .join("packfile.cpp"),
            ],
            &zc,
            &[root.join("project").join("src").join("wml"), zlib.clone()],
        )?;
        out
    };
    let mut c = tc.env.command(&exe);
    c.arg(game_dir.join("packfiles"))
        .arg(root.join("tools").join("glyphgen").join("art.txt"))
        .arg(dist.join("kbm_ui.bin"));
    run(r, c, "glyphgen", |_| {})
}
