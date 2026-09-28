cmake_minimum_required(VERSION 3.15)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

project(UE1 CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT EXISTS ${CMAKE_BINARY_DIR}/CMakeCache.txt)
  if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
  endif()
endif()

# Options

option(USE_SDL "Use NSDLDrv instead of WinDrv" ON)
# Web build mode (EMSCRIPTEN only) — see the three-mode block further down. UT_WEB_PERF is read by
# Source/Unreal/CMakeLists.txt for the link flags; UT_WEB_DEBUG selects -O0 over the -O2 dev default.
option(UT_WEB_PERF "Emscripten: shipping build — link at -O3 -g0, assertions off" OFF)
option(UT_WEB_DEBUG "Emscripten: -O0 instead of the -O2 dev default (nothing inlined, every local live)" OFF)
# ★ ufront 2.39 — BUILD_EDITOR IS NOT OPTIONAL FOR A PLAYABLE GAME, on any platform.
#
# UE1 ships game CONTENT that references classes living in the Editor package, so "the editor" here is
# not a tool you are choosing to build — it is a runtime dependency of maps and meshes. The web build has
# forced it ON since 2.6f; the NATIVE and WINDOWS presets still said OFF, and the symptom is exactly what
# the owner reported:
#
#     Warning: Failed to load 'Editor': Can't find Class Editor.TransBuffer in file '../System/Editor.u'
#     Warning: Failed to load 'Level None.MyLevel':
#
# — Dug.unr silently falls back to the default map. (Editor.u, the SCRIPT half, ships with the game; it is
# the NATIVE half that was missing. Same shape as UT99's UWeb crash in 2.38.) The Editor built here is the
# stripped, no-GUI NullEdHook variant that links only Engine+Core, so this costs a static library and
# nothing at runtime. Verified natively: before, Dug fell back to Unreal.unr; after,
# "Bringing Level Dug.MyLevel up for play".
option(BUILD_EDITOR "Build the Editor package (a RUNTIME dependency — see above)" ON)
option(BUILD_SOFTDRV "Build SoftDrv (x86/MSVC only)" OFF)
option(BUILD_NULLDRV "Build NullDrv (GPU-free no-op render device; -NULLRENDERER). No GL context -> headless / dedicated-server run." OFF)
option(BUILD_NOPENGLDRV "Build NOpenGLDrv" OFF)
option(BUILD_NOPENGLESDRV "Build NOpenGLESDrv" ON)
option(BUILD_NULLSOUNDDRV "Build SoundDrv (Null driver)" ON)
option(BUILD_NOPENALDRV "Build NOpenALDrv" ON)
option(UT_BUNDLED_OPENAL "Native Linux: build openal-soft (the ufront fork) and ship libopenal.so.1 beside the binary, instead of linking whatever libopenal the system has (ufront 2.70)" ON)
option(BUILD_WINDRV "Build WinDrv" OFF)
option(BUILD_STATIC "Link everything into a single binary" OFF)
# ufront 2.84 — a 64-bit (LP64) build (2.83 did the same for UT99). OFF keeps the historical behaviour: an
# x86_64 compiler is demoted to -m32, because until 2.84 the engine could only run 32-bit. ON builds x86_64 as
# x86_64. An arm64 compiler has nothing to demote to, so it always builds 64-bit. The C++ side does not read
# this option: PLATFORM_64BITS comes from the compiler (Core.h), so it can never disagree with the code built.
option(UT_ALLOW_64BIT "Build x86_64 as a 64-bit (LP64) engine instead of demoting to -m32" OFF)

