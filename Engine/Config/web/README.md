# Engine/Config/web — web platform config (build-consumed)

`Default.ini` here is the **web** boot config: the Emscripten link `--preload-file`s it
(`Source/Unreal/CMakeLists.txt`), and `appInit` derives `Unreal.ini` from it at boot. It carries the
web-only settings — notably the `[WebRTCDrv.WebRTCNetDriver]` section — that a native game install's inis
do not have.

`../*.ini` (the parent folder) is the **platform-agnostic / native** reference set; it is NOT what the web
ships.

Previously the web build preloaded this from the out-of-tree asset dir, so the web config lived only in an
untracked game install and was silently reverted whenever that image was rebuilt. The ufront manifest now
also excludes `System/Unreal.ini`, so a stale install copy cannot shadow what `appInit` derives here.
**Edit this folder when the web config changes — never the asset tree.**
