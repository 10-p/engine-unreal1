# engine-unreal1 — agent guide

**What this is:** the private engine repo for **Unreal (1998), Unreal Engine 1 v200**: engine source only. It
builds a WebAssembly artifact (and native Linux and Windows binaries, and an Android APK from `Android/`). The frontend repo
`10-p/unreal-frontend-arch-private` consumes the artifact and owns the host page, the UI, the build assembly and
the browser glue behind a thin ABI. Building, options and test tools: `BUILD.md`.

**Lineage:** Epic's v200 drop, imported by BearIvan, then [fgsfdsfgs/UE1](https://github.com/fgsfdsfgs/UE1) up to
`41cf09a`; our work is everything after that commit. The baseline is the Epic drop in `~/WorkUnreal/epic-sources/`
(see its README). No `upstream` remote is configured: compare against `41cf09a` or the drop.

**Features** are listed in the frontend's `docs/14-change-inventory.md`; a new feature adds its line there.

## Map
| Thing | Where | Role |
|-------|------|------|
| This repo (private) | `10-p/engine-unreal1` | UE1 source → web and native builds |
| Frontend | `10-p/unreal-frontend-arch-private` | consumes `build-wasm-perf/SDLLaunch/`; host, UI, glue, publishing |
| Game assets | any local folder (never in git) | an Unreal v200 install; no build reads it |
| Android app | `Android/` | the Gradle/NDK project: SDL's activity, the launcher, the on-screen pad (BUILD.md "Android") |
| ARM device port (reference) | [10-p/UE1-funkey](https://github.com/10-p/UE1-funkey) | a SoftDrv port to a small ARM device; read it, do not cherry-pick its device workarounds |

## Load-bearing facts
- **One static binary on every preset** (`BUILD_STATIC`, `UNREAL_STATIC`, `--whole-archive` keeps the package
  registrants). Classes of linked packages are registered at startup.
- **`ASM=0` off MSVC:** SoftDrv and Core use their portable C paths; SoftDrv's are matched to the MMX kernels
  (`tools/asmport/`).
- **Main loop on the web:** `MainLoop` returns to the worker's event loop every frame through Asyncify
  (`emscripten_sleep`, or a `setTimeout(0)` `EM_ASYNC_JS` yield), never `emscripten_set_main_loop`, whose unwind
  collides with `main()`'s try/catch.
- **Files:** the engine uses libc `fopen`/`opendir`; on the web WasmFS's JSImpl backend (`vendor/glue/wasmfs-shim.js`)
  serves them from the host. Do not write an engine-side web file manager.
- **Glue ABI:** `vendor/glue/` (ue_abi.h, net-shim.js, wasmfs-shim.js) is vendored from the frontend at ABI
  version 2. Only these thin forwarders and `Source/Unreal/msaa-backbuffer.js` are baked into the wasm; the
  transport, the file system and storage are injected by the host at run time. A change to the ABI is a
  cross-engine change owned by the frontend: re-vendor, never edit here.
- **The log reaches stdout on every build** (`FGlobalPlatform::WriteBinary`, the same lines as `Unreal.log`); the
  frontend's E2E reads engine milestones from the browser console. Do not remove it, and do not reword a log line
  the frontend matches (grep `packages/e2e` and `targets/` in the frontend first).
- **Exceptions on the web:** engine objects cannot catch, so web loading fails soft (`SafeLoadError`,
  `GetPackageLinker`, `LoadObject` resolve to NULL; a bad startup map falls back to the menu) under
  `#ifdef __EMSCRIPTEN__`, and `appThrowf` prints `[appThrowf] <message>` first. Native keeps Epic's errors. The
  link enables catching only for openal-soft's `-fexceptions` objects; leave the engine's objects as they are.
- **Net on the web:** WebRTCDrv (`UWebRTCNetDriver`) over the `ue_net_*` imports; a configured
  `IpDrv.TcpNetDriver` is replaced at startup. The package, class and `NetworkDevice` value keep their names.

## Rules
- **Comments** explain the code as it is, in one or two lines. No stage markers or work ids, no story of a fix,
  no where-a-report-came-from, no owner quotes, dates or commit hashes, no cross-references to other files' or
  repos' comments, no glyphs or capitalised emphasis, no platform asides in shared code (a platform statement
  lives inside its `#ifdef`). A comment that disagrees with the code is a bug: fix one of them. Epic's and the
  upstream fork's comments are theirs: leave them. Log text is not a comment.
- **Docs** (README.md, BUILD.md, this file, `Engine/Config/web/README.md`, `tools/asmport/NOTES.md`) describe the
  current state in present tense: no phases, no history, no dates, no owner attributions, no measurements as
  proof. Rewrite from the code; every path, flag, preset and script a doc names must exist.
- **Names** follow behaviour. Shared CMake options take the `UE_` prefix. Do not rename ini sections or keys
  players may have saved (e.g. `ufrontUserRenderDevice`), exported ABI symbols, or log lines the frontend
  matches. "ufront" as a namespace stays (`[ufront.AutoFOV]`, `UfrontStableSort`, `[ufront/travel]`).
- Commit subjects name the feature (Conventional Commits), never a stage number or a date.
- Never commit game assets or build trees (`build-*/`, gitignored). Do not push to fgsfdsfgs/UE1.
- Before a commit, `node <frontend>/.ufront/hygiene/scan.mjs --all --repo .` must report no hard findings.
