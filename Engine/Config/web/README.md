# Engine/Config/web — the web build's boot config

`Default.ini` here is the web build's boot config: the Emscripten link preloads it as `/System/Default.ini`
(`Source/Unreal/CMakeLists.txt`, which also relinks when it changes), and `appInit` derives `Unreal.ini` from it at
boot. It carries the web's settings, among them the `[WebRTCDrv.WebRTCNetDriver]` section a native install's
inis do not have, the web's client and HUD defaults, and no save-slot names. Its render device is NOpenGLESDrv, as
in the native and Android inis; the launcher's Software and Null entries pass their `-*RENDERER` switch.

`../Default.ini` is the native default ini, which the native release archives ship in `System/` and from which
`tools/dev/sync-android-ini.py` writes `../android/Default.ini`; the web does not ship either. `../Unreal.ini` is a
saved player config (window positions, save-slot names), the reference for `UNSDLClient`'s compiled defaults, and
ships nowhere. The frontend's asset manifest leaves `System/Unreal.ini` out, so a game install's copy cannot shadow
the one derived from this file. Change the web config here, not in an asset tree.
