# Building engine-unreal1

The engine source of Unreal (1998), Unreal Engine 1 v200, for the browser and for native Linux and Windows. The
web build produces a WebAssembly artifact that the frontend repository
([10-p/unreal-frontend-arch-private](https://github.com/10-p/unreal-frontend-arch-private)) serves; game assets
(an Unreal v200 install) live outside this repository and no build reads them.

## Presets

All presets are in `Source/CMakePresets.json`; run `cmake --preset …` from `Source/`. Build trees go to the repo
root (`build-<preset>/`, gitignored).

| Preset | Output | What it is |
|---|---|---|
| `native` | `build-native/Unreal/Unreal.bin` + `libopenal.so.1` | 32-bit Linux, one static binary (needs a 32-bit SDL2) |
| `native64` | `build-native64/Unreal/…` | the same at x86_64 (LP64) |
| `native64-asan` | `build-native64-asan/Unreal/…` | `native64` with AddressSanitizer |
| `windows` | `build-windows/Unreal/Unreal.exe` + `OpenAL32.dll` | 32-bit Windows, MinGW-w64 (cross or MSYS2) |
| `wasm` | `build-wasm/SDLLaunch/` | web, the development build: -O2 with DWARF and assertions |
| `wasm-debug` | `build-wasm-debug/SDLLaunch/` | web at -O0, for a bug the optimiser is suspected of |
| `wasm-perf` | `build-wasm-perf/SDLLaunch/` | web, the shipping build: -O2 compile, -O3 link, no debug info, no assertions |

```bash
source <emsdk>/emsdk_env.sh                                                  # emsdk 5.0.7; sets $EMSDK
cmake --preset wasm-perf && cmake --build --preset wasm-perf -j"$(nproc)"   # the web build
cmake --preset native    && cmake --build --preset native    -j"$(nproc)"
cmake --preset windows   && cmake --build --preset windows   -j"$(nproc)"
```

Every preset is `BUILD_STATIC`: all packages link into one executable (`--whole-archive`, `UNREAL_STATIC`), and
`appGetDllHandle` resolves a package through the internal export table (`Source/Core/Src/UnStaticExports.cpp`) instead of
`dlopen`/`LoadLibrary`. Every preset builds the stripped, GUI-less Editor package, which game content needs at run
time, and SoftDrv and NullDrv beside the GL renderers.

## CMake options

| Option | Default | Meaning |
|---|---|---|
| `UE_WEB_PERF` | OFF | web: the shipping link (-O3 -g0, no assertions, a symbol map beside the loader) |
| `UE_WEB_DEBUG` | OFF | web: compile at -O0 instead of -O2 |
| `UE_BUNDLED_OPENAL` | ON | native Linux: build openal-soft and ship `libopenal.so.1` beside the binary instead of linking the system libopenal |
| `UE_ALLOW_64BIT` | OFF | build x86_64 as a 64-bit engine; OFF demotes an x86_64 compiler to -m32 (an arm64 compiler always builds 64-bit; 64-bit MSVC is refused) |
| `BUILD_STATIC` | OFF (ON in every preset) | one binary |
| `BUILD_EDITOR` | ON | the Editor package |
| `BUILD_NOPENGLDRV`, `BUILD_NOPENGLESDRV`, `BUILD_SOFTDRV`, `BUILD_NULLDRV` | OFF, ON, OFF, OFF (ON in every preset but `BUILD_NOPENGLDRV` on the web) | render devices |
| `BUILD_NOPENALDRV`, `BUILD_NULLSOUNDDRV` | ON, ON | audio devices (OpenAL, silent) |
| `BUILD_WINDRV` | OFF | the original Windows client (no preset builds it) |
| `SOFTDRV_MAX_SCREEN` | 4096 (8192 on the web presets) | the bound of SoftDrv's screen-sized tables |

The web (`if(EMSCRIPTEN)` in `Source/CMakeLists.txt`) forces its own set: static, SDL, Editor, NOpenGLESDrv,
SoftDrv, NullDrv, NOpenALDrv, the null audio device, no NOpenGLDrv, `ASM=0`, `-pthread` at compile time.

## Fetched libraries

The first configure of a preset fetches:

- openal-soft from [10-p/openal-soft-web](https://github.com/10-p/openal-soft-web), branch `ufront/web`
  (`Source/cmake/OpenALSoft.cmake`): static on the web, a shared `libopenal.so.1` on native Linux;
- libxmp `libxmp-4.7.1` (`Source/NOpenALDrv/CMakeLists.txt`; `LIBXMP_SOURCE_DIR` points at a local checkout);
- for `windows` when cross-compiling: SDL2 2.32.10's MinGW binaries and OpenAL Soft 1.25.2's Windows binaries,
  hash-pinned (`Source/cmake/SDL2Mingw.cmake`, `Source/cmake/OpenALMingw.cmake`).

A branch is fetched again at every configure (`cmake --preset …`), never by a plain `cmake --build`. The web
build's `meta.json` names each fetched library as `<repo>@<branch|tag>`, or `local:<path>` for a local
checkout (`FETCHCONTENT_SOURCE_DIR_<NAME>`, `LIBXMP_SOURCE_DIR`); `Source/cmake/ArtifactDeps.cmake` builds that
list from the same cache variables the fetch reads.

## The web build

The link (`Source/Unreal/CMakeLists.txt`) emits a bare loader, `SDLLaunch.{js,wasm,data}` with no HTML page, and
`meta.json` (`Source/Unreal/cmake/emit-artifact-meta.mjs`): target `unreal1`, engine family `UE1`, `abiVersion`
read from `vendor/glue/ue_abi.h` (2), `loaderStyle: global-module`, the git sha, the Emscripten version, the
fetched libraries, the main link flags and the files' checksums. The frontend's build validates it.

- **Threads.** The engine runs on a pthread (`-sPROXY_TO_PTHREAD`, `PTHREAD_POOL_SIZE=4`) and `#canvas` is
  transferred to it. NSDLViewport creates the WebGL context itself (`emscripten_webgl_create_context`; WebGL 2,
  or WebGL 1 with `-WEBGL=1` or where WebGL 2 is missing) and presents each frame with
  `emscripten_webgl_commit_frame` under `-sOFFSCREEN_FRAMEBUFFER`; SDL2 keeps the window and input. SoftDrv
  draws into a driver-owned framebuffer that the viewport blits through a 2D context.
- **Frame loop.** `MainLoop` (`Source/Unreal/Src/SDLLaunch.cpp`) returns to the worker's event loop every frame,
  because that is when the canvas is composited: `emscripten_sleep` for the frame-cap budget left, otherwise a
  `setTimeout(0)` yield (`EM_ASYNC_JS`). Both are Asyncify (`-sASYNCIFY`); `emscripten_set_main_loop` is not
  used, because its unwind would cross `main()`'s try/catch.
- **Files.** WasmFS with a JSImpl backend: `vendor/glue/wasmfs-shim.js` forwards the backend's operations to the
  host-injected `Module.UE.fs.backend`, which serves game files on demand. `Engine/Config/web/Default.ini` is
  preloaded as `/System/Default.ini`; `main()` changes to `/System`, and `appBaseDir` is the working directory.
- **Glue ABI.** `vendor/glue/` is vendored from the frontend's `@ufront/glue` at ABI version 2: `ue_abi.h` (the C
  declarations), `net-shim.js` (the `ue_net_*` imports WebRTCDrv calls, forwarded to `Module.UE.net`) and
  `wasmfs-shim.js`. They are `--js-library` inputs of the link, with `Source/Unreal/msaa-backbuffer.js`. The
  engine exports `ue_exec_command`, which runs a console command and returns its output.
- **Rendering.** NOpenGLESDrv runs its GLES2 calls on WebGL 2. Its antialiasing (`NumAASamples`) is a
  multisampled replacement for Emscripten's emulated back buffer (`msaa-backbuffer.js`), resolved at the present
  and before any read of framebuffer 0; WebGL 1 has none. `Source/Unreal/offscreen-depth24.sh` rewrites the
  emitted loader so that emulated framebuffer has 24-bit depth and a stencil, and fails the build if the
  expected rewrites are not found.
- **Exceptions.** The engine's objects are compiled without exception support, so on the web a throw is never
  caught: package and map loading fail soft there instead (a failed load resolves to NULL, a bad startup map
  falls back to the menu), and `appThrowf` prints `[appThrowf] <message>` before it throws. openal-soft throws
  internally, so its objects use `-fexceptions` and the link enables catching for them
  (`-sNO_DISABLE_EXCEPTION_CATCHING`).
- **Network.** WebRTCDrv (web only) is the net driver: a configured `IpDrv.TcpNetDriver` is replaced with
  `WebRTCDrv.WebRTCNetDriver` at startup. A `<room>?connect` startup URL joins a room; the stock "Find Local
  Servers" menu lists rooms through a synthetic LAN (`Source/WebRTCDrv/WebRTCLan.cpp`). The listen peer's
  prefix is `u1`.
- **Exit.** A normal exit returns the page to the launcher.

## Native Linux

`native` links the bundled openal-soft (`DT_NEEDED libopenal.so.1`, found through `$ORIGIN`) and copies it beside
the binary: a run tree that takes `Unreal.bin` takes `libopenal.so.1` with it. Its backends are those the build
host's headers allowed (the configure log lists them under `Building OpenAL with support for the following
backends`), each loaded at run time. `-DUE_BUNDLED_OPENAL=OFF` links the system library instead.

The log goes to `System/Unreal.log` (`LOG=<file>` names another) and every line is mirrored to stdout. SIGTERM,
SIGINT and SIGHUP request a clean exit (the first one prints `<SIGNAL>: exit requested`, and `Exit requested by
<SIGNAL>` is logged); a second one is reported and ignored. The engine takes its base directory from the
executable's path, so the binary must sit in the run tree's `System/`.

## 64-bit (LP64)

The `native64` presets build an x86_64 engine that runs the retail 32-bit `.u` packages unchanged; 32-bit targets
are unaffected. What it rests on (general code, not `#if`-guarded):

- **Script object references are 4-byte object indices** in memory (`FObjectManager::GetScriptObject`), so
  bytecode keeps the layout the 32-bit compiler gave it. Every multi-byte operand is read by copy
  (`ReadScriptOperand<T>`, `ALWAYS_INLINE`).
- **The script mirror.** Every header that declares a scripted intrinsic class is `#pragma pack(push,4)`; object
  properties align to 4; the script variables that stand in for C++ data (the object header, secondary
  vtables, private pointers, `DynamicArray`, `DynamicString`) take their C++ size in `LinkOffsets`. An intrinsic
  class whose C++ size differs from its script layout is a fatal error (`[layout]`).
- **Layout values in packages** are recomputed on load and written in the 32-bit script format on save: a
  function's `ParmsSize`/`ReturnValueOffset` and a database class's `ClassRecordSize`. The script compiler writes
  the script format too (`ScriptFormatElementSize`, `ScriptFormatStructSize`, `ScriptFormatOffset`), so a 64-bit
  `-MAKE` writes the bytes a 32-bit one does but for the package GUID.
- A call lays each parameter where the callee's linked property reads it; a None context clears its whole
  destination; `PTRINT`/`UPTRINT` wherever a pointer passes through an integer.

Test tools (headless, on an Xvfb display, loopback only):

```bash
UE_ASSETS_DIR=<game folder> tools/dev/mkrun.sh build-native64 <run-tree>   # a run tree with its own Save/Cache
cd <run-tree>/System
gdb -batch -x <repo>/tools/dev/layout-sweep.gdb --args ./Unreal.bin -MAKE            # script vs C++ offsets
DISPLAY=:98 gdb -batch -x <repo>/tools/dev/layout-sweep.gdb --args ./Unreal.bin -NULLRENDERER
./Unreal.bin "DmDeck16?game=U64Test.U64Game" -server        # U64 DONE passed=<n> failed=0
<repo>/tools/dev/botmatch.sh <server-tree> 300 [<client-tree>]                     # a dedicated server + a client
ASAN_OPTIONS=alloc_dealloc_mismatch=0:detect_leaks=0 <repo>/tools/dev/botmatch.sh <asan-tree> 240
unshare -rn <repo>/tools/dev/census/play.sh <tree> -NOPENGLESRENDERER "DmDeck16?game=UnrealI.DeathMatchGame" /tmp/p \
    "stat global" "rmode 1" shot "savegame 5" "open ?load=5"
```

- `tools/test/U64Test` holds the script VM checks; it is compiled by the 32-bit `Unreal.bin -MAKE`
  (`EditPackages[n]=U64Test` in Unreal.ini, the folder beside `System`), and must pass on 32-bit before 64-bit.
- Run the layout sweep in both hosts: some reflected layouts exist in only one of them.
- `tools/dev/census/` (`census.py`, `gcovdump.c`) reports which engine functions a `--coverage` build ran. Keep
  the preset's `-DASM=0` in the coverage flags.
- The engine writes its ini back on exit, and a `-*RENDERER` switch is saved as the player's choice
  (`botmatch.sh` restores the trees' ini files). An `appError` leaves the process waiting on SDL's message box
  (`play.sh` stops at a `Critical:` line).

## Windows (MinGW-w64)

`windows` cross-compiles for `i686-w64-mingw32` (`Source/cmake/toolchain-mingw-w64.cmake`) or builds from an MSYS2
`MINGW32` prompt, where pacman's SDL2 is used and nothing is fetched for it. The preset builds a 32-bit exe.

- SDL.h renames `main()` to `SDL_main`, and SDL2main provides the `WinMain` that calls it (`-lmingw32 -lSDL2main`
  before `-lSDL2`).
- The GCC runtime and winpthread link statically, so the exe imports only system DLLs and `OpenAL32.dll`, which
  the build copies beside it.
- `Source/Unreal/Src/Res/LaunchRes.rc` finds its icon beside itself (the build passes that directory to windres).
- Renderers: NOpenGLDrv (first in the default order), NOpenGLESDrv (needs ANGLE's `libEGL.dll`/`libGLESv2.dll`
  beside the exe), SoftDrv and NullDrv.
- The exe is a GUI program: read `System/Unreal.log`. In a console, Ctrl-C, Ctrl-Break or closing it requests a
  clean exit.

Without Windows, `wine` and Xvfb check that it starts (CPU-only renderers):

```bash
Xvfb :99 -screen 0 1024x768x24 &
export WINEDLLOVERRIDES="mscoree,mshtml="    # a new prefix otherwise waits on the Mono/Gecko installers
DISPLAY=:99 WINEPREFIX=<prefix> wine Unreal.exe -SOFTWARERENDERER   # or -NULLRENDERER
WINEPREFIX=<prefix> wineserver -k
```

Check the log for `Opened viewport` and `Init: Game engine initialized`. Copy the exe into the run tree (the base
directory comes from the binary's path) and dereference asset symlinks (`cp -rL`).

## Run-time behaviour shared with the other engines

- **Render devices.** `-NOPENGLRENDERER` (native and Windows), `-NOPENGLESRENDERER`, `-SOFTWARERENDERER`,
  `-NULLRENDERER` pick the device before `Engine->Init`; a switch whose device initializes is saved as the
  player's choice (`[Engine.Engine] ufrontUserRenderDevice`). Without a switch the saved device is used when it is
  a hardware default or the player's choice, else the preference order: NOpenGLDrv first on Windows, NOpenGLESDrv
  first elsewhere.
- **Subsystems.** Before `Engine->Init`, a configured `ViewportManager`, `AudioDevice` or `NetworkDevice` the build
  does not contain (a retail ini names `Galaxy.GalaxyAudioSubsystem`) is replaced by the best one it has
  (NSDLClient; NOpenALDrv, then SoundDrv; TcpNetDriver, or WebRTCNetDriver on the web) and logged.
- **Antialiasing and anisotropic filtering.** NOpenGLESDrv and NOpenGLDrv have `NumAASamples` (default 4; 0 or 1 is
  off) and `MaxAnisotropy` (default 16, clamped to the driver's maximum). Natively NSDLViewport asks for the
  device's sample count when it creates the window. Log lines: `GLES2:`/`OpenGL: antialiasing <n>x
  (multisampled window|multisampled back buffer)` or `… antialiasing off`.
- **AutoFOV.** `[ufront.AutoFOV]` `AutoFOV` (0 off, 1 Hor+, 2 Adaptive, the default), `ReferenceAspect`,
  `WeaponScale`; `[AutoFOV]` log lines.
- **Command line.** `-ini:<File>:[<Section>]:<Key>=<Value>` sets a config key at startup (`User` and `System` both
  mean the one default ini); a token that does not fit the 4,096-character command line is dropped whole, with a
  warning.
- **Frame-rate cap.** `[Engine.Engine] FrameRateCap` (default 120, 0 uncapped) when no net or demo rate applies.
- **Cheats and the FPS log.** `WHEREAMI` prints `<x> <y> <z> <yaw> <pitch>`; `TELEPORT <x> <y> <z> [<yaw>
  [<pitch>]]` moves the local player. Allowed in a standalone game and for a listen host, refused elsewhere unless
  `[Engine.GameEngine] AllowNetCheats=True`; each use logs a `Cheat:` line. `[Engine.GameEngine]
  FpsLogSeconds=<s>` or `FPSLOG <s>` logs an `FPS:` line every `<s>` seconds.
- **Config.** `SaveConfig` writes the file at once; `FLUSHCONFIG` writes the whole config cache.
- **Language.** `[Engine.Engine] Language` (`Init: Language: <ext>` in the log); localized properties are read from
  `<Package>.<lang>`, falling back to `<Package>.int` per key.
- **Game pads.** `[NSDLDrv.NSDLClient] UseJoystick` (on by default) is read every tick, so the game's own option
  opens or closes the first pad at once; pads hot-plug; sticks and triggers are read from the pad's state.
- **Saves** sort their name, import and export tables with a stable sort, so the same save gives the same file on
  every platform.

## CI

`.github/workflows/build.yml` builds `native` (in an `ubuntu:24.04` container), `windows` and `wasm` on every push
and pull request, checks that the Linux build is one engine binary, that the Windows exe imports only system DLLs
and that `meta.json` names the commit built, and uploads each build for 7 days. A push to `main` also replaces the
rolling `latest` release with one zip per build. CI does not run the game.
