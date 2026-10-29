#!/usr/bin/env python3
"""Saints Reborn's unprivileged Linux setup front end (stdlib only)."""
import argparse
import fcntl
import hashlib
import json
import os
import platform
import queue
import selectors
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import tarfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from urllib.parse import quote

HERE = Path(__file__).resolve().parent
MAMBA_URL = 'https://github.com/mamba-org/micromamba-releases/releases/download/2.9.0-0/micromamba-linux-64'
MAMBA_SHA256 = '366cd9cd8be14df1ab8ed50352a82111082a36686b2d389fdb79a92c3fafb3e3'
UMU_URL = 'https://github.com/Open-Wine-Components/umu-launcher/releases/download/1.4.4/umu-launcher-1.4.4-zipapp.tar'
UMU_SHA256 = 'eb590691841f7fad3fc3ad8fd5db4ccb87849fe7948e62b28ece7a4ee48cc851'
STEAM_SENTINEL = Path('/tmp/addnonsteamgamefile')
DEFAULT_REPO = 'https://github.com/whompay/SaintsReborn.git'   # set by linux/package.sh --repo
DEFAULT_BRANCH = 'main'                                         # set by linux/package.sh --branch
ENV_VERSION = 'native-gcc14-rust-python-x11-wayland-protocols-v4'
PACKAGES = ['rust>=1.88', 'gcc_linux-64=14', 'gxx_linux-64=14', 'git',
            'pkg-config', 'openssl', 'ca-certificates', 'make', 'zlib', 'zstd', 'python=3.12', 'xorg-libx11', 'xorg-xorgproto', 'wayland', 'libiconv']


class SetupError(Exception):
    pass


class Cancelled(SetupError):
    pass


def is_steamos():
    try:
        release = dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines()
                       if '=' in line and not line.startswith('#'))
    except OSError:
        return False
    return release.get('ID', '').strip('"\'') == 'steamos'


def steam_command():
    native = shutil.which('steam')
    if native:
        return [native]
    flatpak = shutil.which('flatpak')
    if flatpak and (Path.home() / '.var/app/com.valvesoftware.Steam').is_dir():
        return [flatpak, 'run', 'com.valvesoftware.Steam']
    raise SetupError('Steam was not found. Open or install Steam, then try Add to Steam again.')


