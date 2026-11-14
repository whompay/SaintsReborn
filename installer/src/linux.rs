//! Probe the chosen filesystem rather than guessing from a distribution name.
use anyhow::{Context, Result, bail};
use std::fs;
use std::os::unix::fs::{PermissionsExt, symlink};
use std::path::{Path, PathBuf};
use std::process::Command;

struct Probe(PathBuf);
impl Drop for Probe {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

pub fn preflight(dir: &Path) -> Result<()> {
    if std::env::consts::ARCH != "x86_64" {
        bail!("The bundled toolchain and game require x86-64 Linux.");
    }
    // Reserve a private folder without following a pre-existing symlink.
    let path = dir.join(format!(".sr-drive-probe-{}", std::process::id()));
    fs::create_dir(&path).with_context(|| {
        format!(
            "Cannot write to {}. Check that the drive is mounted and owned by your user.",
            dir.display()
        )
    })?;
    let probe = Probe(path);
    let script = probe.0.join("execute");
    fs::write(&script, "#!/bin/sh\nexit 0\n")?;
    fs::set_permissions(&script, fs::Permissions::from_mode(0o700))?;
    let result = Command::new(&script).status().with_context(|| {
        format!("Cannot execute build tools on {}. The drive may be mounted noexec; choose an executable Linux filesystem.", dir.display())
    })?;
    if !result.success() {
        bail!("The execution check on the install drive failed.");
    }
    symlink("execute", probe.0.join("link")).context(
        "The install drive must support symbolic links. Use ext4, Btrfs or another Linux filesystem; exFAT/FAT are unsuitable for the build tools."
    )?;
    let upper = probe.0.join("CaseProbe");
    fs::write(&upper, "case")?;
    if probe.0.join("caseprobe").exists() {
        bail!(
            "The install drive is case-insensitive. Choose a case-sensitive filesystem for the Linux build tools."
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn supports_spaces_and_cleans_probe() {
        let dir = std::env::temp_dir().join(format!("sr second drive test {}", std::process::id()));
        fs::create_dir(&dir).unwrap();
        preflight(&dir).unwrap();
        assert_eq!(fs::read_dir(&dir).unwrap().count(), 0);
        fs::remove_dir(dir).unwrap();
    }
}
