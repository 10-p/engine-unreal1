# Engine/Config/web — the web build's boot config

`Default.ini` here is the web build's boot config: the Emscripten link preloads it as `/System/Default.ini`
(`Source/Unreal/CMakeLists.txt`, which also relinks when it changes), and `appInit` derives `Unreal.ini` from it at
boot. It carries the web's settings, among them the `[WebRTCDrv.WebRTCNetDriver]` section a native install's
inis do not have, the web's client and HUD defaults, and no save-slot names.

`../*.ini` (the parent folder) is the native set, which a native install copies into `System/`; the web does not
ship it. The frontend's asset manifest leaves `System/Unreal.ini` out, so a game install's copy cannot shadow
the one derived from this file. Change the web config here, not in an asset tree.
