# Initial game memory

`initial-memory.bin` is the ULUS10160 1.03 load image after relocation, before
host/profile patches run. `metadata.json` records the original executable hash,
the blob hash, destination, logical size, entry point, GP and heap boundary.
Trailing zeros (including BSS) are omitted from the file and restored at startup.

This directory is imported game data, alongside `game/generated`. The native
build embeds it using `tools/embed_boot_image.py`; no ELF is read or relocated
when the game starts. The original Allegrex instruction bytes in the load image
are retained for guest memory compatibility, but execution uses the generated
native functions. This is not a conversion of guest pointers into C++ objects.

The complete import from a user-owned game image is:

```sh
python3 tools/import_game.py "/path/to/Grand Theft Auto - Vice City Stories.iso"
```

To reproduce only this bootstrap export from an already decrypted ELF, run:

```sh
cmake --build build --target vcs_export_boot_image
build/vcs_export_boot_image assets/game/PSP_GAME/SYSDIR/EBOOT_DECRYPTED.ELF game/bootstrap
```

The exporter rejects other executable revisions. It uses the existing relocation
implementation and is excluded from normal builds. Export once on the build host;
both native and cross-builds then consume the same platform-independent bytes.

`vcs_boot_image_reference_tests` compares the entire RAM/VRAM image, entry point,
GP and heap boundary against the former ELF startup path when the local original
ELF is available. `vcs_boot_image_tests` also runs without any original executable
and checks reinitialization of data/BSS without altering surrounding memory.
