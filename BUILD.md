# Building engine-unreal1 (WebAssembly + native)

The **private engine repo** for the original **Unreal (1998), Unreal Engine 1 v200** web port — engine
source only. Game assets (Unreal v200: maps/textures/music/sounds + the `.u`/`.int` packages) live **out of
tree** at `$UNREAL_ASSETS_DIR` (default `/home/ion/unreal-assets`) and are never tracked here. Base import:
[fgsfdsfgs/UE1](https://github.com/fgsfdsfgs/UE1) @ `41cf09a` (kept as the `upstream` remote).

The public monorepo `ufront` (`/home/ion/ufrontend`) consumes this repo's WASM artifact, owns the host
page / UI / glue / assembly, and injects the heavy glue behind the thin ABI shim. **Migration state lives
in the monorepo** (`docs/migration/STATE.md`), not here.

## Build matrix (CMake presets — `Source/CMakePresets.json`)

| Preset | Output | Purpose |
|--------|--------|---------|
| `wasm` | `build-wasm/SDLLaunch/SDLLaunch.{js,wasm,data}` + `meta.json` | Emscripten **dev** (assertions + DWARF) |
| `wasm-perf` | `build-wasm-perf/SDLLaunch/…` | Emscripten **product** (`-O3`, no debug) — what `ufront` consumes |
| `native` | `build-native/Unreal/Unreal.bin` | 32-bit native GLES/software twin (needs `libsdl2-dev:i386`) |

```bash
source ~/emsdk/emsdk_env.sh            # emsdk 5.0.x; sets $EMSDK for the toolchain file
cmake --preset wasm-perf && cmake --build --preset wasm-perf -j"$(nproc)"   # the web build
cmake --preset native    && cmake --build --preset native    -j"$(nproc)"   # the native twin
```

The web build emits a **bare loader** (`SUFFIX .js`, no `--shell-file`) + `meta.json` (`engineFamily:UE1`,
`abiVersion:1`, `loaderStyle:global-module`); `ufront`'s `packages/build` validates it. Assets: only
`System/Default.ini` is preloaded into MEMFS; everything else streams at runtime via the monorepo's FetchFS.

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

## GL4ES / future renderers

Unlike engine-ut2004, this repo does **not** use GL4ES — UE1 ships a native GLES2 driver. A more
feature-complete hardware renderer (OldUnreal/XOpenGLDrv with an ES target, or cwdohnal/utglr + GL4ES —
each backported to v200) is a documented future option, not in the current build.
