#!/usr/bin/env python3
"""Import a user-owned ULUS10160 1.03 image and prepare a playable build."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
GAME_ID = "ULUS10160"
GAME_VERSION = "1.03"
EXPECTED_ELF_SHA256 = "fee2e86c7fe457ab6da463d9fdc16f156a0900adcdaa2fd260206c11907c5d65"
EXPECTED_ELF_SIZE = 4_611_996
LOAD_BASE = "0x08804000"
GENERATED_UNIT_SPAN = "0x4000"
EXPECTED_GENERATED_UNITS = 234


class ImportFailure(RuntimeError):
    pass


def status(message: str) -> None:
    print(f"==> {message}", flush=True)


def command_text(command: list[str]) -> str:
    return shlex.join(str(item) for item in command)


def run(command: list[str], *, cwd: Path = ROOT, env: dict[str, str] | None = None) -> None:
    print(f"+ {command_text(command)}", flush=True)
    subprocess.run(command, cwd=cwd, env=env, check=True)


def run_quiet(command: list[str], *, cwd: Path = ROOT) -> None:
    print(f"+ {command_text(command)}", flush=True)
    result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode:
        tail = "\n".join(result.stdout.splitlines()[-30:])
        raise ImportFailure(f"Command failed ({result.returncode}):\n{tail}")


def sha256_file(path: Path, length: int | None = None) -> str:
    digest = hashlib.sha256()
    remaining = length
    with path.open("rb") as source:
        while remaining is None or remaining > 0:
            size = 1024 * 1024 if remaining is None else min(1024 * 1024, remaining)
            block = source.read(size)
            if not block:
                break
            digest.update(block)
            if remaining is not None:
                remaining -= len(block)
    if remaining not in (None, 0):
        raise ImportFailure(f"File is shorter than expected: {path}")
    return digest.hexdigest()


def normalize_decrypted_elf(source: Path, destination: Path,
                            expected_hash: str = EXPECTED_ELF_SHA256,
                            expected_size: int = EXPECTED_ELF_SIZE) -> None:
    if not source.is_file():
        raise ImportFailure(f"Decrypted executable does not exist: {source}")
    if source.stat().st_size < expected_size:
        raise ImportFailure(f"Decrypted executable is too short: {source}")
    if sha256_file(source, expected_size) != expected_hash:
        raise ImportFailure(
            f"Unsupported executable revision: the first {expected_size} bytes do not match "
            f"{GAME_ID} {GAME_VERSION}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    remaining = expected_size
    with source.open("rb") as input_file, destination.open("wb") as output_file:
        while remaining:
            block = input_file.read(min(1024 * 1024, remaining))
            if not block:
                raise ImportFailure(f"Short read from {source}")
            output_file.write(block)
            remaining -= len(block)


def read_param_sfo(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) < 20:
        raise ImportFailure(f"PARAM.SFO is truncated: {path}")
    magic, _version, key_table, data_table, count = struct.unpack_from("<4s4I", data)
    if magic != b"\x00PSF" or count > 4096:
        raise ImportFailure(f"Invalid PARAM.SFO: {path}")
    entries_end = 20 + count * 16
    if entries_end > len(data) or key_table > len(data) or data_table > len(data):
        raise ImportFailure(f"Invalid PARAM.SFO table bounds: {path}")
    values: dict[str, object] = {}
    for index in range(count):
        key_offset, value_format, value_length, _value_capacity, value_offset = \
            struct.unpack_from("<HHIII", data, 20 + index * 16)
        key_start = key_table + key_offset
        key_end = data.find(b"\0", key_start)
        value_start = data_table + value_offset
        value_end = value_start + value_length
        if key_start >= len(data) or key_end < key_start or value_end > len(data):
            raise ImportFailure(f"Invalid PARAM.SFO entry: {path}")
        key = data[key_start:key_end].decode("utf-8", "strict")
        raw = data[value_start:value_end]
        if value_format == 0x0404 and len(raw) >= 4:
            values[key] = struct.unpack_from("<I", raw)[0]
        else:
            values[key] = raw.rstrip(b"\0").decode("utf-8", "strict")
    return values


def locate_psp_game(source: Path) -> tuple[Path, Path]:
    source = source.expanduser().resolve()
    if not source.is_dir():
        raise ImportFailure(f"Extracted game directory does not exist: {source}")
    if (source / "PSP_GAME/USRDIR").is_dir():
        return source / "PSP_GAME", source
    if source.name.upper() == "PSP_GAME" and (source / "USRDIR").is_dir():
        return source, source.parent
    if (source / "USRDIR").is_dir() and (source / "PARAM.SFO").is_file():
        return source, source.parent
    raise ImportFailure(f"Cannot find PSP_GAME/USRDIR below {source}")


def copy_extracted_assets(source: Path, destination: Path) -> tuple[Path, Path]:
    psp_game, ppsspp_source = locate_psp_game(source)
    target = destination / "PSP_GAME"
    target.mkdir(parents=True)
    shutil.copytree(psp_game / "USRDIR", target / "USRDIR")
    if not (psp_game / "PARAM.SFO").is_file():
        raise ImportFailure(f"Missing PARAM.SFO in {psp_game}")
    shutil.copy2(psp_game / "PARAM.SFO", target / "PARAM.SFO")
    return psp_game, ppsspp_source


def find_extractor(image: Path) -> list[str]:
    seven_zip = next((path for name in ("7zz", "7z")
                      if (path := shutil.which(name))), None)
    if seven_zip:
        return [seven_zip, "x", "-y"]
    if image.suffix.lower() == ".iso" and (bsdtar := shutil.which("bsdtar")):
        return [bsdtar, "-xf"]
    raise ImportFailure("ISO extraction requires 7z/7zz (or bsdtar for an uncompressed ISO)")


def extract_image_assets(image: Path, destination: Path) -> None:
    image = image.expanduser().resolve()
    if not image.is_file() or image.suffix.lower() not in (".iso", ".cso"):
        raise ImportFailure("The game input must be an ISO/CSO file or an extracted game directory")
    command = find_extractor(image)
    if Path(command[0]).name in ("7z", "7zz"):
        command += [f"-o{destination}", str(image), "PSP_GAME/USRDIR/*", "PSP_GAME/PARAM.SFO"]
    else:
        destination.mkdir(parents=True, exist_ok=True)
        command += [str(image), "-C", str(destination),
                    "PSP_GAME/USRDIR", "PSP_GAME/PARAM.SFO"]
    run_quiet(command)


def validate_game_identity(game_root: Path) -> None:
    values = read_param_sfo(game_root / "PSP_GAME/PARAM.SFO")
    disc_id = values.get("DISC_ID")
    version = values.get("DISC_VERSION")
    if disc_id != GAME_ID or version != GAME_VERSION:
        raise ImportFailure(
            f"Unsupported game: DISC_ID={disc_id!r}, DISC_VERSION={version!r}; "
            f"expected {GAME_ID} {GAME_VERSION}")


def validate_runtime_assets(game_root: Path) -> int:
    manifest_path = ROOT / "tools/import_data/ulus10160-1.03-assets.json"
    manifest = json.loads(manifest_path.read_text())
    entries = manifest["files"]
    if not entries:
        raise ImportFailure(f"No runtime asset checksums in {manifest_path}")
    for entry in entries:
        relative = entry["path"]
        path = game_root / relative
        if not path.is_file():
            raise ImportFailure(f"Game asset is missing: {relative}")
        digest = sha256_file(path)
        if digest != entry["sha256"]:
            raise ImportFailure(f"Game asset belongs to another revision: {relative}")
    return len(entries)


def resolve_ppsspp(override: str | None) -> Path:
    candidates: list[Path] = []
    if override:
        candidates.append(Path(override).expanduser())
    for name in ("PPSSPPSDL", "PPSSPPQt", "ppsspp", "PPSSPPHeadless"):
        if executable := shutil.which(name):
            candidates.append(Path(executable))
    if sys.platform == "darwin":
        candidates += [
            Path("/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL"),
            Path("/Applications/PPSSPP.app/Contents/MacOS/PPSSPP"),
        ]
    for candidate in candidates:
        candidate = candidate.resolve()
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise ImportFailure(
        "A decrypted ELF was not supplied and PPSSPP was not found. Install PPSSPP "
        "or pass --decrypted-eboot <path>.")


def dump_decrypted_elf(ppsspp_input: Path, workspace: Path, override: str | None,
                       timeout: float = 60.0) -> Path:
    executable = resolve_ppsspp(override)
    isolated_home = workspace / "ppsspp-home"
    isolated_home.mkdir(parents=True)
    append_config = workspace / "ppsspp-dump.ini"
    append_config.write_text(
        "[General]\nFirstRun = False\nAutoRun = True\nDumpFileTypes = 1\n"
        "CheckForNewVersion = False\nDiscordRichPresence = False\n"
        "[Sound]\nEnable = False\n")
    log_path = workspace / "ppsspp-dump.log"
    environment = os.environ.copy()
    environment["HOME"] = str(isolated_home)
    environment["XDG_CONFIG_HOME"] = str(workspace / "ppsspp-xdg")
    command = [str(executable), "--windowed", "--xres", "480", "--yres", "272",
               "--graphics=software", f"--appendconfig={append_config}", str(ppsspp_input)]
    status("Decrypting EBOOT with an isolated PPSSPP configuration")
    print(f"+ {command_text(command)}", flush=True)
    with log_path.open("wb") as log:
        process = subprocess.Popen(command, cwd=ROOT, env=environment,
                                   stdout=log, stderr=subprocess.STDOUT)
        selected: Path | None = None
        previous: tuple[Path, int, int] | None = None
        stable_samples = 0
        deadline = time.monotonic() + timeout
        try:
            while time.monotonic() < deadline:
                dumps = sorted(workspace.rglob("*_EBOOT.BIN"),
                               key=lambda path: (GAME_ID not in path.name, path.name))
                if dumps:
                    candidate = dumps[0]
                    details = (candidate, candidate.stat().st_size,
                               candidate.stat().st_mtime_ns)
                    stable_samples = stable_samples + 1 if details == previous else 0
                    previous = details
                    if details[1] >= EXPECTED_ELF_SIZE and stable_samples >= 2:
                        selected = candidate
                        break
                if process.poll() is not None:
                    selected = dumps[0] if dumps else None
                    break
                time.sleep(0.2)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    if selected and selected.is_file():
        return selected
    tail = "\n".join(log_path.read_text(errors="replace").splitlines()[-30:])
    raise ImportFailure(f"PPSSPP did not dump the decrypted EBOOT within {timeout:g}s.\n{tail}")


def find_existing_decrypted(psp_game: Path) -> Path | None:
    for name in ("EBOOT_DECRYPTED.ELF", "EBOOT_DECRYPTED.BIN"):
        candidate = psp_game / "SYSDIR" / name
        if candidate.is_file():
            return candidate
    return None


def configure_import_tools(build_dir: Path, jobs: int) -> tuple[Path, Path]:
    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja")
    if not cmake or not ninja:
        raise ImportFailure("Building the importer requires cmake and ninja")
    run([cmake, "-S", str(ROOT), "-B", str(build_dir), "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DVCS_IMPORT_ONLY=ON",
         "-DVCS_BUILD_RECOMPILER=ON", "-DPSPRECOMP_BUILD_TESTS=OFF",
         "-DPSPRECOMP_BUILD_PROFILE_TESTS=OFF", "-DPSPRECOMP_ENABLE_SANITIZERS=OFF"])
    run([cmake, "--build", str(build_dir), "--target", "vcs_recomp",
         "vcs_export_boot_image", "--parallel", str(jobs)])
    suffix = ".exe" if os.name == "nt" else ""
    recompiler = build_dir / f"tools/recompiler/vcs_recomp{suffix}"
    exporter = build_dir / f"vcs_export_boot_image{suffix}"
    if not recompiler.is_file() or not exporter.is_file():
        raise ImportFailure("CMake completed without producing the import tools")
    return recompiler, exporter


def validate_generated(directory: Path) -> None:
    report_path = directory / "auto_codegen_report.json"
    registry = directory / "generated_registry.cpp"
    if not report_path.is_file() or not registry.is_file():
        raise ImportFailure("Recompiler did not produce its report and registry")
    report = json.loads(report_path.read_text())
    units = sorted(directory.glob("generated_unit_*.cpp"))
    if (report.get("mode") != "automatic_global_cfg"
            or report.get("unit_span_bytes") != int(GENERATED_UNIT_SPAN, 0)
            or report.get("translation_units") != EXPECTED_GENERATED_UNITS
            or len(units) != EXPECTED_GENERATED_UNITS):
        raise ImportFailure(f"Unexpected generated-code report: {report_path}")


def validate_bootstrap(directory: Path) -> None:
    metadata = json.loads((directory / "metadata.json").read_text())
    blob = directory / "initial-memory.bin"
    if (metadata.get("source_sha256") != EXPECTED_ELF_SHA256
            or metadata.get("image_sha256") != sha256_file(blob)
            or metadata.get("data_size") != blob.stat().st_size):
        raise ImportFailure("Bootstrap exporter produced inconsistent metadata")


def remove_path(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


def install_outputs(assets: Path, generated: Path, bootstrap: Path,
                    workspace: Path) -> None:
    backup_root = workspace / "backups"
    operations: list[tuple[Path, Path | None]] = []

    def install(staged: Path, destination: Path, label: str) -> None:
        backup: Path | None = None
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists() or destination.is_symlink():
            backup = backup_root / label
            backup.parent.mkdir(parents=True, exist_ok=True)
            os.replace(destination, backup)
        try:
            os.replace(staged, destination)
        except Exception:
            if backup is not None:
                os.replace(backup, destination)
            raise
        operations.append((destination, backup))

    try:
        install(assets, ROOT / "assets/game", "assets-game")
        install(generated, ROOT / "game/generated", "generated")
        install(bootstrap / "initial-memory.bin",
                ROOT / "game/bootstrap/initial-memory.bin", "initial-memory.bin")
        install(bootstrap / "metadata.json",
                ROOT / "game/bootstrap/metadata.json", "bootstrap-metadata.json")
    except Exception:
        for destination, backup in reversed(operations):
            remove_path(destination)
            if backup is not None:
                os.replace(backup, destination)
        raise


def ensure_replace_allowed(force: bool) -> None:
    populated = []
    if (ROOT / "assets/game").exists():
        populated.append("assets/game")
    if (ROOT / "game/bootstrap/initial-memory.bin").exists():
        populated.append("game/bootstrap")
    if populated and not force:
        joined = ", ".join(populated)
        raise ImportFailure(f"Already initialized: {joined}. Use --force to regenerate them.")


def import_game(args: argparse.Namespace) -> None:
    if os.name == "nt":
        raise ImportFailure("This importer is currently supported on macOS and Linux")
    ensure_replace_allowed(args.force)
    source = args.game.expanduser().resolve()
    if not source.exists():
        raise ImportFailure(f"Game input does not exist: {source}")
    (ROOT / "work").mkdir(exist_ok=True)
    workspace = Path(tempfile.mkdtemp(prefix="import-game-", dir=ROOT / "work"))
    succeeded = False
    try:
        staged_assets = workspace / "assets-game"
        status("Extracting runtime game assets")
        if source.is_dir():
            original_psp_game, ppsspp_input = copy_extracted_assets(source, staged_assets)
        else:
            extract_image_assets(source, staged_assets)
            original_psp_game = None
            ppsspp_input = source
        validate_game_identity(staged_assets)
        status("Verifying the exact supported asset revision")
        asset_count = validate_runtime_assets(staged_assets)
        (staged_assets / "import.json").write_text(json.dumps({
            "format_version": 1,
            "game": f"{GAME_ID} {GAME_VERSION}",
            "source_executable_sha256": EXPECTED_ELF_SHA256,
            "runtime_asset_files_verified": asset_count,
        }, indent=2) + "\n")

        supplied_elf = args.decrypted_eboot.expanduser().resolve() \
            if args.decrypted_eboot else None
        if supplied_elf is None and original_psp_game is not None:
            supplied_elf = find_existing_decrypted(original_psp_game)
        if supplied_elf is None:
            supplied_elf = dump_decrypted_elf(ppsspp_input, workspace, args.ppsspp)
        decrypted_elf = workspace / "EBOOT_DECRYPTED.ELF"
        normalize_decrypted_elf(supplied_elf, decrypted_elf)
        status(f"Verified decrypted executable: {EXPECTED_ELF_SHA256}")

        recompiler, exporter = configure_import_tools(
            ROOT / "work/import-tools-build", args.jobs)
        staged_generated = workspace / "generated"
        staged_bootstrap = workspace / "bootstrap"
        status("Generating native C++ translation units from the game executable")
        run([str(recompiler), str(decrypted_elf), "--auto", str(staged_generated),
             LOAD_BASE, GENERATED_UNIT_SPAN])
        validate_generated(staged_generated)
        status("Exporting the relocated initial memory image")
        run([str(exporter), str(decrypted_elf), str(staged_bootstrap)])
        validate_bootstrap(staged_bootstrap)

        status("Installing verified local game outputs")
        install_outputs(staged_assets, staged_generated, staged_bootstrap, workspace)
        succeeded = True
        if args.no_build:
            status("Import complete; run: bash tools/build.sh")
        else:
            status("Building, testing, and preparing the launcher")
            run(["bash", str(ROOT / "tools/build.sh")])
            status("VCSNative is ready")
    finally:
        if args.keep_work:
            print(f"Import workspace kept at {workspace}", flush=True)
        else:
            shutil.rmtree(workspace, ignore_errors=True)
        if not succeeded:
            print("No verified import was installed.", file=sys.stderr, flush=True)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path,
                        help="ULUS10160 1.03 ISO/CSO or extracted game directory")
    parser.add_argument("--decrypted-eboot", type=Path,
                        help="use an existing decrypted ELF instead of PPSSPP")
    parser.add_argument("--ppsspp", help="path to the PPSSPP executable")
    parser.add_argument("--force", action="store_true",
                        help="replace an existing local game import")
    parser.add_argument("--no-build", action="store_true",
                        help="prepare the local game data, but do not build VCSNative")
    parser.add_argument("--keep-work", action="store_true",
                        help="keep the temporary import workspace for diagnostics")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 4))
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    return args


def main(argv: list[str] | None = None) -> int:
    try:
        import_game(parse_args(argv))
        return 0
    except (ImportFailure, OSError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print(f"import_game: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
