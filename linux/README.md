# Saints Reborn on Linux

The Linux setup builds Saints Reborn on Linux from **your own** Saints Row (Xbox 360) disc and plays it
with Proton. It builds the same Windows game as the Windows Setup: the same recompiled code,
the same clang 19, the same PGO profiles. Proton then runs it. There is no separate Linux
renderer to fall behind the Windows one.

The setup never asks for `sudo` and changes nothing in the system. Everything it downloads goes
into the install folder: a private Python, Rust and C/C++ compilers (conda-forge, through a
SHA-256-pinned micromamba), LLVM, CMake, Ninja, Microsoft's C++ headers and libraries (xwin),
the Proton launcher and the Proton prefix.

## Install

1. Download `Saints-Reborn-Linux-Setup.run` from the Releases page.
2. Make it executable (file manager: Properties > Permissions > "Allow executing", or
   `chmod +x Saints-Reborn-Linux-Setup.run`) and open it.
3. Choose an install folder (any mounted Linux drive) and your Saints Row ISO or extracted game
   folder (with `default.xex`). Accept the Microsoft license and press **Install / Update**.

The first install needs about 26 GB free and takes an hour or more. Afterwards, start the game
with **Play Saints Reborn** in your application menu. On the first start, Proton (GE-Proton)
downloads itself. On SteamOS / Steam Deck, press **Add to Steam** instead and pick a Proton
version in the game's Compatibility settings; it then works in Game Mode.

Without a desktop, or over SSH:

```sh
./Saints-Reborn-Linux-Setup.run --cli --accept-license --dir ~/Games/SaintsReborn --iso ~/SaintsRow.iso
./Saints-Reborn-Linux-Setup.run --play --dir ~/Games/SaintsReborn
```

**Updating:** use **Update Saints Reborn** in the application menu, or open the setup again on
the same folder. Saves and your mod selection are kept.

## What each kind of distribution needs

The setup checks this itself and says what to do.

| Distribution | Notes |
|---|---|
| Ubuntu, Debian, Mint, Pop!_OS, Fedora, Arch, Manjaro, CachyOS, openSUSE, Gentoo, ... | Works as is. Without `python3-tk`, the setup fetches its own Python with Tk; you don't need to install it. |
| SteamOS / Steam Deck | Run the setup in Desktop Mode, then **Add to Steam**. |
| Bazzite, Fedora Silverblue/Kinoite, Aurora, Bluefin, openSUSE Aeon, Vanilla OS | Works: nothing goes into the read-only system. |
| NixOS | Turn on `programs.nix-ld.enable = true;` or run the setup inside an FHS container. |
| Alpine, Void (musl), Chimera | Not supported directly (needs glibc 2.28+): use a distrobox. |
| ARM | Not supported: the recompiled game is x86-64 code. |

Every distribution also needs:

* **A Vulkan driver** for Proton: Mesa (AMD, Intel) or the NVIDIA driver.
* **A Linux file system** for the install folder: ext4, Btrfs, XFS and so on. It must allow
  running programs, symbolic links and case-sensitive names, so exFAT/FAT/`noexec` drives are
  refused. A second drive works if it's mounted.

## Maintainers

```sh
linux/package.sh      # -> dist/Saints-Reborn-Linux-Setup.run (installs and updates from whompay/SaintsReborn, main)
```

The setup builds the game from the repository and branch baked into the `.run`
(`--repo` / `--branch`). The Linux changes to `installer/` are small: the `gui` feature, `linux.rs`
and `#[cfg(unix)]` paths. Windows builds are unchanged.