# ufront step 2.6a — Emscripten / WebAssembly build (the web port). Emscripten can't load UE1's
# .so/.dll packages, has no x86 inline asm, and gets SDL2/GLES2 from its own ports. Force the web-safe
# configuration here so `emcmake cmake` (or the wasm/wasm-perf presets) needs no extra -D flags.
if(EMSCRIPTEN)
  set(BUILD_STATIC ON)            # single binary via UNREAL_STATIC + --whole-archive (like PSVita)
  set(USE_SDL ON)                 # NSDLDrv (SDL2 window/GL context -> WebGL)
  set(BUILD_EDITOR ON)            # step 2.6f: the stripped (NullEdHook, no-GUI) Editor package — links only
                                  # Engine+Core. UE1 loads some game content (bot/character meshes+skins,
                                  # e.g. Male3Skins) through classes that live in the Editor package; without
                                  # it, spawning DM bots aborts on package load. This is the known UE1-funkey
                                  # "Included the Editor package. Without it Dug can't load." fix (owner-confirmed).
  set(BUILD_WINDRV OFF)
  set(BUILD_NOPENGLDRV OFF)       # desktop GL — not used on the web
  set(BUILD_NOPENGLESDRV ON)      # GLES2 -> WebGL: the default web renderer (D3)
  set(BUILD_SOFTDRV ON)           # software renderer (portable non-x86 C path) (D3)
  set(BUILD_NULLDRV ON)           # null render device (GPU-free, no GL context) — -NULLRENDERER
  set(BUILD_NULLSOUNDDRV ON)      # keep the null AudioDevice as a selectable fallback
  set(BUILD_NOPENALDRV ON)        # ufront 2.12 (web audio): Emscripten's built-in OpenAL (SFX) + a
                                  # FetchContent'd libxmp (tracker music) — see NOpenALDrv/CMakeLists.txt
                                  # and NOpenALDrv.cpp NOPENAL_WEB. Reused for UT99's web audio port.
  # ufront 2.13.2 — -pthread must be a COMPILE option, not just a link option. It defines
  # __EMSCRIPTEN_PTHREADS__, which is what the threaded code paths (e.g. NSDLViewport's direct WebGL
  # context creation) are guarded on; with it only at link time those guards silently compile their
  # SINGLE-THREADED branch, SDL creates a GL context after all, and Emscripten proxies eglCreateContext to
  # a main thread whose canvas has been transferred away -> InvalidStateError (Emscripten #24792). It also
  # gives every object the atomics/bulk-memory features a shared-memory link expects.
  add_compile_options(-pthread)
  add_definitions(-DASM=0)        # Core/SoftDrv use their portable (non-x86) code paths
  # clang (Emscripten) makes passing a non-POD FString through variadic debugf/appErrorf a hard error;
  # GCC/MSVC tolerate it. Downgrade to a warning (these are mostly script-export/debug paths).
  add_compile_options(-Wno-error=non-pod-varargs -Wno-non-pod-varargs)
  # Exceptions: rely on Emscripten's default (exception CATCHING disabled). UE1 guards functions with
  # try/catch (DO_GUARD) and its error handler catches, but -fexceptions is unusable here — its JS invoke_
  # trampolines nest through the deeply-recursive package/object loader (UnrealI.u ~37 MB) until V8's
  # call-stack limit ("Maximum call stack size exceeded"), and -fwasm-exceptions is incompatible with
  # ASYNCIFY (which we need for the main-loop yield). With catching disabled there are no invoke_
  # trampolines: normal boot never throws, so it proceeds; error paths (appError) abort loudly instead of
  # being caught — acceptable for the web build. (This is why the 2.6a build compiled+ran before -fexceptions.)
endif()

if(VITA)
  set(BUILD_STATIC ON)
endif()

if(BUILD_STATIC)
  if(MSVC)
    message(FATAL_ERROR "Static mode is not supported with MSVC")
  endif()
  set(LIB_TYPE STATIC)
  add_definitions(-DUNREAL_STATIC)
  message(STATUS "Static mode enabled")
else()
  set(LIB_TYPE SHARED)
endif()

# Determine target arch and get normalized arch string

include(TargetArch)
target_architecture(TARGET_ARCH)

set(TARGET_IS_X86 FALSE)
set(TARGET_IS_ARM FALSE)
set(TARGET_IS_64BIT FALSE)
set(TARGET_IS_BIG_ENDIAN FALSE)

