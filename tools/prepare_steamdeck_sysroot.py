#!/usr/bin/env python3
"""Extract hash-pinned Debian amd64 development packages; no install scripts run."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'cmake/toolchains/steamdeck-sysroot.lock.json'


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prepare(destination):
    destination = destination.resolve()
    lock_hash = sha256(LOCK)
    stamp = destination / '.vcs-sysroot.json'
    if stamp.exists() and json.loads(stamp.read_text()).get('lock_sha256') == lock_hash:
        return destination
    if destination.exists():
        raise RuntimeError(f'Sysroot exists with a different/incomplete lock: {destination}')
    packages = json.loads(LOCK.read_text())['packages']
    cache = destination.parent / 'packages'
    cache.mkdir(parents=True, exist_ok=True)

    def download(package):
        path = cache / (package['sha256'] + '.deb')
        if path.exists() and sha256(path) == package['sha256']:
            return path
        temporary = path.with_suffix('.part')
        print(f"Download {package['name']} {package['version']}", flush=True)
        with urllib.request.urlopen(package['url'], timeout=120) as response, temporary.open('wb') as out:
            shutil.copyfileobj(response, out)
        if temporary.stat().st_size != package['size'] or sha256(temporary) != package['sha256']:
            temporary.unlink()
            raise RuntimeError(f"Package checksum mismatch: {package['name']}")
        temporary.replace(path)
        return path

    with ThreadPoolExecutor(max_workers=8) as pool:
        archives = list(pool.map(download, packages))
    staging = destination.with_name(destination.name + '.partial')
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    for package, archive in zip(packages, archives):
        members = subprocess.check_output(['ar', 't', str(archive)], text=True).splitlines()
        member = next(name for name in members if name.startswith('data.tar.'))
        payload = subprocess.check_output(['ar', 'p', str(archive), member])
        if member.endswith('.zst'):
            payload = subprocess.run(['zstd', '-d', '-c'], input=payload, check=True, capture_output=True).stdout
        with tarfile.open(fileobj=io.BytesIO(payload)) as tar:
            for info in tar:
                name = PurePosixPath(info.name)
                if name.is_absolute() or '..' in name.parts:
                    raise RuntimeError(f'Unsafe package path: {info.name}')
                # Absolute Linux symlinks must stay inside the extracted sysroot.
                if info.issym() and info.linkname.startswith('/'):
                    info.linkname = os.path.relpath(staging / info.linkname.lstrip('/'), (staging / info.name).parent)
                tar.extract(info, staging, filter='data')
    for name in ('lib', 'lib64'):
        path = staging / name
        if not path.exists() and not path.is_symlink():
            path.symlink_to('usr/' + name, target_is_directory=True)
    (staging / '.vcs-sysroot.json').write_text(json.dumps({
        'lock_sha256': lock_hash, 'packages': len(packages), 'target': 'x86_64-linux-gnu'
    }, indent=2) + '\n')
    staging.replace(destination)
    print(f'Ready: {destination}', flush=True)
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, default=ROOT / 'work/toolchains/steamdeck/sysroot')
    prepare(parser.parse_args().destination)
