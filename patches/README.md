# Game patches

Handwritten corrections to original game behavior live here. Keep them out of
the generated game and the reusable PSP runtime. Hooks, regression tests,
supported executable versions and attribution are kept with the corrections.

Current layers:

| Layer | Location |
| --- | --- |
| Recompiled game, generated from Allegrex | `game/generated/` |
| Reusable PSP execution and memory runtime | `runtime/include/psprecomp/`, `runtime/src/` |
| Game corrections | `patches/` |
| Native host, PSP services and game integration | `host/vcs_profile.cpp`, other host adapters |
| Graphics extensions and settings | `graphics/`, including `graphics/shaders/` |

The VCS patch target depends only on `psprecomp_core`. Host integration supplies
the selected frame rate. It does not depend on SDL, Vulkan, DirectX or the UI.
The standalone repository keeps the patch module independent of the renderer.

## Mission timing and NPC firing fixes

Target: **ULUS10160 1.03**, load base `0x08804000`, executable SHA-256
`fee2e86c7fe457ab6da463d9fdc16f156a0900adcdaa2fd260206c11907c5d65`.
Addresses require revalidation for another executable.

## NPC damage: fix the emitter, preserve health and hit damage

`weapon_timing.cpp` corrects the two simplified NPC shooting paths whose calls
return to `0x089126DC` and `0x08912804`. While a burst timer is active, they call
`CWeapon::Fire` (`0x08A45338`) once **per game frame**. The original path does not
wait for an animation firing marker. At 60 FPS it consumes a round and emits a
bullet every frame, twice the original game's emission rate.

The replacement allows these instant-hit firearms to emit at their original
30 Hz maximum cadence in **simulation time**. Time advances once per completed
game frame, using `CTimer::ms_fTimeStep / 50`, not once per PSP VBlank and not
from the requested FPS. Fractional time is retained within a burst. Idle time
cannot accumulate a backlog of shots. Pausing does not advance the cooldown;
clock reset/new game clears it. Each owner/weapon pair has separate state.

An extra call returns before the original `Fire` prologue, so it cannot consume
ammo, produce a trace, or inflict damage. An allowed call preserves the original
prologue and resumes unmodified AOT. Player and animation-driven firing callers,
melee, area effects, explosions and per-hit damage are not scaled by this fix.

**Health compensation was removed.** The truck retains its original 7000 max and
current health, and Lance's original script health coefficient remains 11.
Changing 30/60/uncapped does not rewrite initial or live health. No pre-mission
FPS selection, health multiplier or manual switch is needed for this firing fix.

## Script timing corrections

`mission_fps.cpp` adapts the float corrections and concert fallback from
[ThirteenAG's GTAVCS.PPSSPP.WidescreenFix](https://github.com/ThirteenAG/WidescreenFixesPack/blob/2b7b0686213320478a9574cf27c19187d8a36f81/source/GTAVCS.PPSSPP.WidescreenFix/main.c#L492),
commit `2b7b0686213320478a9574cf27c19187d8a36f81` (MIT; see
[third_party/ThirteenAG-LICENSE.txt](third_party/ThirteenAG-LICENSE.txt)).
The upstream truck/Lance health boosts are deliberately not applied.

| Mission | Script correction above 30 FPS |
| --- | --- |
| Boomshine Blowout | Halve one float |
| The Exchange | Halve one float |
| Hose the Hoes | Halve three floats |
| Balls | Halve one float |
| Farewell to Arms | Halve the plane movement float; health stays original |
| In the Air Tonight | Halve one float; original limiter while on a mission and `REN7_O9` has nonempty text |

These fixed-60 script corrections impose a temporary ceiling of 60 when a
higher/uncapped rate is requested; the concert guard can use 30. The configured
preference is retained and restored when the script ends. Selecting 30 restores
original float operands. As upstream does, the concert's temporary limiter keeps
the 60-FPS float operand until the user explicitly selects 30.

Only the actual loaded mission bytes in guest RAM are scanned. MAIN.SCM on disk,
saves and generated C++ are not changed. Ambiguous signatures, invalid bounds,
external writers and replacement script allocations are handled conservatively.

`mission_fps_hooks.cpp` applies the script corrections at the real cross-unit
`StartNewScript` call (`0x08863470`, loader return `0x08ABC138`), after reading the
exact mission length and before its first opcode. It preserves the original
prologue and resumes AOT at `0x088626C0`. Local AOT labels cannot substitute for
this hook, because local gotos bypass runtime dispatch. The limiter override
at `0x08A070C8` preserves its delay-slot load. CText lookup is bounded/read-only.

The `vcs_patches` target depends only on `psprecomp_core`; rendering stays separate.

## Verification

Swimming stamina is corrected separately in `swimming_timing.cpp`. It normalizes
the velocity sample after water drag to the original 30 Hz interval without
changing physical movement or per-hit damage. Actual AOT regressions cover
30/60/120/240 Hz; the first mission's accelerated swimming encounter was replayed
at 30 and 60. These new checks were run in Release on Mac and as a native Linux
x86-64 AOT test on Deck; they are not part of the historical sanitizer claim
below.

Five regression targets pass in Release and **ASan + UBSan**:

- Mission bytes, transitions, ownership, bounds and concert text lookup.
- Original AOT loader, script initialization and limiter continuations.
- All 99 local mission scripts: eight changed float operands in six missions;
  all health operands stay original; exact restoration at 30 FPS.
- NPC cadence at 30/60/90/120/144/240/360 FPS: 300 shots in ten simulated seconds;
  live transitions, short bursts, separate actors, pause and reset.
- Actual NPC AOT cross-unit call reaches the hook; allowed calls reproduce every
  prologue register and memory write; rejected calls change no guest memory;
  other firing callers bypass the gate.

`Farewell to Arms` was also launched in isolated game roots, with normal mission
code and ordinary enemies. The only test-data edit selects mission 93 instead
of mission 8 at new-game startup. No health, immunity or mission-success cheats
were used. The gunship/truck combat section ran at 30 and 60, plus live
60 → 30 → 60 → uncapped changes. Median consecutive NPC shot intervals were
33.366 ms in simulation time in both modes after the fix. Before it, 60-FPS
shots occurred every game frame (16 ms in the game's truncated integer clock).
AK hits still inflicted 20 damage, and the truck started at 7000 HP.

The unattended runs intentionally provided no defensive aiming/shooting and
ended in mission failure. They verify the firing defect and live switching in
combat, **not a successful full mission playthrough**. Different runs have
varying vehicle paths, attackers and hit counts; their total health loss is not
an identical-scene damage benchmark. Other FPS-dependent physics, all weapon
classes and a full Light My Pyre playthrough are not covered by this evidence.

Opt-in diagnostics: `PSPRECOMP_DAMAGE_PROBE=1` logs firing attempts, accepted
shots and vehicle damage inputs. `PSPRECOMP_DISABLE_NPC_FIRE_FIX=1` provides a
baseline for comparisons. Neither variable is enabled by the normal launcher.