if(TARGET_ARCH MATCHES "arm")
  set(TARGET_IS_ARM TRUE)
  if(TARGET_ARCH MATCHES "64")
    # arm64/aarch64
    set(TARGET_IS_64BIT TRUE)
    set(TARGET_ARCH "arm64")
  else()
    # arm
    set(TARGET_ARCH "arm")
  endif()
elseif((TARGET_ARCH MATCHES "86") OR (TARGET_ARCH STREQUAL "ia64"))
  set(TARGET_IS_X86 TRUE)
  if(TARGET_ARCH MATCHES "64")
    # x86_64/amd64
    set(TARGET_IS_64BIT TRUE)
    set(TARGET_ARCH "x86_64")
  else()
    # x86
    set(TARGET_ARCH "x86")
  endif()
endif()

message(STATUS "Target arch: ${TARGET_ARCH}")
message(STATUS "64-bit: ${TARGET_IS_64BIT}")

if(TARGET_IS_64BIT)
  if(MSVC)
    # LLP64 (Windows x64) is not part of ufront 2.84: `long` is 32-bit there, which the typedefs cover, but
    # nothing has been built or run. Refuse rather than produce an unverified binary.
    message(FATAL_ERROR "64-bit MSVC builds are not supported. Use -A Win32.")
  elseif(TARGET_IS_X86 AND NOT UT_ALLOW_64BIT)
    message(STATUS "Building x86 binary with x86_64 compiler (pass -DUT_ALLOW_64BIT=ON for x86_64)")
    set(TARGET_IS_64BIT FALSE)
    set(TARGET_ARCH "x86")
    add_compile_options(-m32)
  else()
    message(STATUS "Building a 64-bit (LP64) engine for ${TARGET_ARCH}")
  endif()
endif()

# Determine target OS/platform

set(TARGET_IS_WINDOWS FALSE)
set(TARGET_IS_UNIX FALSE)
if(WIN32)
  set(TARGET_IS_WINDOWS TRUE)
else()
  # TODO
  set(TARGET_IS_UNIX TRUE)
endif()

message(STATUS "Windows target: ${TARGET_IS_WINDOWS}")

# Common compiler options

# (ufront 2.84: the PLATFORM_32BIT/PLATFORM_64BIT definitions that stood here were read by nothing. The
# width macro the code uses is PLATFORM_64BITS, derived from the compiler in Core/Inc/Core.h.)

if(TARGET_IS_X86)
  add_definitions(-DPLATFORM_X86)
elseif(TARGET_IS_ARM)
  add_definitions(-DPLATFORM_ARM)
  add_compile_options(-fsigned-char -fno-short-enums)
  add_link_options(-fno-short-enums)
  if(VITA)
    add_compile_options(-mno-unaligned-access -fno-optimize-sibling-calls)
    add_link_options(-mno-unaligned-access -Wl,-q)
  endif()
endif()

if(TARGET_IS_BIG_ENDIAN)
  add_definitions(-DPLATFORM_BIG_ENDIAN)
else()
  add_definitions(-DPLATFORM_LITTLE_ENDIAN)
endif()

if(TARGET_IS_WINDOWS)
  add_definitions(-DPLATFORM_WIN32)
  add_definitions(-DWIN32)
  add_definitions(-DWINDOWS_IGNORE_PACKING_MISMATCH)
  if(MINGW)
    # ufront 2.33 — link GCC's runtime statically. Without this the .exe imports libgcc_s_dw2-1.dll and
    # libstdc++-6.dll, which live in the toolchain (or MSYS2's /mingw32/bin), NOT in a Windows install:
    # the binary runs on the build machine and dies with "not found" everywhere else. With it, the only
    # imports left are stock system DLLs, so the static build really is a single self-contained file.
    add_link_options(-static-libgcc -static-libstdc++)
  endif()
else()
  # TODO
  add_definitions(-DPLATFORM_POSIX)
  if(VITA)
    add_definitions(-DPLATFORM_PSVITA)
  else()
    add_definitions(-DPLATFORM_CASE_SENSITIVE_FS)
    set(CMAKE_EXECUTABLE_SUFFIX ".bin")
  endif()
