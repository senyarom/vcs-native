#!/usr/bin/env python3
"""Materialize the VCS code generator with local memory-use integration fixes."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


UPSTREAM_SHA256 = "452bc182e0ac648d4384ca140c4ec1f3f8671a417357c8ada9d093cbef131b0d"


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise RuntimeError(f"Expected exactly one patch site, found {text.count(old)}")
    return text.replace(old, new)


def materialize(source: Path, destination: Path) -> None:
    source_bytes = source.read_bytes()
    digest = hashlib.sha256(source_bytes).hexdigest()
    if digest != UPSTREAM_SHA256:
        raise RuntimeError(
            f"Refusing to patch unexpected PSPRecomp source {source}: {digest}")

    text = source_bytes.decode("utf-8")
    text = replace_once(
        text,
        "#include <vector>\n\nnamespace {\n",
        """#include <vector>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace {

void release_unused_heap_pages() noexcept {
#if defined(__APPLE__)
    (void)malloc_zone_pressure_relief(nullptr, 0u);
#elif defined(__GLIBC__)
    (void)malloc_trim(0);
#endif
}
""",
    )
    text = replace_once(
        text,
        "    const auto program = psprecomp::analyze_program(elf, memory, load_base);\n",
        "    auto program = psprecomp::analyze_program(elf, memory, load_base, 131072u, false);\n",
    )
    text = replace_once(
        text,
        """                        if (target_is_import) {
                            body << "    ctx.pc = " << psprecomp::hex32(target) << "u;\\n"
                                 << "    return;\\n";
                            continue;
                        }
""",
        """                        if (target_is_import) {
                            body << "    ctx.pc = " << psprecomp::hex32(target) << "u;\\n"
                                 << "    return;\\n";
                            break;
                        }
""",
    )
    text = replace_once(
        text,
        """    if (unit_span_bytes != 0u && target >= executable_base) {
""",
        """    if (unit_span_bytes != 0u && target >= executable_base &&
        direct_entry_ids != nullptr && direct_entry_ids->contains(target)) {
""",
    )
    text = replace_once(
        text,
        """                        const bool direct_unit = function.unit_span_bytes != 0u &&
                            target >= function.executable_base;
""",
        """                        const bool direct_unit = function.unit_span_bytes != 0u &&
                            target >= function.executable_base &&
                            function.direct_entry_ids != nullptr &&
                            function.direct_entry_ids->contains(target);
""",
    )
    text = replace_once(
        text,
        """        std::ostringstream out;
        out << "#include \\"psprecomp/runtime.hpp\\"\\n#include \\"generated_units.hpp\\"\\n#include <bit>\\n#include <cmath>\\n#include <cstdint>\\n#include <limits>\\n\\nnamespace psprecomp {\\n";
""",
        """        std::cerr << "[codegen] unit " << suffix_text
                  << " begin instructions=" << generated_unit.instructions.size()
                  << " entries=" << generated_unit.entry_labels.size() << "\\n" << std::flush;
        std::ostringstream out;
        out << "#include \\"psprecomp/runtime.hpp\\"\\n#include \\"generated_units.hpp\\"\\n#include <bit>\\n#include <cmath>\\n#include <cstdint>\\n#include <limits>\\n\\nnamespace psprecomp {\\n";
""",
    )
    marker = """    for (const auto &unit : units) {
        std::uint16_t id = 1u;
        for (const auto label : unit.entries) direct_entry_ids[label] = id++;
    }

"""
    release = marker + """    // Unit ownership is complete. Function analyses contain heavily overlapping
    // label sets and are no longer needed while source text is emitted. Release
    // them here so one large unit cannot push the importer over its memory limit.
    const std::size_t discovered_function_seed_count = program.seeds.size();
    const std::size_t emitted_instruction_count = program.covered_labels.size();
    program.functions.clear();
    program.functions.shrink_to_fit();
    program.covered_labels.clear();
    program.covered_entry_labels.clear();
    program.seeds.clear();
    program.executable_ranges.clear();
    release_unused_heap_pages();

"""
    text = replace_once(text, marker, release)
    text = replace_once(
        text,
        '           << "  \\"discovered_function_seeds\\": " << program.seeds.size() << ",\\n"\n',
        '           << "  \\"discovered_function_seeds\\": " << discovered_function_seed_count << ",\\n"\n',
    )
    text = replace_once(
        text,
        '           << "  \\"unique_emitted_instructions\\": " << program.covered_labels.size() << ",\\n"\n',
        '           << "  \\"unique_emitted_instructions\\": " << emitted_instruction_count << ",\\n"\n',
    )
    text = replace_once(
        text,
        '              << "  function seeds:    " << program.seeds.size() << "\\n"\n',
        '              << "  function seeds:    " << discovered_function_seed_count << "\\n"\n',
    )
    text = replace_once(
        text,
        '              << "  emitted code PCs:  " << program.covered_labels.size() << "\\n"\n',
        '              << "  emitted code PCs:  " << emitted_instruction_count << "\\n"\n',
    )

    destination.parent.mkdir(parents=True, exist_ok=True)
    encoded = text.encode("utf-8")
    if destination.is_file() and destination.read_bytes() == encoded:
        return
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    temporary.write_bytes(encoded)
    temporary.replace(destination)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    materialize(args.source, args.destination)


if __name__ == "__main__":
    main()
