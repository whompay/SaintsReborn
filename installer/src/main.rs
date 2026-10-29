// SPDX-License-Identifier: MIT
//! Saints Reborn Setup: installs and updates Saints Reborn on Windows and
//! Linux. Downloads a portable build toolchain (no Visual Studio), builds the
//! game from the player's own disc, and keeps it up to date.
#![windows_subsystem = "windows"]
// Without the window (the Linux setup builds it that way) some helpers only it uses are idle.
#![cfg_attr(not(feature = "gui"), allow(dead_code))]

mod config;
mod fsx;
#[cfg(feature = "gui")]
mod gui;
#[cfg(target_os = "linux")]
mod linux;
mod online;
mod pipeline;
mod platform;
mod report;
mod tools;

use pipeline::{GameSource, Options};
use report::Reporter;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;

const HELP: &str = "Saints Reborn Setup

Without options the setup window opens. Command line use:
  SaintsReborn-Setup --cli --dir <folder> [--iso <file> | --game-dir <folder>]
                     --accept-license [--update] [--force] [--no-shortcut]

  --dir <folder>       install folder (or the existing Saints Reborn folder)
  --iso <file>         your Saints Row (Xbox 360) disc image
  --game-dir <folder>  a folder with your game files (default.xex, packfiles)
  --accept-license     accept the Microsoft Visual Studio license terms for the
                       C++ headers and libraries that are downloaded
  --update             update an existing install (default when --dir is one)
  --force              rebuild even when nothing changed
  --no-shortcut        no desktop shortcut
  --no-integration     no shortcuts or menu entries at all, and the folder is
                       not remembered (for test builds)
  --online-stamp <src> print the online pack stamp of a source folder
  --sdk-only <src>     only build the toolchain, the SDK and the recompiler
                       for a source folder (for CI; no game files needed)";

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let has = |f: &str| {
        args.iter()
            .any(|a| a.eq_ignore_ascii_case(f) || a.eq_ignore_ascii_case(&f.replace("--", "/")))
    };
    let value = |f: &str| {
        args.iter()
            .position(|a| a.eq_ignore_ascii_case(f))
            .and_then(|i| args.get(i + 1))
            .map(PathBuf::from)
    };
    let cli =
        has("--cli") || has("--help") || has("-h") || has("--online-stamp") || has("--sdk-only");
    if cli {
        platform::attach_console();
    }
    if has("--help") || has("-h") {
        println!("{HELP}");
        return;
    }
    if let Some(src) = value("--online-stamp") {
        let commit = pipeline::sdk_commit(&src);
        match online::stamp(&src, &commit) {
            Ok(s) => println!("{s}"),
            Err(e) => {
                eprintln!("{e:#}");
                std::process::exit(1)
            }
        }
        return;
    }
    if let Some(src) = value("--sdk-only") {
        let r = Reporter::new(None, Arc::new(AtomicBool::new(false)), true);
        match pipeline::sdk_only(
            &r,
            &std::path::absolute(&src).unwrap_or(src),
            has("--accept-license"),
        ) {
            Ok(s) => println!("{s}"),
            Err(e) => {
                eprintln!("ERROR: {e:#}");
                std::process::exit(1)
            }
        }
        return;
    }
    if cli {
        let Some(dir) = value("--dir") else {
            eprintln!("--dir is needed.\n\n{HELP}");
            std::process::exit(2)
        };
        let game = match (value("--iso"), value("--game-dir")) {
            (Some(i), _) => GameSource::Iso(i),
            (_, Some(g)) => GameSource::Folder(g),
            _ => GameSource::None,
        };
        let dir = std::path::absolute(&dir).unwrap_or(dir);
        let o = Options {
            update: has("--update") || pipeline::is_install_folder(&dir),
            dir,
            game,
            desktop_shortcut: !has("--no-shortcut") && !has("--no-integration"),
            integrate: !has("--no-integration"),
            accept_license: has("--accept-license"),
            force: has("--force"),
            eos: std::env::var_os("SR_EOS").map(PathBuf::from),
        };
        if let Err(e) = pipeline::validate(&o) {
            eprintln!("{e:#}");
            std::process::exit(2)
        }
        let r = Reporter::new(None, Arc::new(AtomicBool::new(false)), true);
        match pipeline::run_all(&r, &o) {
            Ok(s) => {
                r.log(s);
                r.close_log();
            }
            Err(e) => {
                r.log(format!("ERROR: {e:#}"));
                r.close_log();
                std::process::exit(1)
            }
        }
        return;
    }

    #[cfg(feature = "gui")]
    {
        let update = has("--update");
        let dir = value("--dir");
        let options = eframe::NativeOptions {
            viewport: eframe::egui::ViewportBuilder::default()
                .with_title("Saints Reborn Setup")
                .with_inner_size([760.0, 560.0])
                .with_min_inner_size([620.0, 460.0]),
            ..Default::default()
        };
        let _ = eframe::run_native(
            "Saints Reborn Setup",
            options,
            Box::new(move |cc| {
                cc.egui_ctx.set_theme(eframe::egui::Theme::Dark);
                Ok(Box::new(gui::SetupApp::new(update, dir)))
            }),
        );
    }
    #[cfg(not(feature = "gui"))]
    {
        eprintln!("Use the Linux Setup launcher, or pass --cli.\n\n{HELP}");
        std::process::exit(2);
    }
}
