# PSPRecomp recompiler snapshot

This directory contains an unmodified subset of PSPRecomp sources vendored from:

- Repository: `https://github.com/jessicanataliagta/PSPRecomp`
- Commit: `f6e7d415c7f447b934cc3865a31eb725f353d659`
- License: MIT; see `LICENSE`

Only the analysis and code-generation sources needed by this project are
included. Their upstream directory layout is preserved. Project-specific build
integration remains outside the snapshot in `tools/recompiler/CMakeLists.txt`.

## Included files

| Upstream path | SHA-256 |
| --- | --- |
| `tools/analyzer_main.cpp` | `389b7a13f1c62784c0e8c12a4aed213e6446cb70861098e1b9ef0fd36c6da516` |
| `tools/codegen_main.cpp` | `4ec34d8c832cef28122bbc524428d2409d04ddf6615a0bd7033f8904a88f7ee8` |
| `tools/dump_function.cpp` | `fe444745472037ad61dec2e4924822b8028168dd87b4c2210afe495fbb5c2eda` |
| `src/decoder.cpp` | `9fb603a5798be2f88334cf2d9338d1d3a678f9560a1107465d85cea67d3d5845` |
| `src/program_analysis.cpp` | `08cdf1da3adb3f5ccf7211e98cfcd21c77db23278bc37863fe85e50eabbb74b7` |
| `include/psprecomp/codegen_policy.hpp` | `56bef5b5aca276657ccc139b744a691cddd355fbbb87633026ff14f705bb12b5` |
| `include/psprecomp/decoder.hpp` | `24791c6e012464d5b1f7ed26ff715cc22c51e4b8e8ce3447ace4ae57cb6f1a92` |
| `include/psprecomp/program_analysis.hpp` | `d8c0dc190fb159bfd5229b261a79233513869101e1a0f015143bdebe9a915dd0` |
| `profiles/vcs/tools/vcs_codegen_main.cpp` | `452bc182e0ac648d4384ca140c4ec1f3f8671a417357c8ada9d093cbef131b0d` |

The MIT license covers these PSPRecomp sources. It does not license the
generated game code, the original game assets, or the HD texture pack.
