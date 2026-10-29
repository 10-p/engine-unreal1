## What?

Unreal Engine 1 v200 source with modifications to make it run on modern systems and in the browser.
Requires assets from the original Unreal v200 retail release or from the v205 demo. Other versions have not been tested.
Based on [fgsfdsfgs/UE1](https://github.com/fgsfdsfgs/UE1). Build details, options and test tools: [BUILD.md](BUILD.md).

## Changes from the original source

* **WebAssembly build:** every package in one static wasm that runs on a browser worker thread, with WebGL 2, a file system served by the host page and a frame loop that returns to the browser every frame.
* **SDL2 client driver (NSDLDrv):** a resizable window that follows its real drawable size, pointer capture in play, game pads with hot-plug.
* **OpenGL ES renderer (NOpenGLESDrv):** the default hardware renderer on the web, Linux and Android (the Windows builds leave it out), with antialiasing and anisotropic filtering.
* **OpenGL renderer (NOpenGLDrv):** desktop GL in the native and Windows builds, with antialiasing and anisotropic filtering.
* **Software renderer (SoftDrv):** its portable C path on every target, with volumetric fog, mesh fog and texture smoothing matched to the original MMX kernels.
* **Null renderer (NullDrv):** a render device that draws nothing, for headless runs.
* **WebRTC net driver (WebRTCDrv):** multiplayer and the stock LAN server browser in the browser.
* **OpenAL Soft audio (NOpenALDrv):** one openal-soft build on the web, Linux and Windows, with EFX reverb, optional HRTF and libxmp tracker music.
* **64-bit and ARM support:** x86_64 and arm64 builds that run the retail 32-bit packages unchanged.
* **Windows build:** one static MinGW-w64 executable, cross-compiled from Linux or built under MSYS2.
* **PS Vita build** (from fgsfdsfgs/UE1).
* **Android build:** a Gradle/NDK project (`Android/`) for Android 7.0 and later: the engine as `libUnreal.so` under SDL's activity, NOpenGLESDrv on the device's OpenGL ES, openal-soft on OpenSL ES, the game folder copied into internal storage from a folder chosen with the system's folder picker (or read in place from shared storage), a resolution scale, an exit prompt on Back, an on-screen pad that is a virtual SDL controller (the mouse is off), hidden while a real pad is used and back on a touch, gyro aiming (a launcher switch, off by default, with horizontal and vertical sensitivities; slowed by the zoom as the mouse is), and a suspended game in the background (no tick, the OpenAL device paused).
* **GCC and Clang support:** the engine compiles cleanly with GCC, Clang, Emscripten and MinGW-w64.
* **Languages:** localized properties load, so menus and messages follow the chosen language.
* Editor UI is not supported.

The full list, with the medium and small changes, is in the frontend's change inventory
(`<repo>/../unreal-frontend-arch-private/docs/14-change-inventory.md`).

## Building

Builds are 32-bit unless noted and use the CMake presets in `Source/`, so run every `cmake` command from `Source/`.
The first configure of each preset needs network access: OpenAL Soft, libxmp and (Windows) SDL2 are fetched.

### Linux x86
1. Install git, cmake (>= 3.21), make, gcc and g++ with 32-bit (multilib) support, and a 32-bit SDL2.
   * Debian/Ubuntu:
     ```
     sudo dpkg --add-architecture i386 && sudo apt update
     sudo apt install git cmake make gcc g++ gcc-multilib g++-multilib libsdl2-dev:i386
     ```
   * Arch/Manjaro: enable `[multilib]`, then `sudo pacman -S git cmake make gcc lib32-glibc lib32-gcc-libs`.
     32-bit SDL2 is not in the official repos: install `lib32-sdl2-compat` from the AUR, or build SDL2 yourself (below).
2. Build:
   ```
   cmake --preset native && cmake --build --preset native -j$(nproc)
   ```
3. The result is `build-native/Unreal/Unreal.bin` plus `libopenal.so.1`, which must stay next to it.

**64-bit (x86_64):** use the `native64` preset instead of `native`, with the regular 64-bit libraries in place of the i386/lib32 ones. The result lands in `build-native64/Unreal/`.

**32-bit SDL2 from source** (when your distro has no package): build it into any prefix and point the preset at it.
```
git clone --depth 1 --branch release-2.32.10 https://github.com/libsdl-org/SDL.git
cmake -S SDL -B SDL/build -DCMAKE_C_FLAGS=-m32 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=<prefix>
cmake --build SDL/build -j$(nproc) && cmake --install SDL/build
cmake --preset native -DSDL2_INCLUDE_DIR=<prefix>/include/SDL2 "-DSDL2_LIBRARY=-L<prefix>/lib -lSDL2"
```
Copy `<prefix>/lib/libSDL2-2.0.so.0` next to `Unreal.bin` (or add `<prefix>/lib` to `LD_LIBRARY_PATH`).

### Windows x86 (cross-compiled from Linux)
1. Install the MinGW-w64 i686 toolchain: `g++-mingw-w64-i686-posix` (Debian/Ubuntu) or `mingw-w64-gcc` (Arch).
2. Build:
   ```
   cmake --preset windows && cmake --build --preset windows -j$(nproc)
   ```
3. The result is `build-windows/Unreal/Unreal.exe` plus `OpenAL32.dll`, which must stay next to it.
4. For 64-bit Windows, install the x86_64 toolchain (`g++-mingw-w64-x86-64-posix`) and use the `windows64` preset; the
   result is in `build-windows64/Unreal/`.

### Windows x86 (MSYS2 on Windows)
1. Install MSYS2 and open the `MINGW32` prompt. **Do not** use the `MINGW64` or `MSYS` prompts.
2. Install dependencies: `pacman -S git make mingw-w64-i686-toolchain mingw-w64-i686-cmake mingw-w64-i686-SDL2`
3. Build with the same `windows` preset as above. The output is the same, and `SDL2.dll` from `/mingw32/bin` must sit next to the exe.

### Web (WebAssembly)
1. Install [emsdk](https://github.com/emscripten-core/emsdk) 5.0.7 and load it: `./emsdk install 5.0.7 && ./emsdk activate 5.0.7 && source ./emsdk_env.sh`
2. Build:
   ```
   cmake --preset wasm-perf && cmake --build --preset wasm-perf -j$(nproc)
   ```
3. The result is `build-wasm-perf/SDLLaunch/` (`SDLLaunch.{js,wasm,data}` + `meta.json`). It is not playable on its own: the frontend repo serves it.

### Android
1. Install JDK 17, the Android SDK (platform 36, build-tools 36.0.0, NDK r28c) and CMake, and write `Android/local.properties` for them (see [BUILD.md](BUILD.md), "Android").
2. Build and install:
   ```
   cd Android && ./gradlew assembleRelease && adb install -r app/build/outputs/apk/release/app-release.apk
   ```
3. Put the game folder (`System`, `Maps`, `Textures`, `Sounds`, `Music` of an Unreal v200 install) on the device as `Unreal`, then in the app choose it and copy it to internal storage, or play it in place from `/sdcard/Unreal` with All files access.

The Android build runs on a Retroid Pocket 6 (Android 13, Adreno 740): the menus with its built-in pad, the intro and the first level at 1920x1080 with 4x antialiasing at the 120 fps cap, sound on OpenSL ES, the resolution scale, Back as Escape and the exit prompt.

## Running

1. Install the original retail v200 release of Unreal or the v205 demo.
2. Copy the files you built (see each platform above) to `Unreal/System`. Overwrite everything.
3. Copy the contents of `Engine/Config` (the `.ini` files, not `web/`) to `Unreal/System`. Overwrite everything.
4. Run `System/Unreal.bin` (Linux) or `System/Unreal.exe` (Windows).

Use real copies of the game files, not symlinked folders: the engine does not find packages through symlinked directories (`Can't find file for package ...`).
Headless check: `-NULLRENDERER` (Linux) or `-SOFTWARERENDERER` (Windows, also under wine); the log `System/Unreal.log` should reach `Init: Game engine initialized`.
On Linux, SIGTERM, SIGINT (Ctrl-C) or SIGHUP ends the game cleanly; a second one is ignored.

## Credits

* Epic Games wrote Unreal and its v200 source, which this engine is built from.
* BearIvan (Ivan Sobolevskiy) imported the v200 source and made it build with MSVC.
* fgsfds ([fgsfdsfgs/UE1](https://github.com/fgsfdsfgs/UE1)) ported it to GCC and CMake and wrote the SDL2 client, the OpenGL, OpenGL ES and OpenAL drivers and the PS Vita port, with pull requests by BSzili (big-endian fixes, driver fixes, Hor+ widescreen) and babbygremlin (game speed above the standard frame rate).
* The libraries: OpenAL Soft (Chris Robinson, kcat; LGPL), libxmp, SDL2, glad (David Herberth), glm (G-Truc Creation), TargetArch.cmake (Petroules Corporation) and Emscripten.

## Note

Unreal Engine, Unreal and any related trademarks or copyrights are owned by Epic Games. This repository is not affiliated with or endorsed by Epic Games.
This is based on the v200 source available elsewhere on the Internet, with assets and third party proprietary libraries removed.
Do not use for commercial purposes.
