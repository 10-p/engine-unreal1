# engine-unreal1 — agent guide

**What this is:** the **private** engine repo for the original **Unreal (1998), Unreal Engine 1 v200**,
web port. It holds engine source only and produces a **WASM artifact** (+ native binaries). It is **not**
the product on its own — the public monorepo `ufront` (`10-p/unreal-frontend-arch-private`) consumes this engine's WASM
artifact, owns the host page / UI / build assembly, and owns the browser glue behind a thin **ABI shim**.

Created by **step 2.6** of the ufront re-architecture (the first cross-engine-*generation* port: UE1 vs
UT2004's UE2.5, on the same shell + glue + ABI). Base import: **[fgsfdsfgs/UE1](https://github.com/fgsfdsfgs/UE1)**
@ `41cf09a` (kept as the `upstream` remote; our private work is fresh commits on `main`).

## Map of the world
| Thing | Where | Role |
|-------|------|------|
| This engine repo (private) | `10-p/engine-unreal1` | UE1 source → WASM/native artifacts |
| Sibling engine (the MODEL) | `10-p/engine-ut2004` | the UE2.5 port — the pattern this repo mirrors |
| Public monorepo | `10-p/unreal-frontend-arch-private` | consumes the artifact; host/UI/glue/build; **migration state lives here** |
| Game assets (never in git) | any local folder | Unreal v200, ~456 MB; not read by any build |
| UE1 reference (read-only) | [fgsfdsfgs/UE1](https://github.com/fgsfdsfgs/UE1) (add as `upstream`) | pristine fgsfdsfgs/UE1 @ `41cf09a` (identical to our base) |
| Software-renderer REFERENCE | [10-p/UE1-funkey](https://github.com/10-p/UE1-funkey) | ARM port proving `SoftDrv` on non-x86 — **reference only, not a commit quarry** |

> **Migration plan & durable state are in the monorepo**, not here:
> `docs/migration/STATE.md` (read first if continuing the migration),
> `ROADMAP.md`, `steps/2.6-engine-unreal1.md` (this step's spec + decisions D1–D12).

## Status (Phase 2, step 2.6 — IN PROGRESS)
Standing up the web port. The engine bakes only the thin `webrtc_*` shim (`vendor/glue/webrtc-shim.js`,
vendored from `@ufront/glue` at `ABI_VERSION 1`); heavy glue (FetchFS + IDB + WebRTC session logic) is
**injected at runtime** by the host, not baked. The WASM build emits a **bare loader**
(`SDLLaunch.{js,wasm,data}` + `meta.json`, `loaderStyle=global-module`, no `.html`) that the monorepo's
`packages/build` validates + assembles into `serve-web/`.

## Conventions / load-bearing facts (the UE1 specifics)
- **Renderer (step 2.6, ADR/D3):** ship the port's **native `NOpenGLESDrv` (GLES2 → WebGL directly, no
  GL4ES)** as the default + **`SoftDrv` (software)**. Under `__EMSCRIPTEN__`, `NOpenGLESDrv` bypasses
  `glad` and calls Emscripten's linked GLES2 symbols directly. A better HW renderer (OldUnreal/XOpenGLDrv
  ES-target, or cwdohnal/utglr + GL4ES — each backported to v200) is a documented **future** step.
- **`ASM=0` for the web/non-x86 build.** UnBuild.h defaults `ASM 1` (x86 inline asm). `SoftDrv`'s portable
  C path (upstream `softdrv: make it build and run on gcc/non-x86`, present at `41cf09a`) is what the web
  build uses.
- **Static single binary.** `BUILD_STATIC=ON` → `-DUNREAL_STATIC` + `Src/UnStaticExports.cpp`
  (`GExportsTable`/`appGetStaticExport`), all packages linked with `--whole-archive` (preserve the
  package registrants). Emscripten can't do UE1's `.so`/`.dll` package loading (`docs/10` §4).
- **Main-loop inversion (Asyncify).** UE1's blocking `MainLoop` (`Unreal/Src/SDLLaunch.cpp`,
  `while(GIsRunning){ Engine->Tick(); appSleep(); }`) becomes `emscripten_set_main_loop` driving one
  `Tick()` per callback + `-sASYNCIFY` (`docs/10` §2).
- **File layer is POSIX — untouched.** UE1 bottoms out at libc `fopen`/POSIX `opendir` (`Core/Src/UnFile.cpp`,
  no `FFileManager`). FetchFS (injected glue) intercepts **below** it at the Emscripten FS layer — so the
  same shim/glue serve UE1 unchanged. **Do not** write an engine-side web file manager.
- **Net (step 2.6c):** a new engine-local **WebRTC `UNetDriver`** (adapting engine-ut2004's `WebRTCDrv` to
  UE1's `UNetDriver`/`UNetConnection`, `Engine/Inc/UnNetDrv.h`/`UnConn.h`); auto-switch
  `IpDrv.TcpNetDriver` → the WebRTC driver on Emscripten. Engines are independent — **no shared code**.
- **Console logging (step 2.6e):** under `__EMSCRIPTEN__`, `FGlobalPlatform::WriteBinary` (`Core/Src/UnPlat.cpp`,
  the `GLog` sink) echoes each non-suppressed log line to `stdout` → the host `Module.print` → `console.log`.
  So the browser console mirrors the MEMFS `Unreal.log` and the ufront E2E harness reads engine milestones
  from the console (matching engine-ut2004). **Do not** remove it — the E2E net/boot assertions depend on it.
- **Editor package + exception-safe loading (step 2.6f):** `BUILD_EDITOR` is ON for Emscripten (stripped
  no-GUI Editor) — bot/character meshes+skins load through Editor classes. Exception CATCHING stays OFF
  (`-fexceptions` overflows V8 on `UnrealI.u`; `-fwasm-exceptions` conflicts with ASYNCIFY), so UE1's
  exception-based error model can't unwind across Asyncify frames → a thrown load error aborts the module.
  Fix (mirrors engine-ut2004): make load failures resolve to **NULL** instead of throwing —
  `Core/Src/UnObj.cpp` `SafeLoadError` (no re-throw) + `LoadObject` (return NULL on not-found), guarded by
  `#ifdef __EMSCRIPTEN__`. **Patch new throw sites one by one** as they surface; the `[appThrowf]` console
  line (added in `UnFile.cpp`) names each. **Do NOT** try to enable exception catching — it's a dead end here.
  **Step 2.6g** extended this to the missing-map path: `GetPackageLinker` returns NULL (file-resolution
  throws) + `LoadMap` NULL-checks the verify result + `UGameEngine::Init` falls back to the Entry/menu
  instead of `appErrorf(FailedBrowse)` — so a bad `?map=…` fails soft (menu fallback), no abort.

## Do NOT
- **Do NOT** blindly cherry-pick from `10-p/UE1-funkey` — it's a very limited ARM device with device-
  specific workarounds. Pull **only** what this web port genuinely needs, case-by-case (e.g. its
  Editor-package inclusion). **Never** copy its device hacks (240×240 / 64 MB / DirectFB / `.opk` inis).
- **Do NOT** bake heavy glue (FetchFS/IDB/WebRTC logic) into the WASM — only the thin `webrtc_*` shim.
- **Do NOT** push to `upstream` (it's fgsfdsfgs/UE1). This repo stays **private, local-only**.
- **Do NOT** commit game assets or build trees (`build-*/`) — gitignored.
- **Do NOT** change the ABI. It's frozen at `vendor/glue/ue_abi.h` (`ABI_VERSION 1`); a change is a
  deliberate cross-engine act owned by the monorepo (`docs/02` §5).
