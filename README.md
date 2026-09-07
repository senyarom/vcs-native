# VCSNative

VCSNative is a native port of the US PSP release of **Grand Theft Auto: Vice
City Stories** (`ULUS10160`, version `1.03`). The original Allegrex executable
is translated to C++ ahead of time and runs on a small PSP compatibility
runtime. The renderer, audio, input, save-data support and graphics menu are
implemented on the host.

The current builds target Apple Silicon macOS and x86-64 Linux, including Steam
Deck. They include a Vulkan renderer, higher internal resolutions, HD texture
replacement, 30/60/unlimited frame-rate modes and fixes for several pieces of
game logic that used to run too quickly above 30 FPS. Press **F10** in game to
open the graphics settings.

This repository does not contain a disc image or the game's original assets.
You need your own copy of the US `1.03` PSP release. Other regions and revisions
are not supported.

## Build

Clone the repository and install the host dependencies.

On macOS:

```sh
brew install llvm@21 cmake ninja pkgconf sdl2 ffmpeg libpng \
  vulkan-headers vulkan-loader molten-vk glslang
brew install --cask ppsspp-emulator
```

On Debian or Ubuntu Linux:

```sh
sudo apt install clang cmake ninja-build pkg-config python3 libarchive-tools \
  libsdl2-dev libpng-dev libavcodec-dev libavformat-dev libavutil-dev \
  libswresample-dev libswscale-dev libvulkan-dev glslang-tools
```

Install [PPSSPP](https://www.ppsspp.org/download/) as well, then prepare the
checkout from your ISO:

```sh
cd VCSNative
python3 tools/import_game.py "/path/to/Grand Theft Auto - Vice City Stories.iso"
```

The importer checks the game revision, extracts the files needed at runtime and
uses PPSSPP once to obtain the executable in an isolated configuration. The
temporary decrypted executable is translated to C++ locally and removed
afterwards. The generated code and extracted assets stay outside Git. The
importer then builds the game, runs the tests and prepares the launch directory.

Rebuild later with:

```sh
bash tools/build.sh
```

Run `Start VCS.command` on macOS or `Start VCS.sh` on Linux. Saves and settings
live under `userdata/` and survive rebuilds. The Linux executable is native and
does not need Proton.

To cross-compile for Steam Deck on a Mac:

```sh
brew install llvm@21 lld@21 cmake ninja pkgconf glslang python@3.14
bash tools/build.sh --steamdeck
```

`bash tools/build.sh --steamdeck --deploy` also tests and installs the result in
`~/Downloads/VCSNative` on the host named `steamdeck`.

## Repository layout

- `game/generated/` — local, ignored Allegrex-to-C++ output created by the
  importer from your game copy.
- `runtime/` — the PSP CPU, memory and execution runtime.
- `host/` — operating-system services, input, audio and savedata.
- `graphics/` — renderers and native graphics features.
- `patches/` — game-specific timing and mission fixes.
- `textures/hdtextures/` — the optional HD texture pack.
- `third_party/psprecomp/` — an unchanged, pinned PSPRecomp source snapshot.

## Acknowledgements

VCSNative is an unofficial fan project and is not affiliated with or endorsed
by Rockstar Games or Take-Two Interactive. The original game was developed by
Rockstar Leeds with Rockstar North and published by Rockstar Games. Grand Theft
Auto, Vice City Stories and the original game content belong to their respective
owners. Generated code and locally extracted assets remain subject to the rights
in the original game.

The port is built on
[PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp). The importer uses
[PPSSPP](https://www.ppsspp.org/) to decrypt the user's own executable. Game
timing fixes and the Project2DFX light data are based in part on work by
[ThirteenAG](https://github.com/ThirteenAG/WidescreenFixesPack).

The host code also uses Dear ImGui by Omar Cornut and contributors, xxHash by
Yann Collet, SDL2, FFmpeg, libpng, Vulkan, MoltenVK and glslang. The graphics
work includes SMAA by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando
Navarro and Diego Gutierrez; SimulateHDR/ProperShaders material derived from
work by Jose Negrete (BlueSkyDefender) and its credited contributors; and a
CloudWorks model by Brian Tu (RTU).

HD texture work is credited to DankaishinProductionsModding, TareqGamer,
Parallellines, Djdarko, Ryadica926, SonOfUgly, Solidcal, efonte,
ItzAntonis2012, ASI-Factory, Ilya Kostygov (Till Lindermann) and the Vice Cry
ReBorn Team. File-by-file provenance is recorded in
[the texture sources](textures/hdtextures/SOURCES.md).

Full copyright notices, licenses and pinned upstream revisions are in
[THIRD_PARTY.md](THIRD_PARTY.md). PSPRecomp's MIT license applies to its own
runtime and tools; it does not relicense the game, generated game code or texture
packs.
