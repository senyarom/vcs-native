# Component notices

This checkout preserves the original component notices:

- PSPRecomp execution runtime and host adapters: MIT, `runtime/LICENSE`. Imported from `https://github.com/jessicanataliagta/PSPRecomp`, commit `f6e7d415c7f447b934cc3865a31eb725f353d659`, with local changes.
- PSPRecomp recompiler sources: unmodified vendored snapshot of the same commit, `third_party/psprecomp/LICENSE`; provenance and file hashes are in `third_party/psprecomp/VENDORED.md`. The local build adapter is `tools/recompiler/CMakeLists.txt`.
- PPSSPP is used as an external, one-time import tool to decrypt the executable from the user's own game image. PPSSPP is not distributed with this repository.
- Dear ImGui: MIT, `third_party/imgui/LICENSE.txt`.
- xxHash: BSD-2-Clause, `third_party/xxhash/LICENSE`.
- Mission script fixes adapted from ThirteenAG: MIT, `patches/third_party/ThirteenAG-LICENSE.txt`; details in `patches/README.md`.
- HD images: authors and provenance in `textures/hdtextures/SOURCES.md`, `catalog.csv` and `Sources/InstalledHD/`. The PSPRecomp MIT license does not relicense these images or game code.

- FFmpeg runtime libraries and import libraries: LGPL 2.1 or later. The bundled notice is `third_party/ffmpeg/COPYING.LGPLv2.1`.
- SMAA shader resources: see `third_party/smaa/LICENSE.txt`.
- Project2DFX-derived VCS LOD-light data/behavior reference by ThirteenAG: MIT. See `third_party/project2dfx/LICENSE.txt` and `third_party/project2dfx/ATTRIBUTION.md`.
- HDR shader material under `graphics/shaders/hdr`: see `graphics/shaders/hdr/LICENSE_SIMULATEHDR.txt`.
- CloudWorks Alpha 4.0 volumetric-cloud density/noise model by Brian Tu (RTU):
  CC BY-NC-SA 3.0. See `third_party/cloudworks/ATTRIBUTION.md`.

Original runtime assets extracted from the user's disc are kept in
`assets/game/` and excluded from Git. The decrypted executable is temporary and
is not retained after import. Generated game code is kept separately in
`game/generated/` and is not described as independently authored PSPRecomp
framework code.
