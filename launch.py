#!/usr/bin/env python3
"""Launch the native game with this checkout's assets and persistent settings."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent


def main():
    game = Path(sys.argv[1]).expanduser().resolve() if len(sys.argv) > 1 else ROOT / 'assets/game'
    binary = ROOT / ('run/VCSNative.app/Contents/MacOS/VCSNative' if sys.platform == 'darwin' else 'run/VCSNative')
    if not binary.is_file():
        raise SystemExit('Сначала собери игру: bash tools/build.sh')
    if not (game / 'PSP_GAME/USRDIR').is_dir():
        raise SystemExit('Нужны игровые ресурсы PSP_GAME/USRDIR в assets/game/.')
    # Hold a lock across exec for launches from this checkout. Also catch an
    # already-running old build or an app opened directly from Finder.
    import fcntl
    lock = os.open(ROOT / 'userdata/.launch.lock', os.O_CREAT | os.O_RDWR, 0o600)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise SystemExit('VCSNative уже запущена.')
    active = subprocess.run(['pgrep', '-x', 'VCSNative'], capture_output=True, text=True)
    if active.returncode == 0:
        raise SystemExit('VCSNative уже запущена; второй экземпляр не открыт.')
    os.set_inheritable(lock, True)
    if sys.platform.startswith('linux'):
        # Match the 64 MiB host stack reserved by our macOS/Windows binaries.
        # Generated AOT call chains can exceed Linux's usual 8 MiB soft limit.
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
        desired = 64 * 1024 * 1024
        if hard != resource.RLIM_INFINITY:
            desired = min(desired, hard)
        if soft != resource.RLIM_INFINITY and soft < desired:
            resource.setrlimit(resource.RLIMIT_STACK, (desired, hard))
    os.chdir(ROOT / 'userdata')
    os.execv(str(binary), [str(binary), '--game-root', str(game)])


if __name__ == '__main__':
    main()
