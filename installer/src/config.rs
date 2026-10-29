//! Fixed settings: where things are downloaded from and what they must hash to.

pub const APP_VERSION: &str = env!("CARGO_PKG_VERSION");
pub const REPO_URL: &str = match option_env!("SR_FORK_REPO_URL") {
    Some(url) => url,
    None => "https://github.com/whompay/SaintsReborn.git",
};
pub const REPO_PAGE: &str = "https://github.com/whompay/SaintsReborn";
pub const BRANCH: &str = match option_env!("SR_FORK_BRANCH") {
    Some(branch) => branch,
    None => "main",
};

pub const SDK_REPO: &str = "https://github.com/rexglue/rexglue-sdk.git";
/// Used when the downloaded source does not name an SDK commit itself.
pub const SDK_COMMIT_FALLBACK: &str = "c94f5ebdcb3c9d1a460ca48e04f9758448f8d518";

/// SHA-1 of the disc version the hooks are made for: Saints Row (World).
pub const KNOWN_XEX_SHA1: &str = "C2646093FF3926141FCF4FD630E876873D1CAECE";

pub const ONLINE_PACK_URL: &str =
    "https://github.com/whompay/SaintsReborn/releases/download/online-pack/SaintsReborn-Online.zip";

/// Changes whenever the compiler or the Microsoft libraries change, so old
/// build folders (made with another toolchain) are rebuilt from scratch.
pub const TOOLCHAIN_ID: &str = "llvm-19.1.5+msvc-14.44.17.14+sdk-10.0.26100+r3-linux-bootstrap";

/// Needs to match the clang that recorded the profiles in pgo\.
pub const LLVM_VERSION: &str = "19.1.5";
pub const XWIN_CRT_VERSION: &str = "14.44.17.14";
pub const XWIN_SDK_VERSION: &str = "10.0.26100";

/// A first install: build tools (about 2.5 GB) and the build.
pub const NEEDED_FREE_GB: u64 = 20;
/// Below this Setup stops before downloading anything.
pub const MIN_FREE_GB: u64 = 6;

#[derive(Clone, Copy)]
pub enum Archive {
    /// .tar.xz / .tar.gz, unpacked with the system's tar.
    Tar { strip: bool },
    /// .zip, unpacked here.
    Zip { strip: bool },
}

#[derive(Clone, Copy)]
pub struct Download {
    pub name: &'static str,
    pub url: &'static str,
    pub sha256: &'static str,
    pub size: u64,
    pub archive: Archive,
    /// Folder under the tools folder it is unpacked to.
    pub dir: &'static str,
    /// Unpack only what the build uses (LLVM: about 0.7 of 2.8 GB).
    pub trim: bool,
}

#[cfg(windows)]
pub const TOOLS: &[Download] = &[
    Download {
        name: "LLVM (clang) 19.1.5",
        url: "https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.5/clang+llvm-19.1.5-x86_64-pc-windows-msvc.tar.xz",
        sha256: "467d1a73ca938f47734af3baac2e78c5e730285469096ee088bb5c9590cabd70",
        size: 845_193_092,
        archive: Archive::Tar { strip: true },
        dir: "llvm-19.1.5",
        trim: true,
    },
    Download {
        name: "CMake 3.31.6",
        url: "https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.zip",
        sha256: "d163cd3ab4959b0a53fa8988f2ddbd2e6c501658201e6a154386bad9dbe4f836",
        size: 46_473_549,
        archive: Archive::Zip { strip: true },
        dir: "cmake-3.31.6",
        trim: false,
    },
    Download {
        name: "Ninja 1.13.2",
        url: "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
        sha256: "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65",
        size: 291_570,
        archive: Archive::Zip { strip: false },
        dir: "ninja-1.13.2",
        trim: false,
    },
    Download {
        name: "xwin 0.10.0",
        url: "https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/xwin-0.10.0-x86_64-pc-windows-msvc.tar.gz",
        sha256: "96e83665ef5c1406aabd0be603a8d08e67e604f36fda630b6807ee948a614e26",
        size: 3_074_443,
        archive: Archive::Tar { strip: true },
        dir: "xwin-0.10.0",
        trim: false,
    },
];

#[cfg(windows)]
pub const MINGIT: Download = Download {
    name: "Git 2.47.1",
    url: "https://github.com/git-for-windows/git/releases/download/v2.47.1.windows.1/MinGit-2.47.1-64-bit.zip",
    sha256: "50b04b55425b5c465d076cdb184f63a0cd0f86f6ec8bb4d5860114a713d2c29a",
    size: 47_241_394,
    archive: Archive::Zip { strip: false },
    dir: "git-2.47.1",
    trim: false,
};

#[cfg(windows)]
pub const VC_REDIST_URL: &str = "https://aka.ms/vs/17/release/vc_redist.x64.exe";

#[cfg(not(windows))]
pub const TOOLS: &[Download] = &[
    Download {
        name: "LLVM (clang) 19.1.5",
        url: "https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.5/LLVM-19.1.5-Linux-X64.tar.xz",
        sha256: "13e9975b026d431c945927960e5f8c0a47a155a2f600f57e85f4d1482620c65f",
        size: 1_652_713_376,
        archive: Archive::Tar { strip: true },
        dir: "llvm-19.1.5",
        trim: true,
    },
    Download {
        name: "CMake 3.31.6",
        url: "https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz",
        sha256: "5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf",
        size: 55_010_959,
        archive: Archive::Tar { strip: true },
        dir: "cmake-3.31.6",
        trim: false,
    },
    Download {
        name: "Ninja 1.13.2",
        url: "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip",
        sha256: "5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6",
        size: 134_040,
        archive: Archive::Zip { strip: false },
        dir: "ninja-1.13.2",
        trim: false,
    },
    Download {
        name: "xwin 0.10.0",
        url: "https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/xwin-0.10.0-x86_64-unknown-linux-musl.tar.gz",
        sha256: "d870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88",
        size: 3_416_863,
        archive: Archive::Tar { strip: true },
        dir: "xwin-0.10.0",
        trim: false,
    },
];

pub const MS_LICENSE_URL: &str = "https://visualstudio.microsoft.com/license-terms/";
