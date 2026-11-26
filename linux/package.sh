#!/bin/sh
# Makes Saints-Reborn-Linux-Setup.run: a small shell header plus a tar.gz of the Linux setup
# (launch.sh, setup.py), the setup core (installer/, built on the player's PC) and the notices.
#   linux/package.sh [--repo https://github.com/OWNER/NAME.git] [--branch NAME] [--out FILE]
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname -- "$here")
repo='https://github.com/whompay/SaintsReborn.git'
branch='main'
out="$root/dist/Saints-Reborn-Linux-Setup.run"
while [ $# -gt 0 ]; do
    case "$1" in
        --repo) repo=$2; shift 2 ;;
        --branch) branch=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        *) printf 'unknown argument %s\n' "$1" >&2; exit 2 ;;
    esac
done
case "$repo" in https://github.com/*/*.git) ;; *) printf -- '--repo must look like https://github.com/OWNER/NAME.git\n' >&2; exit 2 ;; esac
case "$repo$branch" in *[!A-Za-z0-9._/:-]*) printf 'bad --repo or --branch\n' >&2; exit 2 ;; esac
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
p="$work/payload"
mkdir -p "$p/core"
cp "$here/launch.sh" "$root/LICENSE" "$root/NOTICE" "$root/THIRD_PARTY_NOTICES.md" "$p/"
sed -e "s|^DEFAULT_REPO = .*|DEFAULT_REPO = '$repo'|" \
    -e "s|^DEFAULT_BRANCH = .*|DEFAULT_BRANCH = '$branch'|" "$here/setup.py" >"$p/setup.py"
grep -q "^DEFAULT_REPO = '$repo'" "$p/setup.py"
cp -R "$root/installer/src" "$root/installer/Cargo.toml" "$root/installer/Cargo.lock" "$root/installer/build.rs" "$p/core/"
chmod 755 "$p/launch.sh"
mkdir -p "$(dirname -- "$out")"
cat >"$out" <<'EOF'
#!/bin/sh
set -eu
sr_tmp=$(mktemp -d "${TMPDIR:-/tmp}/saintsreborn-setup.XXXXXXXX")
trap 'rm -rf -- "$sr_tmp"' EXIT HUP INT TERM
sr_line=$(awk '/^__SAINTSREBORN_PAYLOAD__$/ { print NR + 1; exit }' "$0")
if [ -z "$sr_line" ]; then printf 'Installer payload missing.\n' >&2; exit 1; fi
if ! tail -n +"$sr_line" "$0" | tar -xz -C "$sr_tmp"; then
    printf 'Installer payload is damaged. Download or package it again.\n' >&2
    exit 1
fi
sh "$sr_tmp/launch.sh" "$@"
exit $?
__SAINTSREBORN_PAYLOAD__
EOF
tar -C "$p" --owner=0 --group=0 --numeric-owner -czf - . >>"$out"
chmod 755 "$out"
printf '%s (%s bytes) for %s, branch %s\n' "$out" "$(wc -c <"$out")" "$repo" "$branch"
