#!/usr/bin/env python3
"""Unit tests for the user-owned game import pipeline."""

import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools/import_game.py"
SPEC = importlib.util.spec_from_file_location("vcs_import_game", SCRIPT)
assert SPEC and SPEC.loader
IMPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IMPORT)


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CODEGEN_PATCH = load_module(
    "vcs_codegen_patch", IMPORT.ROOT / "tools/recompiler/prepare_vcs_codegen.py")
ANALYSIS_PATCH = load_module(
    "vcs_analysis_patch", IMPORT.ROOT / "tools/recompiler/prepare_program_analysis.py")


def make_sfo(values: dict[str, str]) -> bytes:
    keys = bytearray()
    payload = bytearray()
    entries = []
    for key, value in values.items():
        key_offset = len(keys)
        keys += key.encode() + b"\0"
        while len(payload) % 4:
            payload += b"\0"
        value_offset = len(payload)
        encoded = value.encode() + b"\0"
        payload += encoded
        entries.append((key_offset, 0x0204, len(encoded), len(encoded), value_offset))
    key_table = 20 + len(entries) * 16
    data_table = key_table + len(keys)
    while data_table % 4:
        keys += b"\0"
        data_table += 1
    header = struct.pack("<4s4I", b"\0PSF", 0x00000101,
                         key_table, data_table, len(entries))
    return header + b"".join(struct.pack("<HHIII", *entry) for entry in entries) \
        + keys + payload


class ImportGameTests(unittest.TestCase):
    def test_reads_identity_from_param_sfo(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "PARAM.SFO"
            path.write_bytes(make_sfo({"DISC_ID": "ULUS10160", "DISC_VERSION": "1.03"}))
            self.assertEqual(IMPORT.read_param_sfo(path)["DISC_ID"], "ULUS10160")
            self.assertEqual(IMPORT.read_param_sfo(path)["DISC_VERSION"], "1.03")

    def test_locates_both_extracted_directory_shapes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "PSP_GAME/USRDIR").mkdir(parents=True)
            expected = (root / "PSP_GAME").resolve()
            self.assertEqual(IMPORT.locate_psp_game(root)[0], expected)
            self.assertEqual(IMPORT.locate_psp_game(root / "PSP_GAME")[0], expected)

    def test_normalizes_ppsspp_trailing_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload = b"\x7fELF" + bytes(range(32))
            source = root / "dump.bin"
            target = root / "game.elf"
            source.write_bytes(payload + b"PPSSPP trailing allocation bytes")
            IMPORT.normalize_decrypted_elf(
                source, target, hashlib.sha256(payload).hexdigest(), len(payload))
            self.assertEqual(target.read_bytes(), payload)

    def test_rejects_unexpected_executable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "wrong.bin"
            source.write_bytes(b"wrong revision")
            with self.assertRaises(IMPORT.ImportFailure):
                IMPORT.normalize_decrypted_elf(source, root / "out", "0" * 64, 5)

    def test_validates_codegen_shape(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "generated_registry.cpp").write_text("// registry\n")
            for index in range(IMPORT.EXPECTED_GENERATED_UNITS):
                (root / f"generated_unit_{index:04d}.cpp").write_text("// unit\n")
            (root / "auto_codegen_report.json").write_text(json.dumps({
                "mode": "automatic_global_cfg",
                "unit_span_bytes": int(IMPORT.GENERATED_UNIT_SPAN, 0),
                "translation_units": IMPORT.EXPECTED_GENERATED_UNITS,
            }))
            IMPORT.validate_generated(root)

    def test_local_generated_code_does_not_block_first_import(self):
        with tempfile.TemporaryDirectory() as temporary:
            original_root = IMPORT.ROOT
            try:
                IMPORT.ROOT = Path(temporary)
                (IMPORT.ROOT / "game/generated").mkdir(parents=True)
                (IMPORT.ROOT / "game/generated/generated_registry.cpp").write_text("// local\n")
                IMPORT.ensure_replace_allowed(False)

                (IMPORT.ROOT / "assets/game").mkdir(parents=True)
                with self.assertRaises(IMPORT.ImportFailure):
                    IMPORT.ensure_replace_allowed(False)
            finally:
                IMPORT.ROOT = original_root

    def test_codegen_adapter_fixes_import_loop_and_invalid_direct_targets(self):
        source = IMPORT.ROOT / \
            "third_party/psprecomp/profiles/vcs/tools/vcs_codegen_main.cpp"
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "vcs_codegen_main.cpp"
            CODEGEN_PATCH.materialize(source, output)
            text = output.read_text()
            self.assertIn("131072u, false", text)
            self.assertIn("target_is_import", text)
            self.assertIn('<< "    return;\\n";\n                            break;', text)
            self.assertIn("direct_entry_ids->contains(target)", text)
            self.assertIn("function.direct_entry_ids->contains(target)", text)

    def test_analysis_adapter_does_not_retain_per_function_results(self):
        header = IMPORT.ROOT / "third_party/psprecomp/include/psprecomp/program_analysis.hpp"
        source = IMPORT.ROOT / "third_party/psprecomp/src/program_analysis.cpp"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output_header = root / "include/psprecomp/program_analysis.hpp"
            output_source = root / "src/program_analysis.cpp"
            ANALYSIS_PATCH.materialize(header, source, output_header, output_source)
            header_text = output_header.read_text()
            source_text = output_source.read_text()
            self.assertIn("bool retain_function_analyses", header_text)
            self.assertIn("if (retain_function_analyses) {", source_text)
            self.assertIn("function.labels.clear();", source_text)
            self.assertIn("release_unused_analysis_pages();", source_text)


if __name__ == "__main__":
    unittest.main()
