param([switch]$NoInstall)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools are required.' }
$compiler = Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang++.exe'
$out = Join-Path $root 'build/wantedandturf'
New-Item -ItemType Directory -Force $out | Out-Null
$args = @('-std=c++23', '-O2', '-msse4.1', '-D_CRT_SECURE_NO_WARNINGS', '-Wall',
    "-I$root/build/sdk/include", "-I$root/modding/include", '-shared',
    (Join-Path $PSScriptRoot 'main.cpp'), '-o', (Join-Path $out 'WantedAndTurf.dll'),
    '-luser32', '-lgdi32')
& $compiler @args
if ($LASTEXITCODE) { throw 'Wanted & Turf build failed.' }
if (-not $NoInstall) {
    $dest = Join-Path $root 'dist/mods/WantedAndTurf'
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item (Join-Path $out 'WantedAndTurf.dll') $dest -Force
    Copy-Item (Join-Path $PSScriptRoot 'mod.ini'), (Join-Path $PSScriptRoot 'README.md') $dest -Force
    Remove-Item (Join-Path $dest 'WantedAndTurf.lib'), (Join-Path $dest 'WantedAndTurf.exp') -ErrorAction SilentlyContinue
    Write-Host "Built and installed: $dest"
}
