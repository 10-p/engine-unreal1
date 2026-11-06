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

## Config version

Every default ini this repo ships opens with a section of its own:

```ini
[ConfigVersion]
Version=1
```

`appInit` makes a player's `Unreal.ini` as a byte copy of `Default.ini` on the first run and never refreshes it, so
the `Version` it keeps says which defaults it was copied from; an `Unreal.ini` without the section was made before the
number and is version 0. No class owns the section, so no `SaveConfig` or `ResetConfig` touches it and the config
cache writes it back as it is. Nothing reads the number to change anything: a player's ini is never migrated.
`Default.ini` here, `../Default.ini` and the generated `../android/Default.ini` (the generator keeps the section first)
carry the same number; `../Unreal.ini` ships nowhere and has none.

The rule: a commit that adds a default, changes a default value or removes a key in one of these inis, or adds or
changes a compiled or script default of a setting the engine saves to the ini, bumps `Version` by one in all of them
and adds the new number's row below, saying what a player on the version before lacks. Comment-only and
whitespace-only changes do not bump. The frontend's `ini-config-version` pre-commit gate checks the inis; its
`docs/CONVENTIONS.md` § Ini config version has the rule in full.

| Version | What a player on the version before lacks |
|---------|-------------------------------------------|
| 1 | The defaults when the number was introduced: an `Unreal.ini` without the section may lack any of them. |