endif()

if(USE_SDL)
  add_definitions(-DPLATFORM_SDL)
endif()

if(MSVC)
  message(STATUS "Building ${CMAKE_BUILD_TYPE} with MSVC")
  add_definitions(-DPLATFORM_MSVC)
  add_compile_options(/nologo /fp:precise /analyze- /MP /MD /Gd /Gy /GS /Gm- /EHsc /Zi /Zc:wchar_t /Zc:inline /Zp4)
  add_compile_options(/wd4291 /wd4297)
  if(TARGET_IS_64BIT)
    set(MSVC_LIBDIR "Win64")
  else()
    set(MSVC_LIBDIR "Win32")
  endif()
else()
  message(STATUS "Building ${CMAKE_BUILD_TYPE} with GCC or Clang")
  if(EMSCRIPTEN)
    # ★ THE THREE WEB BUILD MODES (ufront 2.19d), identical in all four engine repos:
    #
    #   preset        UT_WEB_PERF  UT_WEB_DEBUG  compile   what it is for
    #   wasm-perf     ON           -             -O2 (*)   shipping: link-time -O3 -g0, no assertions
    #   wasm          OFF          OFF           -O2 -g    THE DEV DEFAULT: named stacks, DWARF, assertions
    #   wasm-debug    OFF          ON            -O0 -g    last resort: every local live, nothing inlined
    #
    # (*) This engine's perf build differs from the dev build at LINK only (see Source/Unreal/CMakeLists);
    #     ut2004 additionally compiles its perf build at -O3. That divergence is recorded, not fixed here —
    #     changing it would alter a shipping build's codegen and needs its own verification.
    #
    # -O0 is what you switch to when the OPTIMISER is the suspect: a bug that appears at one -O level and
    # not another is almost always undefined behaviour rather than a logic error. It is NOT the smaller
    # build — measured on ut2004, -O2 -g came out LARGER than -O0 -g, because DWARF forces Emscripten to
    # "run only limited binaryen optimizations" so -O2's inlining is never cleaned up. Speed is what the
    # dev default buys, not size.
    if(UT_WEB_DEBUG)
      add_compile_options(-O0 -g)
    elseif(UT_WEB_PERF)
      # UNCHANGED from before the three-mode split: the shipping build carries no compile-side DWARF.
      # (It is -O2 at compile and -O3 -g0 at link — see Unreal/CMakeLists.txt. Adding -g here would grow
      # every object and make emcc "run only limited binaryen optimizations".)
      add_compile_options(-O2)
    else()
      add_compile_options(-O2 -g)
    endif()
  # ★ ufront 2.54 — CHECKED, AND THIS ENGINE IS CLEAN; DO NOT "TIDY" IT. CMake injects
  # CMAKE_<LANG>_FLAGS_<CONFIG> ahead of anything add_compile_options() appends and the compiler takes
  # the LAST -O, which on engine-ut2004 silently compiled every native build at -O0 (its native branch
  # appended -O0 to every configuration). Here the appended level MATCHES the injected one, so a
  # RelWithDebInfo TU reads `-O2 … -O2` — redundant on the line, identical in effect. Measured with
  # compile_commands.json rather than reasoned about.
  #
  # ⚠ The per-config variables are deliberately NOT blanked here. They are where -DNDEBUG comes from on
  # this engine — the branches below never add it — so clearing them to remove a harmless duplicate -O
  # would switch assert() back on across the whole engine for no gain.
  elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-D_DEBUG)
    add_compile_options(-O0)
  else()
    add_compile_options(-O2)
  endif()
  add_compile_options(-ffunction-sections -fno-strict-aliasing)
  add_compile_options(-Wno-write-strings)
endif()

# Dependencies

