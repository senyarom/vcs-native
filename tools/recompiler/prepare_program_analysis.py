#!/usr/bin/env python3
"""Materialize PSPRecomp analysis with a compact VCS code-generation mode."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


HEADER_SHA256 = "d8c0dc190fb159bfd5229b261a79233513869101e1a0f015143bdebe9a915dd0"
SOURCE_SHA256 = "08cdf1da3adb3f5ccf7211e98cfcd21c77db23278bc37863fe85e50eabbb74b7"


def read_verified(path: Path, expected: str) -> str:
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != expected:
        raise RuntimeError(f"Refusing to patch unexpected PSPRecomp source {path}: {digest}")
    return data.decode("utf-8")


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise RuntimeError(f"Expected exactly one patch site, found {text.count(old)}")
    return text.replace(old, new)


def write_if_changed(path: Path, text: str) -> None:
    data = text.encode("utf-8")
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)


def materialize(header_source: Path, implementation_source: Path,
                header_output: Path, implementation_output: Path) -> None:
    header = read_verified(header_source, HEADER_SHA256)
    implementation = read_verified(implementation_source, SOURCE_SHA256)

    implementation = replace_once(
        implementation,
        "#include <unordered_map>\n\nnamespace psprecomp {\nnamespace {\n",
        """#include <unordered_map>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace psprecomp {
namespace {

void release_unused_analysis_pages() noexcept {
#if defined(__APPLE__)
    (void)malloc_zone_pressure_relief(nullptr, 0u);
#elif defined(__GLIBC__)
    (void)malloc_trim(0);
#endif
}
""",
    )
    header = replace_once(
        header,
        """                                              std::uint32_t load_base,
                                              std::size_t max_instructions_per_function = 131072u);
""",
        """                                              std::uint32_t load_base,
                                              std::size_t max_instructions_per_function,
                                              bool retain_function_analyses);
""",
    )
    implementation = replace_once(
        implementation,
        """                                std::uint32_t load_base,
                                std::size_t max_instructions_per_function) {
""",
        """                                std::uint32_t load_base,
                                std::size_t max_instructions_per_function,
                                bool retain_function_analyses) {
""",
    )
    implementation = replace_once(
        implementation,
        "    program.functions.reserve(program.seeds.size());\n\n"
        "    std::unordered_map<std::uint32_t, std::size_t> label_owners;\n",
        "    if (retain_function_analyses) program.functions.reserve(program.seeds.size());\n\n"
        "    std::unordered_map<std::uint32_t, std::size_t> label_owners;\n"
        "    std::size_t function_index = 0u;\n",
    )
    implementation = replace_once(
        implementation,
        """            const auto [it, inserted] = label_owners.emplace(label, program.functions.size());
            if (!inserted && it->second != program.functions.size()) ++program.overlapping_label_count;
""",
        """            const auto [it, inserted] = label_owners.emplace(label, function_index);
            if (!inserted && it->second != function_index) ++program.overlapping_label_count;
""",
    )
    implementation = replace_once(
        implementation,
        "        program.functions.push_back(std::move(function));\n",
        """        if (retain_function_analyses) {
            program.functions.push_back(std::move(function));
        } else {
            function.labels.clear();
            function.entry_labels.clear();
            function.direct_calls.clear();
            function.indirect_call_sites.clear();
        }
        ++function_index;
        if (!retain_function_analyses && (function_index % 32u) == 0u) {
            release_unused_analysis_pages();
        }
""",
    )
    implementation = replace_once(
        implementation,
        "    }\n    return program;\n}\n\n} // namespace psprecomp\n",
        """    }
    if (!retain_function_analyses) release_unused_analysis_pages();
    return program;
}

} // namespace psprecomp
""",
    )

    write_if_changed(header_output, header)
    write_if_changed(implementation_output, implementation)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("header_source", type=Path)
    parser.add_argument("implementation_source", type=Path)
    parser.add_argument("header_output", type=Path)
    parser.add_argument("implementation_output", type=Path)
    args = parser.parse_args()
    materialize(args.header_source, args.implementation_source,
                args.header_output, args.implementation_output)


if __name__ == "__main__":
    main()
