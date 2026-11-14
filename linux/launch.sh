#!/bin/sh
# No sudo, package-manager changes or shell-profile edits.
set -eu
sr_here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
sr_error() {
    printf '%s\n' "$1" >&2
    if command -v zenity >/dev/null 2>&1; then
        zenity --error --title='Saints Reborn Setup' --text="$1" || :
    elif command -v kdialog >/dev/null 2>&1; then
        kdialog --error "$1" --title 'Saints Reborn Setup' || :
    fi
    exit 1
}
case "$(uname -m)" in x86_64) ;; *) sr_error 'This Setup currently supports x86-64 Linux, including Steam Deck.' ;; esac
sr_cli=false
for sr_arg in "$@"; do
    case "$sr_arg" in --cli|--play|--repair-mods|--add-to-steam|--help|-h) sr_cli=true ;; esac
done
if command -v python3 >/dev/null 2>&1; then
    if [ "$sr_cli" = true ] || python3 -c 'import tkinter' >/dev/null 2>&1; then
        exec python3 "$sr_here/setup.py" "$@"
    fi
fi
# Obtain a private Python/Tk only on systems missing a usable desktop Python.
# Choose the drive first so the environment is not put on a small system disk.
sr_dest=''
if command -v zenity >/dev/null 2>&1; then
    sr_dest=$(zenity --file-selection --directory --title='Choose a drive/folder for Saints Reborn and its required programs') || exit 0
elif command -v kdialog >/dev/null 2>&1; then
    sr_dest=$(kdialog --getexistingdirectory "$HOME" --title 'Choose a drive/folder for Saints Reborn') || exit 0
elif [ -t 0 ]; then
    printf 'Choose a full parent folder path for Saints Reborn: '
    read -r sr_dest
else
    sr_error 'No graphical folder picker was found. Run this file in a terminal to choose where to download the private Python interface.'
fi
case "$sr_dest" in /*) ;; *) sr_error 'Choose an absolute folder path.' ;; esac
sr_dest="$sr_dest/SaintsReborn"
sr_boot="$sr_dest/.saintsreborn-setup/python-bootstrap"
mkdir -p "$sr_boot"
sr_task() {
    sr_note=$1
    shift
    "$@" >"$sr_boot/download.log" 2>&1 &
    sr_task_pid=$!
    if command -v zenity >/dev/null 2>&1; then
        (
            printf '#%s\n' "$sr_note"
            while kill -0 "$sr_task_pid" 2>/dev/null; do sleep 1; done
            printf '100\n'
        ) | zenity --progress --pulsate --auto-close --no-cancel --title='Saints Reborn Setup' --text="$sr_note" || :
    elif command -v kdialog >/dev/null 2>&1; then
        kdialog --passivepopup "$sr_note" 10 --title 'Saints Reborn Setup' || :
    fi
    wait "$sr_task_pid"
}
sr_hash=$(printf '%s' "$sr_boot" | sha256sum)
sr_hash=${sr_hash%% *}
sr_alias_base="/tmp/saintsreborn-python-$(id -u)"
if [ ! -e "$sr_alias_base" ] && [ ! -L "$sr_alias_base" ]; then mkdir -m 700 "$sr_alias_base"; fi
[ -d "$sr_alias_base" ] && [ ! -L "$sr_alias_base" ] && [ "$(stat -c '%u:%a' "$sr_alias_base")" = "$(id -u):700" ] || sr_error 'The temporary Python setup folder is not private to your user.'
sr_alias="$sr_alias_base/$sr_hash"
if [ -L "$sr_alias" ]; then
    [ "$(readlink "$sr_alias")" = "$sr_boot" ] || sr_error 'The temporary Python alias points elsewhere.'
else
    ln -s "$sr_boot" "$sr_alias"
fi
sr_mamba="$sr_alias/micromamba"
sr_expected=366cd9cd8be14df1ab8ed50352a82111082a36686b2d389fdb79a92c3fafb3e3
sr_url=https://github.com/mamba-org/micromamba-releases/releases/download/2.9.0-0/micromamba-linux-64
sr_valid=false
if [ -f "$sr_mamba" ]; then
    sr_actual=$(sha256sum "$sr_mamba")
    [ "${sr_actual%% *}" = "$sr_expected" ] && sr_valid=true
fi
if [ "$sr_valid" = false ]; then
    printf 'Downloading the private setup package manager…\n'
    if command -v curl >/dev/null 2>&1; then
        sr_task 'Downloading the setup package manager…' curl --fail --location --proto '=https' --tlsv1.2 --retry 2 --connect-timeout 15 --output "$sr_mamba.partial" "$sr_url" || sr_error 'Download failed. Check your network/DNS and reopen Setup.'
    elif command -v wget >/dev/null 2>&1; then
        sr_task 'Downloading the setup package manager…' wget --https-only --tries=3 --timeout=30 -O "$sr_mamba.partial" "$sr_url" || sr_error 'Download failed. Check your network/DNS and reopen Setup.'
    else
        sr_error 'A downloader (curl or wget) is needed to obtain Python on this system.'
    fi
    sr_actual=$(sha256sum "$sr_mamba.partial")
    [ "${sr_actual%% *}" = "$sr_expected" ] || sr_error 'The private package manager failed SHA-256 verification and was not run.'
    chmod 755 "$sr_mamba.partial"
    mv "$sr_mamba.partial" "$sr_mamba"
fi
if [ ! -x "$sr_alias/env/bin/python" ]; then
    printf 'Downloading the Python setup interface…\n'
    sr_task 'Downloading the private Python setup interface…' "$sr_mamba" --no-rc --root-prefix "$sr_alias/packages" create --yes --prefix "$sr_alias/env" --channel conda-forge --strict-channel-priority python=3.12 tk ca-certificates || sr_error 'The private Python download failed. Check the network connection and reopen Setup.'
fi
exec "$sr_alias/env/bin/python" "$sr_here/setup.py" --dir "$sr_dest" "$@"