def add_to_steam(directory):
    directory = Path(directory).expanduser().resolve()
    game = directory / 'dist/WhompaysModLoader.exe'
    if not game.is_file():
        game = directory / 'dist/saintsrow.exe'
    if not game.is_file():
        raise SetupError('Finish installing Saints Reborn before adding it to Steam.')
    command = steam_command()
    # Steam adds the file it is given as the shortcut's target (a .desktop file would itself become the
    # target, which Proton cannot run), with the file's folder (dist) as its start folder.
    # Steam's Linux import handler requires a local-path marker. Do not follow
    # a pre-existing symlink or accept a marker owned by another user.
    flags = os.O_CREAT | os.O_WRONLY | os.O_NOFOLLOW | os.O_NONBLOCK
    descriptor = os.open(STEAM_SENTINEL, flags, 0o600)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
            raise SetupError('Steam import marker is not a regular file owned by your user.')
    finally:
        os.close(descriptor)
    uri = 'steam://addnonsteamgame/' + quote(str(game), safe='')
    try:
        subprocess.run([*command, uri], check=True, timeout=20,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        raise SetupError('Steam did not accept the request. Open Steam and try Add to Steam again.') from exc
    return ('Request sent to Steam. Find Saints Reborn in your Library.\n\n'
            'If required, open Properties → Compatibility and select a Proton version '
            'you already have installed. Setup has not downloaded Proton.')


def existing_install(path):
    return any((path / p).is_file() for p in ('setup.bat', 'scripts/setup.ps1', 'project/CMakeLists.txt'))


def validate_destination(path, update=False, minimum_free=26 * 1024**3):
    path = Path(path).expanduser()
    if not path.is_absolute() or path == Path('/'):
        raise SetupError('Choose a full path to a dedicated Saints Reborn folder.')
    if any(c in str(path) for c in '\n\r\t;\\'):
        raise SetupError('Choose a folder without control characters, semicolons or backslashes (build-tool path limitations).')
    # Do not silently recreate an absent removable mount on the system disk.
    for base in ('/run/media/', '/media/', '/mnt/'):
        if str(path).startswith(base):
            ancestor = path
            while not ancestor.exists():
                ancestor = ancestor.parent
            if not any(p.is_mount() for p in (ancestor, *ancestor.parents) if str(p) not in ('/', '/run', '/media', '/mnt')):
                raise SetupError('The selected secondary drive is not mounted. Mount it in the file manager first.')
    if path.exists() and not path.is_dir():
        raise SetupError('The install path is a file. Choose a folder instead.')
    if update and not existing_install(path):
        raise SetupError('Choose the existing Saints Reborn folder containing project and dist.')
    if path.exists() and not existing_install(path):
        allowed = {'.saintsreborn-setup', '.download', 'setup-installer.log'}
        if any(p.name not in allowed for p in path.iterdir()):
            raise SetupError('Choose an empty folder, or an existing Saints Reborn installation.')
    path.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(path).free < minimum_free:
        raise SetupError('The selected drive needs at least 26 GB free for a first build and its private tools.')
    with tempfile.TemporaryDirectory(prefix='.sr-drive-check-', dir=path) as tmp:
        probe = Path(tmp)
        executable = probe / 'execute'
        executable.write_text('#!/bin/sh\nexit 0\n')
        executable.chmod(0o700)
        try:
            subprocess.run([str(executable)], check=True, timeout=10, capture_output=True)
            (probe / 'link').symlink_to('execute')
        except (OSError, subprocess.SubprocessError) as exc:
            raise SetupError('This drive must allow running programs and symbolic links. Use ext4/Btrfs and check its mount permissions; exFAT/FAT will not work.') from exc
        (probe / 'CaseProbe').write_text('case')
        if (probe / 'caseprobe').exists():
            raise SetupError('The build requires a case-sensitive filesystem. Choose a Linux filesystem such as ext4 or Btrfs.')
    return path.resolve()


def short_alias(destination):
    # Conda prefixes must not contain spaces. Only this small symlink is on /tmp;
    # the environment, cache and all downloads remain on the selected drive.
    base = Path('/tmp') / ('saintsreborn-' + str(os.getuid()))
    try:
        base.mkdir(mode=0o700)
    except FileExistsError:
        pass
    info = base.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise SetupError('The temporary setup directory is not private to your user: ' + str(base))
    link = base / hashlib.sha256(os.fsencode(destination)).hexdigest()[:24]
    if link.is_symlink():
        if link.resolve() != destination.resolve():
            raise SetupError('The temporary drive alias points to another location: ' + str(link))
    elif link.exists():
        raise SetupError('The temporary drive alias is occupied: ' + str(link))
    else:
        link.symlink_to(destination, target_is_directory=True)
    return link


def desktop_value(text):
    return str(text).replace('\\', '\\\\').replace('\n', '\\n').replace('\r', '\\r').replace('\t', '\\t')


def desktop_arg(text):
    text = str(text).replace('%', '%%')
    for char in ('\\', '"', '`', '$'):
        text = text.replace(char, '\\' + char)
    return desktop_value('"' + text + '"')


def atomic_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as f:
        tmp = Path(f.name)
        f.write(text)
    tmp.replace(path)


class KeepAwake:
    """Keeps the PC from sleeping while setup works: the first install takes an hour or more, and a PC
    (a Steam Deck in particular) that suspends meanwhile stops it. A logind inhibitor (systemd-inhibit,
    or elogind-inhibit) is held by a small process that ends by itself when setup does, even if setup
    is killed, so the PC is never left unable to sleep."""

    def __init__(self, say, who):
        self.say = say
        self.who = who
        self.proc = None

    def start(self):
        tool = shutil.which('systemd-inhibit') or shutil.which('elogind-inhibit')
        if not tool:
            self.say('Note: this system cannot be kept awake automatically; keep it from sleeping until setup finishes.')
            return self
        watch = ('import os, sys, time\n'
                 'while True:\n'
                 '    try:\n'
                 '        os.kill(%d, 0)\n'
                 '    except OSError:\n'
                 '        sys.exit(0)\n'
                 '    time.sleep(5)\n' % os.getpid())
        try:
            self.proc = subprocess.Popen([tool, '--what=sleep:idle', '--mode=block', '--who=' + self.who,
                                          '--why=Building the game', sys.executable, '-c', watch],
                                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                         stderr=subprocess.DEVNULL)
            time.sleep(0.5)
        except OSError:
            self.proc = None
        if self.proc is None or self.proc.poll() is not None:
            self.proc = None
            self.say('Note: the PC could not be kept awake automatically; keep it from sleeping until setup finishes.')
        else:
            self.say('Keeping the PC awake until setup finishes.')
        return self

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None


def tree_pids(pid):
    children = []
    try:
        raw = Path('/proc/%s/task/%s/children' % (pid, pid)).read_text()
        for child in map(int, raw.split()):
            children.extend(tree_pids(child))
    except (OSError, ValueError):
        pass
    return children + [pid]


class Engine:
    def __init__(self, destination, source=None, update=False, emit=print, cancel=None,
                 payload=HERE, repository=DEFAULT_REPO, branch=DEFAULT_BRANCH):
        self.destination = Path(destination)
        self.source = Path(source).expanduser().resolve() if source else None
        self.update = update
        self.emit = emit
        self.cancel = cancel or threading.Event()
        self.payload = Path(payload)
        self.repository = repository
        self.branch = branch
        self.log = None
        self.work = None
        self.alias = None
        self.env = os.environ.copy()

    def say(self, text):
        if self.log:
            self.log.write(str(text) + '\n')
            self.log.flush()
        self.emit(str(text))

    def check_cancel(self):
        if self.cancel.is_set():
            raise Cancelled('Setup stopped. Run it again to retry; completed downloads and builds are kept.')

    def run(self, command, cwd=None, env=None):
        self.check_cancel()
        self.say('Running: ' + str(command[0]) + ' ' + ' '.join(map(str, command[1:])))
        process = subprocess.Popen(list(map(str, command)), cwd=cwd, env=env or self.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        pending = b''
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while selector.get_map():
                    self.check_cancel()
                    for key, _ in selector.select(timeout=0.2):
                        chunk = os.read(key.fileobj.fileno(), 65536)
                        if not chunk:
                            selector.unregister(key.fileobj)
                            continue
                        pending += chunk.replace(b'\r', b'\n')
                        lines = pending.split(b'\n')
                        pending = lines.pop()
                        for line in lines:
                            if line:
                                self.say(line.decode('utf-8', errors='replace'))
            if pending:
                self.say(pending.decode('utf-8', errors='replace'))
            code = process.wait()
            if code:
                raise SetupError('A setup step failed (exit %s). See the last lines above and the saved log. Retry after correcting the reported problem.' % code)
        finally:
            if process.poll() is None:
                pids = tree_pids(process.pid)
                for pid in pids:
                    try:
                        os.kill(pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    pass
                for pid in pids:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                process.wait()
            process.stdout.close()

    def download(self, url, destination, expected):
        if destination.is_file() and hashlib.sha256(destination.read_bytes()).hexdigest() == expected:
            return
        destination.parent.mkdir(parents=True, exist_ok=True)
        partial = destination.with_suffix('.partial')
        error = None
        for attempt in range(3):
            self.check_cancel()
            digest = hashlib.sha256()
            try:
                self.say('Downloading %s (attempt %s/3)' % (destination.name, attempt + 1))
                request = urllib.request.Request(url, headers={'User-Agent': 'SaintsReborn-Linux-Setup/1'})
                with urllib.request.urlopen(request, timeout=30) as response, partial.open('wb') as out:
                    while True:
                        self.check_cancel()
                        chunk = response.read(256 * 1024)
                        if not chunk:
                            break
                        out.write(chunk)
                        digest.update(chunk)
                if digest.hexdigest() != expected:
                    raise SetupError('The downloaded package manager failed its SHA-256 check. It was not run.')
                partial.chmod(0o755)
                partial.replace(destination)
                return
            except (OSError, urllib.error.URLError) as exc:
                error = exc
                self.say('Download failed: ' + str(exc))
            finally:
                partial.unlink(missing_ok=True)
        raise SetupError('Could not download required programs. Check your internet connection, DNS and proxy settings, then click Install / Update again. ' + str(error))

    def mamba(self, *args):
        return [self.alias / 'micromamba', '--no-rc', '--root-prefix', self.alias / 'packages', *args]

    def inside(self, *args):
        return self.mamba('run', '--prefix', self.alias / 'env', *args)

    def prepare_environment(self):
        self.download(MAMBA_URL, self.work / 'micromamba', MAMBA_SHA256)
        marker = self.work / 'environment-version'
        if not marker.is_file() or marker.read_text() != ENV_VERSION:
            self.say('Downloading Git, Rust, native C/C++ compilers and development libraries. No administrator access is needed.')
            action = 'install' if (self.work / 'env/conda-meta/history').is_file() else 'create'
            self.run(self.mamba(action, '--yes', '--prefix', self.alias / 'env',
                                '--channel', 'conda-forge', '--strict-channel-priority', *PACKAGES))
            atomic_text(marker, ENV_VERSION)
        prefix = self.alias / 'env'
        if not (prefix / 'x86_64-conda-linux-gnu/sysroot/usr/include/stdio.h').is_file():
            raise SetupError('The private Linux build environment is incomplete. Remove its environment-version file and retry.')
        self.env.update({
            'SR_NATIVE_PREFIX': str(prefix),
            'CARGO_HOME': str(self.alias / 'cargo'),
            'CARGO_TARGET_DIR': str(self.alias / 'target'),
            'TMPDIR': str(self.alias / 'tmp'),
            'SR_FORK_REPO_URL': self.repository,
            'SR_FORK_BRANCH': self.branch,
            'CARGO_BUILD_JOBS': str(max(1, min(os.cpu_count() or 1, 4))),
            'LD_LIBRARY_PATH': str(prefix / 'lib') + (':' + self.env['LD_LIBRARY_PATH'] if self.env.get('LD_LIBRARY_PATH') else ''),
        })
        (self.work / 'tmp').mkdir(exist_ok=True)
        self.prepare_llvm_runtime()
        self.validate_native_packages()

    def validate_native_packages(self):
        prefix = self.alias / 'env'
        probe_env = self.env.copy()
        probe_env['PKG_CONFIG_PATH'] = str(prefix / 'lib/pkgconfig') + ':' + str(prefix / 'share/pkgconfig')
        self.say('Checking native recompiler dependencies: X11/XCB and Wayland.')
        self.run(self.inside(prefix / 'bin/pkg-config.bin', '--exists', '--print-errors',
                             'xproto', 'kbproto', 'x11-xcb', 'wayland-client'), env=probe_env)
        required_headers = ('X11/X.h', 'X11/Xproto.h', 'X11/Xlib-xcb.h',
                            'xcb/xcb.h', 'wayland-client.h')
        missing = [name for name in required_headers if not (prefix / 'include' / name).is_file()]
        if missing:
            raise SetupError('Native recompiler development headers are incomplete: ' + ', '.join(missing))

    def prepare_llvm_runtime(self):
        # LLVM 19's linker/manifest tool require the old libxml2.so.2 ABI.
        # Keep it separate so newer Git/ICU dependencies need no downgrade.
        runtime = self.alias / 'llvm-runtime'
        if not (runtime / 'lib/libxml2.so.2').is_file():
            self.say('Downloading the LLVM linker runtime (libxml2.so.2 and its dependencies).')
            action = 'install' if (runtime / 'conda-meta/history').is_file() else 'create'
            self.run(self.mamba(action, '--yes', '--prefix', runtime,
                                '--channel', 'conda-forge', '--strict-channel-priority',
                                'libxml2=2.13.*'))
        if not (runtime / 'lib/libxml2.so.2').is_file():
            raise SetupError('The LLVM runtime download did not provide libxml2.so.2. See the saved log.')
        self.env['LD_LIBRARY_PATH'] = str(runtime / 'lib') + ':' + self.env['LD_LIBRARY_PATH']

    def prepare_core(self):
        src = self.payload / 'core'
        digest = hashlib.sha256((self.repository + '\n' + self.branch + '\n' + ENV_VERSION).encode())
        for path in sorted(src.rglob('*')):
            if path.is_file():
                digest.update(str(path.relative_to(src)).encode())
                digest.update(path.read_bytes())
        key = digest.hexdigest()
        marker = self.work / 'core-version'
        binary = self.alias / 'target/release/SaintsReborn-Setup'
        if not binary.is_file() or not marker.is_file() or marker.read_text() != key:
            self.say('Preparing the updated installer. This is only needed on the first run or when Setup changes.')
            target = self.work / 'core'
            shutil.copytree(src, target, dirs_exist_ok=True)
            self.run(self.inside('cargo', 'build', '--release', '--locked', '--no-default-features'), cwd=self.alias / 'core')
            atomic_text(marker, key)
        return binary

    def prepare_runner(self):
        if is_steamos():
            self.say('SteamOS: using Steam for compatibility. No separate Proton launcher will be downloaded.')
            return
        self.say('Downloading the portable Proton launcher. Proton and its runtime download automatically on first play.')
        archive = self.work / 'umu-launcher-1.4.4.tar'
        self.download(UMU_URL, archive, UMU_SHA256)
        runner = self.work / 'umu/umu-run'
        with tarfile.open(archive) as source:
            candidates = [member for member in source.getmembers()
                          if member.isfile() and Path(member.name).name == 'umu-run']
            if len(candidates) != 1 or candidates[0].size > 5 * 1024**2:
                raise SetupError('The portable Proton launcher archive has an unexpected layout.')
            with source.extractfile(candidates[0]) as content:
                data = content.read()
        runner.parent.mkdir(exist_ok=True)
        runner.write_bytes(data)
        runner.chmod(0o755)
        self.run(self.inside('python', runner, '--help'))

    def integrate(self):
        # Keep the complete launcher with the installation so moving/removing the
        # original Desktop download does not break the update shortcut.
        bundle = self.work / 'launcher'
        bundle.mkdir(exist_ok=True)
        if self.payload.resolve() != bundle.resolve():
            for name in ('setup.py', 'launch.sh', 'LICENSE', 'NOTICE', 'THIRD_PARTY_NOTICES.md'):
                shutil.copy2(self.payload / name, bundle / name)
            shutil.copytree(self.payload / 'core', bundle / 'core', dirs_exist_ok=True)
        updater = bundle / 'launch.sh'
        updater.chmod(0o755)
        apps = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share') / 'applications'
        identity = hashlib.sha256(os.fsencode(self.destination)).hexdigest()[:10]
        command = ' '.join(map(desktop_arg, (updater, '--dir', self.destination, '--update')))
        entry = '[Desktop Entry]\nType=Application\nName=Update Saints Reborn\nExec=%s\nCategories=Game;\nTerminal=false\n' % command
        atomic_text(apps / ('saintsreborn-update-' + identity + '.desktop'), entry)
        self.write_game_launcher(bundle, apps, identity)
        config = {'directory': str(self.destination), 'repository': self.repository, 'branch': self.branch}
        atomic_text(self.work / 'installation.json', json.dumps(config, indent=2))
        self.say('Update shortcut added to your application menu. You can also reopen this Setup file.')

    def write_game_launcher(self, bundle, apps, identity):
        import shlex
        destination = shlex.quote(str(self.destination))
        text = '#!/bin/sh\nset -eu\nexec %s --play --dir %s\n' % (
            shlex.quote(str(bundle / 'launch.sh')), destination)
        launcher = bundle / 'play.sh'
        atomic_text(launcher, text)
        launcher.chmod(0o755)
        if is_steamos():
            command = ' '.join(map(desktop_arg, (bundle / 'launch.sh', '--add-to-steam', '--dir', self.destination)))
            entry = '[Desktop Entry]\nType=Application\nName=Add Saints Reborn to Steam\nExec=%s\nCategories=Game;\nTerminal=false\n' % command
        else:
            entry = '[Desktop Entry]\nType=Application\nName=Play Saints Reborn\nExec=%s\nCategories=Game;\nTerminal=false\n' % desktop_arg(launcher)
        atomic_text(apps / ('saintsreborn-' + identity + '.desktop'), entry)

    def execute(self):
        if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'amd64'):
            raise SetupError('This setup currently supports x86-64 Linux, including Steam Deck.')
        libc, version = platform.libc_ver()
        if libc != 'glibc':
            raise SetupError('This installer needs a glibc Linux system. Alpine/musl needs a glibc container or separate packaging.')
        if tuple(map(int, version.split('.')[:2])) < (2, 28):
            raise SetupError('The portable LLVM toolchain requires glibc 2.28 or newer. Use a newer distro or a compatible container.')
        if not Path('/lib64/ld-linux-x86-64.so.2').exists():
            raise SetupError('The standard Linux executable loader is missing. On NixOS, configure nix-ld or use an FHS container before running this installer.')
        self.destination = validate_destination(self.destination, self.update,
                                               minimum_free=(6 if self.update else 26) * 1024**3)
        if not (self.destination / 'dist/game/default.xex').is_file():
            if not self.source or not self.source.exists():
                raise SetupError('Choose your own Saints Row Xbox 360 ISO or extracted game folder first.')
            if self.source.is_dir() and not (self.source / 'default.xex').is_file():
                raise SetupError('The game folder must contain default.xex and packfiles.')
        self.work = self.destination / '.saintsreborn-setup'
        self.work.mkdir(exist_ok=True)
        self.env['XDG_CACHE_HOME'] = str(self.work / 'cache')
        self.alias = short_alias(self.work)
        with (self.work / 'setup.lock').open('a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise SetupError('Another installer is already using this game folder.') from exc
            with (self.work / 'setup.log').open('a', buffering=1) as log:
                self.log = log
                awake = KeepAwake(self.say, 'Saints Reborn Setup')
                try:
                    self.say('\nSaints Reborn Linux Setup — ' + time.strftime('%Y-%m-%d %H:%M:%S'))
                    self.say('Install folder: ' + str(self.destination))
                    awake.start()
                    self.prepare_environment()
                    binary = self.prepare_core()
                    self.prepare_runner()
                    command = self.inside(binary, '--cli', '--dir', self.destination,
                                          '--accept-license', '--no-integration')
                    if self.update:
                        command.append('--update')
                    if self.source:
                        command += ['--game-dir' if self.source.is_dir() else '--iso', self.source]
                    self.say('Downloading game build tools, preparing your game files and building Saints Reborn.')
                    self.run(command)
                    self.integrate()
                    self.say('Finished. Use Add to Steam, then launch Saints Reborn from Steam.' if is_steamos() else 'Finished. Use Play Saints Reborn in your application menu. Proton and its runtime download automatically on first play.')
                except Exception as exc:
                    self.say(str(exc))
                    raise
                finally:
                    awake.stop()
                    self.log = None


def repair_bundled_mods(directory):
    directory = Path(directory).expanduser().resolve()
    if not existing_install(directory):
        raise SetupError('Choose the existing Saints Reborn installation folder.')
    templates = directory / 'modding/mods'
    if not templates.is_dir():
        raise SetupError('Bundled mod sources are missing. Run the full update first.')
    destination = directory / 'dist/mods'
    destination.mkdir(parents=True, exist_ok=True)
    repaired, skipped = [], []
    for mod in sorted(templates.iterdir()):
        if not mod.is_dir() or mod.is_symlink() or not (mod / 'mod.ini').is_file():
            continue
        native = any(p.suffix.lower() in ('.c', '.cpp') for p in mod.rglob('*'))
        target = destination / mod.name
        if target.is_symlink():
            skipped.append(mod.name + ' (custom linked folder preserved)')
            continue
        if native and not any(target.glob('*.dll')):
            skipped.append(mod.name + ' (needs the full game build)')
            continue
        for source in mod.rglob('*'):
            if not source.is_file() or source.is_symlink():
                continue
            if source.suffix.lower() in ('.c', '.cpp', '.h', '.ps1'):
                continue
            relative = source.relative_to(mod)
            output = target / relative
            if output.exists() or output.is_symlink():
                continue  # Preserve modified settings and existing mod files.
            if any((target / parent).is_symlink() for parent in relative.parents):
                continue
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, output)
        repaired.append(mod.name)
    return ('Bundled mods available: ' + (', '.join(repaired) or 'none') +
            '\n\nClose and reopen the mod loader, then tick the mods you want. Existing settings and mod selections were preserved.' +
            ('\n\nNot installed: ' + ', '.join(skipped) if skipped else ''))


def play(directory):
    if is_steamos():
        raise SetupError('On SteamOS, use Add to Steam and launch Saints Reborn from your Steam Library with an installed Proton version.')
    directory = Path(directory).expanduser().resolve()
    work = directory / '.saintsreborn-setup'
    alias = short_alias(work)
    exe = directory / 'dist/WhompaysModLoader.exe'
    if not exe.is_file():
        exe = directory / 'dist/saintsrow.exe'
    runner = work / 'umu/umu-run'
    interpreter = alias / 'env/bin/python'
    if not exe.is_file() or not runner.is_file() or not interpreter.is_file():
        raise SetupError('Setup has not finished. Open Saints Reborn Install or Update first.')
    env = os.environ.copy()
    env.update({
        'WINEPREFIX': str(work / 'wineprefix'),
        'GAMEID': '0',
        'PROTONPATH': 'GE-Proton',
        'UMU_FOLDERS_PATH': str(work / 'runtime'),
        'XDG_CACHE_HOME': str(work / 'cache'),
        'TMPDIR': str(work / 'tmp'),
        'UMU_ZENITY': '1',
    })
    for path in ('tmp', 'runtime', 'cache'):
        (work / path).mkdir(exist_ok=True)
    with (work / 'play.log').open('a') as log:
        result = subprocess.run([str(interpreter), str(runner), str(exe)], cwd=exe.parent,
                                env=env, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        message = 'The game could not start. See ' + str(work / 'play.log')
        if shutil.which('zenity'):
            subprocess.run(['zenity', '--error', '--text=' + message], check=False)
        raise SetupError(message)
    return result.returncode


def gui(args):
    import tkinter as tk
    from tkinter import filedialog, messagebox, scrolledtext, ttk
    root = tk.Tk()
    root.title('Saints Reborn — Linux Setup & Updater')
    width = min(900, max(600, root.winfo_screenwidth() - 80))
    height = min(700, max(480, root.winfo_screenheight() - 100))
    root.geometry('%sx%s' % (width, height))
    root.minsize(min(660, width), min(550, height))
    root.columnconfigure(0, weight=1)
    root.rowconfigure(0, weight=1)
    # The footer is outside the expanding log area and retains its requested height.
    frame = ttk.Frame(root, padding=(18, 14, 18, 0))
    frame.grid(row=0, column=0, sticky='nsew')
    ttk.Label(frame, text='Saints Reborn', font=('', 22, 'bold')).pack(anchor='w')
    ttk.Label(frame, text='Install or update on this drive or another mounted drive.\nRequired build tools download automatically; no sudo or system changes.').pack(anchor='w', pady=(5, 16))
    initial = args.dir or str(Path.home() / 'Games/SaintsReborn')
    directory = tk.StringVar(value=initial)
    source = tk.StringVar(value=args.iso or args.game_dir or '')
    accepted = tk.BooleanVar(value=args.accept_license)
    update = tk.BooleanVar(value=args.update or existing_install(Path(initial)))
    for title, variable, chooser in (
        ('Install folder', directory, lambda: filedialog.askdirectory(title='Choose the Saints Reborn install folder (any mounted drive)', mustexist=False)),
        ('Your disc image or game folder', source, lambda: filedialog.askopenfilename(title='Choose your Saints Row Xbox 360 ISO', filetypes=[('Xbox disc image', '*.iso'), ('All files', '*')])),
    ):
        ttk.Label(frame, text=title).pack(anchor='w')
        row = ttk.Frame(frame)
        row.pack(fill='x', pady=(3, 10))
        ttk.Entry(row, textvariable=variable).pack(side='left', fill='x', expand=True)
        def browse(var=variable, choose=chooser):
            result = choose()
            if result:
                var.set(result)
        ttk.Button(row, text='Browse…', command=browse).pack(side='left', padx=(8, 0))
        if variable is source:
            def game_folder():
                result = filedialog.askdirectory(title='Choose the folder containing default.xex and packfiles')
                if result:
                    source.set(result)
            ttk.Button(row, text='Game folder…', command=game_folder).pack(side='left', padx=(8, 0))
    ttk.Checkbutton(frame, text='Update an existing installation (no ISO needed when game files are present)', variable=update).pack(anchor='w', pady=(0, 8))
    ttk.Checkbutton(frame, text='I accept the Microsoft C++ headers / libraries license terms.', variable=accepted).pack(anchor='w')
    def license_page():
        import webbrowser
        webbrowser.open('https://visualstudio.microsoft.com/license-terms/')
    ttk.Button(frame, text='Read license terms', command=license_page).pack(anchor='w', pady=(3, 9))
    status = tk.StringVar(value='Choose your game and destination, then click Install / Update. First install can take an hour or more.')
    ttk.Label(frame, textvariable=status, wraplength=740).pack(anchor='w')
    progress = ttk.Progressbar(frame, mode='indeterminate')
    progress.pack(fill='x', pady=8)
    output = scrolledtext.ScrolledText(frame, height=10, wrap='word', state='disabled')
    output.pack(fill='both', expand=True)
    controls = ttk.Frame(root, padding=(18, 10, 18, 14))
    controls.grid(row=1, column=0, sticky='ew')
    controls.columnconfigure(0, weight=1)
    controls.columnconfigure(1, weight=1)
    events = queue.Queue()
    cancel = threading.Event()
    running = False
    engine = None
    def append(text):
        output.configure(state='normal')
        output.insert('end', text + '\n')
        if int(output.index('end-1c').split('.')[0]) > 2200:
            output.delete('1.0', '500.0')
        output.see('end')
        output.configure(state='disabled')
    def worker():
        try:
            engine.execute()
            events.put(('done', 'Installation/update complete. Click Add to Steam.' if is_steamos() else 'Installation/update complete. Use Play Saints Reborn in your application menu.'))
        except Exception as exc:
            events.put(('error', str(exc)))
    def start():
        nonlocal running, engine
        if not accepted.get():
            messagebox.showinfo('License terms', 'Accept the Microsoft C++ headers / libraries license terms before downloading them.')
            return
        if not directory.get().strip():
            messagebox.showerror('Choose a folder', 'Choose a dedicated Saints Reborn install folder.')
            return
        chosen = Path(directory.get()).expanduser()
        is_update = update.get() or existing_install(chosen)
        if is_update and not messagebox.askokcancel('Update Saints Reborn', 'Update this installation?\n\n' + str(chosen) + '\n\nProject source files are refreshed. The installer preserves saves and your mod selection.'):
            return
        engine = Engine(chosen, source.get().strip() or None, is_update,
                        emit=lambda text: events.put(('log', text)), cancel=cancel,
                        repository=args.repository, branch=args.branch)
        cancel.clear()
        running = True
        start_button.configure(state='disabled')
        steam_button.configure(state='disabled')
        stop_button.configure(state='normal')
        progress.start()
        status.set('Preparing installation…')
        threading.Thread(target=worker, daemon=False).start()
    def stop():
        cancel.set()
        status.set('Stopping setup; keeping completed work for retry…')
        stop_button.configure(state='disabled')
    def repair_mods():
        if running:
            messagebox.showinfo('Setup is running', 'Wait for Setup to finish before repairing mods.')
            return
        try:
            messagebox.showinfo('Bundled mods', repair_bundled_mods(directory.get()))
        except (SetupError, OSError) as exc:
            messagebox.showerror('Mod repair stopped', str(exc))
    def send_steam():
        if running:
            return
        steam_button.configure(state='disabled')
        chosen = directory.get()
        def send():
            try:
                events.put(('steam_done', add_to_steam(chosen)))
            except (SetupError, OSError) as exc:
                events.put(('steam_error', str(exc)))
        threading.Thread(target=send, daemon=True).start()
    def refresh_steam(*_):
        folder = Path(directory.get()).expanduser()
        ready = any((folder / 'dist' / name).is_file() for name in ('WhompaysModLoader.exe', 'saintsrow.exe'))
        steam_button.configure(state='normal' if ready and not running else 'disabled')
    def open_log():
        path = (engine.work / 'setup.log') if engine and engine.work else Path(directory.get()) / '.saintsreborn-setup/setup.log'
        if path.is_file():
            subprocess.Popen(['xdg-open', str(path)])
        else:
            messagebox.showinfo('Setup log', 'A log is saved after Setup starts preparing the installation.')
    ttk.Button(controls, text='Repair bundled mods', command=repair_mods).grid(
        row=2, column=0, sticky='ew', padx=(0, 4), pady=(8, 0), ipady=3)
    steam_button = ttk.Button(controls, text='Add to Steam', command=send_steam)
    steam_button.grid(row=2, column=1, sticky='ew', padx=(4, 0), pady=(8, 0), ipady=3)
    directory.trace_add('write', refresh_steam)
    refresh_steam()
    start_button = ttk.Button(controls, text='Install / Update', command=start)
    start_button.grid(row=0, column=0, columnspan=2, sticky='ew', ipady=6, pady=(0, 8))
    stop_button = ttk.Button(controls, text='Stop', command=stop, state='disabled')
    stop_button.grid(row=1, column=1, sticky='ew', padx=(4, 0), ipady=3)
    ttk.Button(controls, text='Open saved log', command=open_log).grid(
        row=1, column=0, sticky='ew', padx=(0, 4), ipady=3)
    def poll():
        nonlocal running
        for _ in range(250):
            try:
                kind, text = events.get_nowait()
            except queue.Empty:
                break
            if kind == 'log':
                append(text)
            elif kind in ('steam_done', 'steam_error'):
                if kind == 'steam_done':
                    messagebox.showinfo('Add to Steam', text)
                else:
                    refresh_steam()
                    messagebox.showerror('Add to Steam', text)
            else:
                running = False
                progress.stop()
                start_button.configure(state='normal')
                stop_button.configure(state='disabled')
                refresh_steam()
                status.set('Ready to retry.' if kind == 'error' else text)
                append(text)
                if kind == 'error':
                    messagebox.showerror('Setup stopped — you can retry', text + '\n\nUse Open saved log for details.')
        root.after(100, poll)
    def close():
        if running:
            messagebox.showinfo('Setup is running', 'Use Stop and wait for Setup to finish stopping before closing this window.')
        else:
            root.destroy()
    root.protocol('WM_DELETE_WINDOW', close)
    poll()
    root.mainloop()


def main():
    parser = argparse.ArgumentParser(description='Saints Reborn Linux installer/updater with automatic private build tools')
    parser.add_argument('--dir')
    parser.add_argument('--iso')
    parser.add_argument('--game-dir')
    parser.add_argument('--update', action='store_true')
    parser.add_argument('--accept-license', action='store_true')
    parser.add_argument('--cli', action='store_true')
    parser.add_argument('--play', action='store_true')
    parser.add_argument('--repair-mods', action='store_true')
    parser.add_argument('--add-to-steam', action='store_true')
    parser.add_argument('--repository', default=DEFAULT_REPO)
    parser.add_argument('--branch', default=DEFAULT_BRANCH)
    args = parser.parse_args()
    # Update launchers preserve their embedded fork destination.
    config = HERE.parent / 'installation.json'
    if config.is_file():
        settings = json.loads(config.read_text())
        args.repository = settings.get('repository', args.repository)
        args.branch = settings.get('branch', args.branch)
    if args.add_to_steam:
        if not args.dir:
            parser.error('--add-to-steam needs --dir')
        try:
            result = add_to_steam(args.dir)
            print(result)
            if not args.cli:
                import tkinter as tk
                from tkinter import messagebox
                root = tk.Tk(); root.withdraw()
                messagebox.showinfo('Add to Steam', result)
                root.destroy()
            return 0
        except (SetupError, OSError) as exc:
            print(str(exc), file=sys.stderr)
            return 1
    if args.repair_mods:
        if not args.dir:
            parser.error('--repair-mods needs --dir')
        try:
            result = repair_bundled_mods(args.dir)
            print(result)
            if not args.cli:
                import tkinter as tk
                from tkinter import messagebox
                root = tk.Tk(); root.withdraw()
                messagebox.showinfo('Saints Reborn bundled mods', result)
                root.destroy()
            return 0
        except (SetupError, OSError) as exc:
            print(str(exc), file=sys.stderr)
            return 1
    if args.play:
        if not args.dir:
            parser.error('--play needs --dir')
        try:
            return play(args.dir)
        except (SetupError, OSError) as exc:
            print(str(exc), file=sys.stderr)
            return 1
    if args.cli:
        if not args.dir or not args.accept_license:
            parser.error('--cli needs --dir and --accept-license')
        try:
            Engine(args.dir, args.iso or args.game_dir,
                   args.update or existing_install(Path(args.dir)),
                   repository=args.repository, branch=args.branch).execute()
        except (SetupError, OSError) as exc:
            print('Setup stopped: ' + str(exc), file=sys.stderr)
            return 1
        return 0
    gui(args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