if(USE_SDL)
  if(MSVC)
    if(NOT DEFINED SDL2_INCLUDE_DIR)
      set(SDL2_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../Thirdparty/SDL2/include")
    endif()
    if(NOT DEFINED SDL2_LIBRARY)
      set(SDL2_LIBRARY "${CMAKE_SOURCE_DIR}/../Thirdparty/SDL2/lib/${MSVC_LIBDIR}/SDL2.lib")
    endif()
  else()
    # Won't bother with find_package as we're probably cross compiling.
    if(NOT DEFINED SDL2_INCLUDE_DIR)
      set(SDL2_INCLUDE_DIR )
    endif()
    if(NOT DEFINED SDL2_LIBRARY)
      if(EMSCRIPTEN)
        # ufront 2.13.2 — the -sUSE_SDL=2 PORT supplies SDL2 (and picks the right variant per build:
        # libSDL2.a normally, libSDL2-mt.a under -pthread). An explicit -lSDL2 on top of that resolves to
        # the NON-threaded archive, and the threaded link then dies with
        #   "--shared-memory is disallowed by SDL_atomic.c.o because it was not compiled with 'atomics'"
        # because the two archives disagree about features. Leave it to the port.
        set(SDL2_LIBRARY )
      elseif(MINGW AND CMAKE_CROSSCOMPILING)
        # ufront 2.33 — cross-building for Windows from Linux: no system SDL2 to link against, so fetch
        # SDL2's official prebuilt MinGW binaries. Under MSYS2 on real Windows this branch is skipped
        # (not cross-compiling) and pacman's mingw-w64-i686-SDL2 answers -lSDL2 as usual.
        include(SDL2Mingw)
      else()
        set(SDL2_LIBRARY "-lSDL2")
      endif()
    endif()
  endif()
endif()

if(BUILD_NOPENALDRV)
  if(MSVC)
    if(NOT DEFINED OPENAL_INCLUDE_DIR)
      set(OPENAL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../Thirdparty/openal-soft/include")
    endif()
    if(NOT DEFINED OPENAL_LIBRARY)
      set(OPENAL_LIBRARY "${CMAKE_SOURCE_DIR}/../Thirdparty/openal-soft/lib/${MSVC_LIBDIR}/OpenAL32.lib")
    endif()
    if(NOT DEFINED LIBXMP_INCLUDE_DIR)
      set(LIBXMP_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../Thirdparty/libxmp-lite/include/libxmp-lite")
    endif()
    if(NOT DEFINED LIBXMP_LIBRARY)
      set(LIBXMP_LIBRARY "${CMAKE_SOURCE_DIR}/../Thirdparty/libxmp-lite/lib/${MSVC_LIBDIR}/libxmp-lite.lib")
    endif()
  else()
    # Won't bother with find_package as we're probably cross compiling.
    if(NOT DEFINED OPENAL_INCLUDE_DIR)
      set(OPENAL_INCLUDE_DIR )
    endif()
    if(NOT DEFINED OPENAL_LIBRARY)
      if(MINGW AND CMAKE_CROSSCOMPILING)
        # ufront 2.34 — cross-building for Windows: no system OpenAL to link against, so fetch OpenAL
        # Soft's official prebuilt Windows binaries (same pattern as SDL2Mingw). Real audio, because a
        # null audio subsystem is not a usable default.
        include(OpenALMingw)
      elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND UT_BUNDLED_OPENAL)
        # ufront 2.70: native Linux links the fork's own libopenal.so.1 (the `OpenAL` target, built below by
        # cmake/OpenALSoft.cmake NATIVE and copied beside the binary), not the system library.
        set(OPENAL_LIBRARY OpenAL)
      else()
        set(OPENAL_LIBRARY "-lopenal")
      endif()
    endif()
    if(NOT DEFINED LIBXMP_INCLUDE_DIR)
      set(LIBXMP_INCLUDE_DIR )
    endif()
    if(NOT DEFINED LIBXMP_LIBRARY)
      set(LIBXMP_LIBRARY "-lxmp")
    endif()
  endif()
endif()

# Subprojects

set(INSTALL_TARGETS Core Engine Render IpDrv Fire Unreal)

set(CMAKE_SHARED_LIBRARY_PREFIX "")

add_subdirectory(Core)
add_subdirectory(Engine)
add_subdirectory(Render)
add_subdirectory(IpDrv)
add_subdirectory(Fire)

# ufront step 2.6c — the WebRTC net driver is a WEB-ONLY package (its transport is the glue webrtc_* ABI).
if(EMSCRIPTEN)
  add_subdirectory(WebRTCDrv)
  list(APPEND INSTALL_TARGETS WebRTCDrv)
endif()

if(USE_SDL)
  # SDL2
  add_subdirectory(NSDLDrv)
  list(APPEND INSTALL_TARGETS NSDLDrv)
else()
  # archaic WinAPI shit
  set(BUILD_WINDRV ON)
  set(BUILD_SOFTDRV ON)
  set(BUILD_NOPENGLDRV OFF)
  set(BUILD_NOPENGLESDRV OFF)
endif()

if(BUILD_WINDRV)
  add_subdirectory(Window)
  add_subdirectory(WinDrv)
  list(APPEND INSTALL_TARGETS Window WinDrv)
endif()

if(BUILD_SOFTDRV)
  add_subdirectory(SoftDrv)
  list(APPEND INSTALL_TARGETS SoftDrv)
endif()

# ufront (null render device): NullDrv (no-op URenderDevice) — GPU-free, no GL context. Selected via
# -NULLRENDERER for a headless / "poor-man's dedicated server" run. Additive like SoftDrv.
if(BUILD_NULLDRV)
  add_subdirectory(NullDrv)
  list(APPEND INSTALL_TARGETS NullDrv)
endif()

if(BUILD_NOPENGLDRV)
  add_subdirectory(NOpenGLDrv)
  list(APPEND INSTALL_TARGETS NOpenGLDrv)
endif()

if(BUILD_NOPENGLESDRV)
  add_subdirectory(NOpenGLESDrv)
  list(APPEND INSTALL_TARGETS NOpenGLESDrv)
endif()

if(BUILD_NULLSOUNDDRV)
  add_subdirectory(SoundDrv)
  list(APPEND INSTALL_TARGETS SoundDrv)
endif()

if(BUILD_NOPENALDRV)
  if(EMSCRIPTEN)
    # ufront 2.61: openal-soft instead of Emscripten's built-in OpenAL (5.1/HRTF/EFX reverb on the web).
    # EAX OFF: NOpenALDrv never calls the EAX API — its "EAX" reverb is EFX's AL_EFFECT_EAXREVERB, which
    # openal-soft always has. openal-soft throws for every AL error, so its objects are -fexceptions and the
    # link enables catching (Unreal/CMakeLists.txt); the engine's own objects are unchanged (see above).
    include(OpenALSoft)
    ufront_openal_soft(PLATFORM WEB EAX OFF)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND UT_BUNDLED_OPENAL)
    # ufront 2.70: native Linux carries the same fork, SHARED, beside the binary (cmake/OpenALSoft.cmake) —
    # the system libopenal is whatever the distro built, and this way native plays through what the web does.
    include(OpenALSoft)
    ufront_openal_soft(PLATFORM NATIVE EAX OFF)
  endif()
  add_subdirectory(NOpenALDrv)
  list(APPEND INSTALL_TARGETS NOpenALDrv)
endif()

if(BUILD_EDITOR)
  # GUI requires WinDrv
  add_subdirectory(Editor)
  list(APPEND INSTALL_TARGETS Editor)
endif()

add_subdirectory(Unreal)

# assemble all modules and pdbs in one dir

set(INSTALL_FILES )

foreach(TGT ${INSTALL_TARGETS})
  list(APPEND INSTALL_FILES "$<TARGET_FILE:${TGT}>")
  if(MSVC)
    list(APPEND INSTALL_FILES "$<TARGET_PDB_FILE:${TGT}>")
  endif()
endforeach()

install(FILES ${INSTALL_FILES} DESTINATION "${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}")
