# Building engine-unreal1 (WebAssembly + native)

The **private engine repo** for the original **Unreal (1998), Unreal Engine 1 v200** web port — engine
source only. Game assets (Unreal v200: maps/textures/music/sounds + the `.u`/`.int` packages) live **out of
tree** and are never tracked here; no build reads them. Base import:
[fgsfdsfgs/UE1](https://github.com/fgsfdsfgs/UE1) @ `41cf09a` (kept as the `upstream` remote).

The public monorepo `ufront` ([10-p/unreal-frontend-arch-private](https://github.com/10-p/unreal-frontend-arch-private)) consumes this repo's WASM artifact, owns the host
page / UI / glue / assembly, and injects the heavy glue behind the thin ABI shim. **Migration state lives
in the monorepo** (`docs/migration/STATE.md`), not here.

## Build matrix (CMake presets — `Source/CMakePresets.json`)

| Preset | Output | Purpose |
|--------|--------|---------|
| `wasm` | `build-wasm/SDLLaunch/SDLLaunch.{js,wasm,data}` + `meta.json` | Emscripten **dev** (assertions + DWARF) |
| `wasm-perf` | `build-wasm-perf/SDLLaunch/…` | Emscripten **product** (`-O3`, no debug) — what `ufront` consumes |
| `native` | `build-native/Unreal/Unreal.bin` | 32-bit native GLES/software twin (needs a 32-bit SDL2; see README) |
| `native64` | `build-native64/Unreal/Unreal.bin` | 64-bit (LP64) x86_64 twin (needs the 64-bit SDL2) — § *64-bit* |
| `native64-asan` | `build-native64-asan/` | `native64` + AddressSanitizer |
| `windows` | `build-windows/Unreal/Unreal.exe` | 32-bit **Windows** binary via MinGW-w64 (§ *Windows builds*) |

```bash
source <emsdk>/emsdk_env.sh            # emsdk 5.0.7; sets $EMSDK for the toolchain file
cmake --preset wasm-perf && cmake --build --preset wasm-perf -j"$(nproc)"   # the web build
cmake --preset native    && cmake --build --preset native    -j"$(nproc)"   # the native twin
cmake --preset windows   && cmake --build --preset windows   -j"$(nproc)"   # the Windows binary
```

⚠ Presets live in `Source/`, so run `cmake --preset …` from `Source/`, not the repo root.

The web build emits a **bare loader** (`SUFFIX .js`, no `--shell-file`) + `meta.json` (`engineFamily:UE1`,
`abiVersion:1`, `loaderStyle:global-module`); `ufront`'s `packages/build` validates it. Assets: only
`System/Default.ini` is preloaded into MEMFS; everything else streams at runtime via the monorepo's FetchFS.

### `meta.json` records what was fetched (ufront 2.70)

`toolchain` names every repository FetchContent pulled, as `<repo>@<branch|tag>` — never a commit: the
project-owned repositories (`10-p/*`) are consumed at their branch tip so a rebuild takes the latest of
each, and an external upstream (`libxmp/libxmp@libxmp-4.7.1`) stays on its release tag. The values come from
the same cache variables the `FetchContent_Declare` read (`Source/cmake/ArtifactDeps.cmake`), so the
descriptor cannot drift from the fetch; a local checkout override (`FETCHCONTENT_SOURCE_DIR_<NAME>`,
`LIBXMP_SOURCE_DIR`, `GL4ES_SOURCE_DIR`) shows up as `local:<path>`. ⚠ A branch ref is re-fetched on every
**configure** (`cmake --preset …`), never by a plain `cmake --build`: an old build directory rebuilt without
re-configuring still holds the dependency it fetched last time.

### openal-soft on native Linux (ufront 2.70)

The native binary no longer links the distro's `libopenal`: `Source/cmake/OpenALSoft.cmake` (the 2.61 web
module, generalised — `ufront_openal_soft(PLATFORM WEB|NATIVE …)`) FetchContent's the same fork the web uses
(`10-p/openal-soft-web` `ufront/web`) and builds it SHARED for the `native` preset. The launcher links it
(`DT_NEEDED libopenal.so.1`), carries `$ORIGIN` in its rpath and copies `libopenal.so.1` beside itself, so
**a run tree that takes the binary must take `libopenal.so.1` with it** (the loader stops before `main`
otherwise). Backends are whatever this host's headers allowed at configure time — e.g. PulseAudio, ALSA,
OSS, each dlopen'd at run time — and the configure log says which
(`Building OpenAL with support for the following backends`). `-DUT_BUNDLED_OPENAL=OFF` restores the system
library. Why: the system library is whatever the distro built, so the native binary carries the fork to play
what the web plays.

## The web port (what it took — the portable UE1→web lessons, `docs/10`)

A baked `EMSCRIPTEN` branch in `Source/CMakeLists.txt` forces the web-safe config; the per-file changes:

- **Static single binary** — `BUILD_STATIC` (`--whole-archive`, preserves the package registrants), like
  the PSVita target. Emscripten can't load UE1's `.so`/`.dll` packages.
- **Renderers** — **`NOpenGLESDrv` (GLES2 → WebGL, the default)** + **`SoftDrv` (software)**. `-SOFTWARE`
  selects the software rasterizer (the Advanced video-renderer picker). GLES2 loads via
  `gladLoadGLES2Loader(SDL_GL_GetProcAddress)` unchanged under Emscripten; `SoftDrv` uses its portable C
  path (`ASM=0` — its `__asm` is MSVC-only; the upstream `softdrv: gcc/non-x86` fix).
- **Main-loop inversion** (`Unreal/Src/SDLLaunch.cpp`) — the blocking `while(){Tick();appSleep();}` yields
  each frame with `emscripten_sleep` (Asyncify), so the browser event loop runs. **Not**
  `emscripten_set_main_loop` (its unwind collides with UE1 `main()`'s `try/catch`).
- **`appBaseDir()`** (`Core/Src/UnPlat.cpp`) — use `getcwd()` on Emscripten (not `SDL_GetBasePath()`, which
  returns `/`). The host chdir's to `/System` before `main`, so config + package `Paths` resolve like a
  native install.
- **Exceptions off** — rely on Emscripten's default (catching disabled). `-fexceptions` overflows V8's call
  stack via `invoke_` trampolines through UE1's deep package loader (`UnrealI.u` ~37 MB); `-fwasm-exceptions`
  is incompatible with Asyncify. Normal boot never throws; error paths abort loudly.
- **`WebRTCDrv`** (new, web-only) — a UE1 `UNetDriver`/`UNetConnection` over the glue `webrtc_*` ABI (the
  vendored `vendor/glue/webrtc-shim.js`). Auto-switched from `IpDrv.TcpNetDriver` on Emscripten. `Browse`
  promotes the web `<host>?connect` URL's map token to `Host` so UE1 takes the network path. `ue_exec_command`
  (ADR-10) runs a console command and returns captured output.
- **Console logging** — under `__EMSCRIPTEN__`, `FGlobalPlatform::WriteBinary` (`Core/Src/UnPlat.cpp`, the
  `GLog` sink) also echoes each non-suppressed log line to `stdout` (`EventName: Data\n`), which the host's
  `Module.print` forwards to `console.log`. UE1 otherwise logs only to the MEMFS `Unreal.log`; forwarding to
  the browser console (same `RF_Suppress` filter → console == `Unreal.log`) lets the ufront E2E harness read
  engine milestones straight off the console, matching engine-ut2004 (no `FS.readFile` hooks).
- **Editor package linked (step 2.6f)** — `BUILD_EDITOR` is ON for Emscripten (the stripped, no-GUI
  `NullEdHook` Editor, linked into the static binary). Some game content — notably bot/character
  meshes+skins — loads through classes in the Editor package; without it, spawning DM bots fails. (Compile
  fix it needed: `UnScrCom.cpp` `Min(1024, ptrdiff)` → `Min<INT>(…)`, a clang template-deduction nit.)
- **Exception-safe content loading (step 2.6f)** — exception CATCHING is disabled in the web build
  (`-fexceptions` overflows V8's stack on `UnrealI.u`'s deep recursive loader; `-fwasm-exceptions` is
  incompatible with ASYNCIFY), so a thrown load error can't unwind across Asyncify frames — it escapes every
  `guard`/`unguard` catch and aborts the module. UE1's error model is exception-based, so recoverable
  load failures (e.g. a bot mesh referencing an absent/`None` skin texture) became hard aborts. Fix (mirrors
  engine-ut2004): `Core/Src/UnObj.cpp` — `FObjectManager::SafeLoadError` does not re-throw on `LOAD_Throw`,
  and `LoadObject` returns NULL on object-not-found — both under `#ifdef __EMSCRIPTEN__`, resolving the load
  to NULL (exactly what the native local-catch path yields) instead of aborting. Additional throw sites are
  patched **one by one** as they surface (the `[appThrowf]` console line names each). Verified: `DmDeck16` +
  `UnrealI.DeathMatchGame` runs a 4-bot deathmatch (Ash/Kurgan/Rhiannon/Dante) rendering via WebGL, no abort.
- **Graceful map-load failure (step 2.6g)** — extends 2.6f to the missing/invalid-map path (which aborted
  elsewhere): `GetPackageLinker` returns NULL under `#ifdef __EMSCRIPTEN__` for the file-resolution throws
  (`PackageResolveFailed`/`PackageNotFound`/`FileNotFound`, `UnObj.cpp`); `LoadMap` NULL-checks the
  `VerifyPackages` result (the dead catch's job) and returns NULL; and `UGameEngine::Init` (`UnGame.cpp`)
  falls back to the Entry/menu (`GLevel = GEntry`) instead of `appErrorf(FailedBrowse)`. So a bad
  `?map=…` fails soft (`"… failed to load … falling back to the menu"`) and spawns in the menu, no abort.
  Mirrors engine-ut2004's map-load fix.
- **Small POSIX fixes** (`docs/10` §7, guarded): `pthread_setname_np` no-op; IpDrv socket headers
  (`<sys/select.h>`/`<sys/time.h>`, `u_long`, musl `h_errno`-vs-`errno`).

## 64-bit (LP64) — ufront 2.84

Unreal runs as a 64-bit engine on Linux x86_64 (and still on every 32-bit target) with the **retail 32-bit `.u`
packages unchanged**. engine-ut99 did it first (2.83); 2.84 walked its list of changes one by one. The record,
the list and the evidence are in the monorepo: `docs/migration/steps/2.84-64bit-unreal1.md`; the portable
lessons in `docs/10` §10.

**What makes it work** (each is a commit on `main`; general, not `#if`-guarded):

- **Script object references are 4-byte object indices** in memory (`FObjectManager::GetScriptObject`), so the
  bytecode keeps the layout the 32-bit compiler gave it: no jump, label or saved-state offset moves. Every operand is
  read by copy (`ReadScriptOperand<T>`), and the readers are `ALWAYS_INLINE` (the VM files are `-Og`):
  `execLocalVariable` is a load, a compare and a table load at both widths.
- **The mirror contract:** every header declaring a scripted intrinsic class is `#pragma pack(push,4)`; object
  properties align to 4; the script `int`s that stand in for C++ data (the object header, secondary vtables,
  private pointers) and v200's `DynamicArray`/`DynamicString` stand-ins take their C++ size in `LinkOffsets`. A
  C++/script size mismatch is **fatal** (`[layout]`, then `appErrorf`) — v200 had no guard before 2.84.
- **v200 stores layout values in its packages**, so they are recomputed on load and written in the 32-bit script
  format on save: a function's `ParmsSize`/`ReturnValueOffset`, a database class's `ClassRecordSize` (the level's
  actor list, the BSP surfaces). The compiler writes the script format too (`ScriptFormatElementSize/StructSize/
  Offset`, Core), so a 64-bit `-MAKE` writes the bytes a 32-bit one does but for the GUID.
- **A call lays each parameter where the callee's linked property reads it** — v200 summed `EX_BeginFunction`'s
  compiled (32-bit) spans.
- **A None context clears its whole destination** — it fills only the compiled (32-bit) size, so every reader of
  one zeroes the rest to the value's size in memory (`ZeroNullContextTail()`, CorePrivate.h: `ExecLet`, a struct
  member, a struct compare); a scratch buffer an object is read from starts zeroed, `P_GET_OBJECT`/`P_GET_ACTOR`
  start NULL; an object `switch` compares whole pointers.
- `PTRINT`/`UPTRINT` wherever a pointer passes through an integer; the autoregister chain in a pointer-sized field;
  no `long` where 32 bits are meant; no `FString` object through `...`; SoftDrv's lit palette after its struct.

**Fixed on the way — on every width, the web's included:** four members GCC laid out differently from the script
(`EditorEngine.AutosaveTimeMinutes`, `IceTexture.MoveIce`, `TcpLink.URL`/`LinkState` — 46 members read at the wrong
offset); `Player` was 12 bytes short of its C++ class (`DynamicString`); a byte out-parameter's copy-back wrote its
caller's neighbouring bytes. The test pass (ASan, player sessions, bot matches) then found seven more, none of
them 64-bit: `IntToByte` and a temporary struct's member were returned as pointers into a returned frame; a string
constant passed as a parameter was copied at the parameter's full size past the function's `Script`; the music
thread was spawned detached and joined; a viewport outlived its client at exit; `rmode 1` stopped every GCC
SoftDrv game (its depth-cue table stayed locked); a dedicated server stopped on a NaN rocket ("moved without proper
hashing", NaN != NaN).

**Test loops** (headless; `Xvfb :98`, loopback only):

```bash
tools/dev/mkrun.sh build-native64 /home/ion/native-runtrees/unreal-64      # + build-native64/Unreal/libopenal.so.1
cd /home/ion/native-runtrees/unreal-64/System
gdb -batch -x <repo>/tools/dev/layout-sweep.gdb --args ./Unreal.bin -MAKE             # 1035/1 (EditorEngine.Pad), structs 10/0
DISPLAY=:98 gdb -batch -x <repo>/tools/dev/layout-sweep.gdb --args ./Unreal.bin -SOFTWARERENDERER   # 925/0, 10/0
./Unreal.bin "DmDeck16?game=U64Test.U64Game" -server                              # U64 DONE passed=<n> failed=0
<repo>/tools/dev/botmatch.sh <server-tree> 300 [<client-tree>]                     # bMultiPlayerBots/InitialBots in the ini
# ASan: build-native64-asan (preset native64-asan), a tree of its own, and for every run
ASAN_OPTIONS=alloc_dealloc_mismatch=0:detect_leaks=0 <repo>/tools/dev/botmatch.sh <asan-tree> 240
# player sessions, every device (SoftDrv, NOpenGLES, NOpenGL), under ASan too:
unshare -rn <repo>/tools/dev/census/play.sh <tree> -NOPENGLESRENDERER "DmDeck16?game=UnrealI.DeathMatchGame" /tmp/p \
    "stat global" "rmode 1" "rmode 5" shot "savegame 5" "open ?load=5" "open DmAriza"
```

**Coverage census here:** the build must keep the preset's `-DASM=0` —
`cmake --preset native64 -B ../build-native64-cov "-DCMAKE_C_FLAGS=-DASM=0 --coverage" "-DCMAKE_CXX_FLAGS=-DASM=0
--coverage" "-DCMAKE_EXE_LINKER_FLAGS=--coverage /tmp/gcovdump.o" -DCMAKE_SHARED_LINKER_FLAGS=--coverage`. The
counters land in the build directory, so any number of trees running that binary add to one census. 2.84: 2,297 of
3,376 functions ran at x86_64 (fixture, bot match, `-MAKE`, the three devices, a menu walk, downloads); the rest are
UnrealEd, SoftDrv's 15/16-bit/MMX variants and code read for LP64 defects (none open).

v200 harness facts: the log goes **only** to `System/<exe>.log` (`LOG=<file>` names it; two processes in one tree
need two names); the console stays open after Return; a `-*RENDERER` switch is saved in the ini as the player's
choice (`botmatch.sh` restores the trees' ini files); an `appError` leaves the process alive, waiting on SDL's
message box (`play.sh` stops at a `Critical:` line).

`tools/test/U64Test` is compiled by the **32-bit** `Unreal.bin -MAKE` (`EditPackages[n]=U64Test` in Unreal.ini, the
folder linked beside `System`); a 64-bit `-MAKE` of it must match but for the GUID. **Run the layout sweep in both
hosts** — a reflected layout can exist in only one process. **Coverage census:** `tools/dev/census/` (engine-ut99's
BUILD.md "Coverage census" has the recipe; the binary here is `Unreal.bin`). A run tree's `Save` and `Cache` are its
own (`mkrun.sh`); never write into `$UNREAL_ASSETS_DIR`.

## Windows builds (MinGW-w64) — ufront step 2.33

**Nearly free here**, because upstream (fgsfdsfgs/UE1) already did the GCC/Windows work: this tree carries
the unified `PLATFORM_WIN32`-aware platform layer (`Core/Src/UnPlat.cpp`, `UnFile.cpp`, `UnThread.cpp`)
rather than UE1's original `UnUnix.cpp`/`UnVcWin32.cpp` split. Every engine library cross-compiled clean
with **zero source changes**; only three build-side things were missing.

```bash
cd Source
cmake --preset windows && cmake --build --preset windows -j"$(nproc)"   # -> build-windows/Unreal/Unreal.exe
```

⚠ **32-bit only.** `Source/CMakeLists.txt` demotes an x86_64 compiler to `-m32` and `FATAL_ERROR`s if it
cannot; upstream's README says the same ("**Do not** use the `MINGW64` or `MSYS` prompts"). The toolchain
file targets `i686-w64-mingw32`.

**Cross-compiling (Linux) vs MSYS2 (Windows).** `Source/cmake/toolchain-mingw-w64.cmake` supplies only
what a *cross* build cannot infer — the compilers and a find-root. It is the same MinGW-w64 GCC that
MSYS2's `mingw32` environment ships, so from an MSYS2 `MINGW32` prompt the preset works with pacman's
`mingw-w64-i686-toolchain` / `mingw-w64-i686-SDL2` and the SDL2 fetch below is skipped.

What the port actually needed:

- **SDL2** — a cross build has no system SDL2, so `Source/cmake/SDL2Mingw.cmake` FetchContents SDL2's
  official prebuilt MinGW binaries (hash-pinned, 2.32.10). ⚠ Link order: `-lmingw32 -lSDL2main` **before**
  `-lSDL2` — SDL.h renames the launcher's `main()` to `SDL_main` and SDL2main provides the `WinMain` that
  calls it, which is why the launcher needs no source change.
- **`-static-libgcc -static-libstdc++`** — otherwise the `.exe` imports `libgcc_s_dw2-1.dll` and
  `libstdc++-6.dll`, which exist on the build machine and nowhere else. With them the only imports left
  are stock system DLLs plus `OpenAL32.dll` (copied beside the exe since ufront 2.34), so ship the two together.
- **`-lwinpthread` statically** — a posix-thread-model MinGW (MSYS2, Arch) otherwise adds `libwinpthread-1.dll`.
- **`Unreal/Src/Res/LaunchRes.rc`** — its icon path (`..\res\Unreal.ico`) only ever resolved because
  Windows filesystems are case-insensitive; cross-compiling from Linux it is a hard `windres` error.
- **`SoftDrv/Src/SoftDrvPrivate.h`** — the Win32 headers define `IN`/`OUT` as empty annotation macros,
  which deletes DrawSurf.cpp's local `IN`. Undefined rather than renamed, so the Linux/web builds stay
  textually identical (verified: the web wasm sha256 is unchanged).

`BUILD_STATIC=ON` means `appGetDllHandle` resolves through the internal registry, so there is **no
`LoadLibrary`, no `Core.dll`/`Engine.dll`** — the same mechanism the web and PSVita builds use.

**Verifying without Windows.** `wine` + `Xvfb` (needs `wine32`; the renderers below are CPU-only, so no
GPU is involved):

```bash
Xvfb :99 -screen 0 1024x768x24 &
export WINEDLLOVERRIDES="mscoree,mshtml="   # a new prefix otherwise waits on the Mono/Gecko installer dialogs
DISPLAY=:99 WINEPREFIX=<prefix> wine Unreal.exe -SOFTWARERENDERER   # or -NULLRENDERER
WINEPREFIX=<prefix> wineserver -k          # ⚠ always; wine leaves processes behind
```

Assert on **milestones in `System/Unreal.log`** (`Game class is 'Intro'`, `Opened viewport`,
`Init: Game engine initialized`), not on a window appearing. Two traps when building the run tree: copy
the `.exe` in (the engine derives its base dir from the real binary path), and dereference asset symlinks
(`cp -rL`) or every package silently becomes a `FileNotFound`. ⚠ wine is not Windows — this is a
regression net, not a substitute for a real-hardware pass.

**Out of scope for now:** hardware renderers on Windows (`NOpenGLESDrv`), and `-mwindows` means no console
— read the log file.

## GL4ES / future renderers

Unlike engine-ut2004, this repo does **not** use GL4ES — UE1 ships a native GLES2 driver. A more
feature-complete hardware renderer (OldUnreal/XOpenGLDrv with an ES target, or cwdohnal/utglr + GL4ES —
each backported to v200) is a documented future option, not in the current build.
