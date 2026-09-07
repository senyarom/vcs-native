# Project context

- This is the working repository for VCSNative (GTA Vice City Stories PSP USA, ULUS10160 1.03). Read README.md for the layout and current limitations.
- game/generated/ is the imported Allegrex-to-C++ corpus. Keep handwritten mission fixes in patches/, rendering and graphical extensions in graphics/, and host integration in host/. Regeneration is a separate deliberate operation.
- runtime/ contains only execution dependencies. Analysis/code generation belongs in tools/recompiler/ and is not linked into the game.
- textures/hdtextures/ is the active combined HD pack. Preserve image bytes, alpha, relative manifest paths, source attribution and catalog metadata when reorganizing it.
- Preserve the user's assets/game/ dump, userdata/ settings and saves. Build and packaging scripts must not overwrite existing settings or depend on the old Codex workspace.
- Before a diagnostic game launch, check for an existing VCSNative process. Run at most one test game, stop only the process started for the test, and use bounded automatic shutdown. Prefer no window and dummy audio for automated checks.
- On Apple Silicon use verified LLVM Clang 21 with the macOS SDK. Keep AOT optimization enabled; do not mask compiler faults with -O0.
- Build with tools/build.sh and run relevant existing CTest targets. The script runs the full current suite and prepares run/ after a successful Release build. Sanitizer builds do not replace the Release launcher.
- For Steam Deck from macOS, use tools/build.sh --steamdeck (native Clang/LLD 21, Linux sysroot, -O3 on all units). This builds Linux tests without running them on macOS. Run tools/test_steamdeck.py --smoke for target verification; it tests and publishes directly in ~/Downloads/VCSNative, preserves userdata and keeps run/VCSNative.previous. Do not create temporary package directories on Deck. Never feed a cross-build to prepare_run.py on macOS.
- The public game-asset checksum manifest is in tools/import_data/. tools/verify_import.py verifies the local user-owned asset import against it.
- Historical mission tests establish specific firing/timing behavior, not a successful full mission playthrough. Linux x86-64 on Steam Deck passed the 22-test suite and a headless gameplay smoke test. Visible Linux controller/presentation checks, Windows migration builds and complete game compatibility remain unverified.
