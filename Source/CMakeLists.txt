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
# Web build mode (Emscripten only; see the build-mode table below). UE_WEB_PERF sets the link flags in
# Unreal/CMakeLists.txt; UE_WEB_DEBUG compiles at -O0 instead of -O2.
option(UE_WEB_PERF "Emscripten: shipping build — link at -O3 -g0, assertions off" OFF)
option(UE_WEB_DEBUG "Emscripten: -O0 instead of the -O2 dev default (nothing inlined, every local live)" OFF)
# The Editor package is needed to play: game content (maps, meshes, skins) references its classes, and without
# it a map such as Dug.unr fails to load. It is the stripped, no-GUI NullEdHook variant (Engine+Core only).
option(BUILD_EDITOR "Build the Editor package (game content needs it at run time)" ON)
option(BUILD_SOFTDRV "Build SoftDrv (the software renderer; its portable C path builds everywhere)" OFF)
option(BUILD_NULLDRV "Build NullDrv (GPU-free no-op render device; -NULLRENDERER). No GL context -> headless / dedicated-server run." OFF)
option(BUILD_NOPENGLDRV "Build NOpenGLDrv" OFF)
option(BUILD_NOPENGLESDRV "Build NOpenGLESDrv" ON)
option(BUILD_NULLSOUNDDRV "Build SoundDrv (Null driver)" ON)
option(BUILD_NOPENALDRV "Build NOpenALDrv" ON)
option(UE_BUNDLED_OPENAL "Native Linux: build openal-soft (the ufront fork) and ship libopenal.so.1 beside the binary, instead of linking the system libopenal" ON)
option(BUILD_WINDRV "Build WinDrv" OFF)
option(BUILD_STATIC "Link everything into a single binary" OFF)
# A 64-bit (LP64) build. OFF demotes an x86_64 compiler to -m32; ON builds x86_64 as x86_64. An arm64 compiler
# always builds 64-bit. The C++ side does not read this option: its pointer-sized types come from the compiler.
option(UE_ALLOW_64BIT "Build x86_64 as a 64-bit (LP64) engine instead of demoting to -m32" OFF)

# Emscripten / WebAssembly build. Emscripten cannot load UE1's .so/.dll packages, has no x86 inline asm, and
# gets SDL2/GLES2 from its own ports; the web configuration is forced here, so `emcmake cmake` (or a wasm
# preset) needs no extra -D flags.
if(EMSCRIPTEN)
  set(BUILD_STATIC ON)            # single binary via UNREAL_STATIC + --whole-archive (like PSVita)
  set(USE_SDL ON)                 # NSDLDrv (SDL2 window/GL context -> WebGL)
  set(BUILD_EDITOR ON)            # the stripped Editor package, which game content needs (see BUILD_EDITOR)
  set(BUILD_WINDRV OFF)
  set(BUILD_NOPENGLDRV OFF)       # desktop GL — not used on the web
  set(BUILD_NOPENGLESDRV ON)      # GLES2 -> WebGL: the default web renderer
  set(BUILD_SOFTDRV ON)           # software renderer (portable non-x86 C path)
  set(BUILD_NULLDRV ON)           # null render device (GPU-free, no GL context) — -NULLRENDERER
  set(BUILD_NULLSOUNDDRV ON)      # keep the null AudioDevice as a selectable fallback
  set(BUILD_NOPENALDRV ON)        # openal-soft (cmake/OpenALSoft.cmake, below) + libxmp for tracker music
  # -pthread at compile time too: it defines __EMSCRIPTEN_PTHREADS__, which guards the threaded code paths
  # (NSDLViewport creates the WebGL context itself on the engine thread instead of through SDL's EGL), and
  # gives every object the atomics/bulk-memory features a shared-memory link needs.
  add_compile_options(-pthread)
  # The SDL2 port is requested at compile time too: Core/NSDLDrv include <SDL2/SDL.h>, which exists in the
  # Emscripten cache only once the port is installed.
  add_compile_options(-sUSE_SDL=2)
  add_definitions(-DASM=0)        # Core/SoftDrv use their portable (non-x86) code paths
  # clang (Emscripten) makes passing a non-POD FString through variadic debugf/appErrorf a hard error;
  # GCC/MSVC tolerate it. Silence it (these are mostly script-export/debug paths).
  add_compile_options(-Wno-error=non-pod-varargs -Wno-non-pod-varargs)
  # Exceptions: the engine's objects keep Emscripten's default (-fignore-exceptions, no invoke_ trampolines),
  # so UE1's try/catch (DO_GUARD, main's handler) never catches on the web: a throw (appError) propagates
  # uncaught. -fexceptions would nest invoke_ trampolines through the deeply recursive package loader past
  # V8's call-stack limit, and -fwasm-exceptions does not work with ASYNCIFY (the frame yield). The link
  # enables catching (-sNO_DISABLE_EXCEPTION_CATCHING, Unreal/CMakeLists.txt) for openal-soft's -fexceptions
  # objects.
endif()

# Android build: the Gradle project in Android/ configures this file with the NDK (Android/app/build.gradle). The
# game is one shared library, libUnreal.so, that SDL's activity loads: the static package set with NOpenGLESDrv on
# the device's OpenGL ES 2, SoftDrv and NullDrv, openal-soft (static, on OpenSL ES) and libxmp. SDL2 is fetched and
# built shared (cmake/SDL2Android.cmake), so the library and the Java classes the app compiles come from one checkout.
if(ANDROID)
  message(STATUS "Building for Android (${ANDROID_ABI}, API ${ANDROID_PLATFORM})")
  set(BUILD_STATIC ON)
  set(USE_SDL ON)
  set(BUILD_EDITOR ON)
  set(BUILD_WINDRV OFF)
  set(BUILD_NOPENGLDRV OFF)       # no desktop GL on Android
  set(BUILD_NOPENGLESDRV ON)
  set(BUILD_SOFTDRV ON)
  set(BUILD_NULLDRV ON)
  set(BUILD_NULLSOUNDDRV ON)
  set(BUILD_NOPENALDRV ON)
  add_definitions(-DASM=0)        # Core/SoftDrv use their portable (non-x86) code paths
  # As for Emscripten's clang above: a non-POD FString through variadic debugf is not an error.
  add_compile_options(-Wno-error=non-pod-varargs -Wno-non-pod-varargs)
  # Epic's code calls methods through null pointers on purpose (`if( this )` checks), which clang would otherwise
  # delete; on Android a null page faults.
  add_compile_options(-fno-delete-null-pointer-checks)
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
    # LLP64 (Windows x64): `long` is 32-bit there, which the typedefs cover, but the build is untested.
    # Refuse rather than produce an unverified binary.
    message(FATAL_ERROR "64-bit MSVC builds are not supported. Use -A Win32.")
  elseif(TARGET_IS_X86 AND NOT UE_ALLOW_64BIT AND NOT ANDROID)
    # (Android takes the width of its ABI: its x86_64 is a 64-bit engine like arm64.)
    message(STATUS "Building x86 binary with x86_64 compiler (pass -DUE_ALLOW_64BIT=ON for x86_64)")
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
    # Link GCC's runtime statically: otherwise the .exe imports libgcc_s_dw2-1.dll and libstdc++-6.dll,
    # which a Windows install does not have. The imports left are system DLLs and OpenAL32.dll.
    add_link_options(-static-libgcc -static-libstdc++)
    # A posix-thread-model MinGW (MSYS2, Arch/Fedora packages) also pulls in libwinpthread-1.dll through
    # libstdc++; link it statically too, or the exe does not start outside the build machine.
    add_link_options(-Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic)
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
    # The three web build modes:
    #
    #   preset        UE_WEB_PERF  UE_WEB_DEBUG  compile   what it is for
    #   wasm-perf     ON           -             -O2       shipping: link-time -O3 -g0, no assertions
    #   wasm          OFF          OFF           -O2 -g    the dev default: named stacks, DWARF, assertions
    #   wasm-debug    OFF          ON            -O0 -g    last resort: every local live, nothing inlined
    #
    # -O0 is for when the optimiser is the suspect: a bug that shows at one -O level and not another is almost
    # always undefined behaviour. It is not the smaller build: DWARF makes Emscripten run only limited binaryen
    # optimizations, so -O2's inlining is never cleaned up. The dev default buys speed, not size.
    if(UE_WEB_DEBUG)
      add_compile_options(-O0 -g)
    elseif(UE_WEB_PERF)
      # No compile-side DWARF in the shipping build: -O2 here, -O3 -g0 at link (Unreal/CMakeLists.txt).
      # -g would grow every object and make emcc run only limited binaryen optimizations.
      add_compile_options(-O2)
    else()
      add_compile_options(-O2 -g)
    endif()
  # CMake puts CMAKE_<LANG>_FLAGS_<CONFIG> ahead of the add_compile_options() flags and the compiler takes the
  # last -O, so the level added in these branches is the one in effect (a RelWithDebInfo TU reads `-O2 … -O2`).
  # The per-config variables are not blanked: they are where -DNDEBUG comes from, which these branches never add.
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
        # The -sUSE_SDL=2 port supplies SDL2 (libSDL2-mt.a under -pthread). An explicit -lSDL2 would resolve
        # to the non-threaded archive and fail the shared-memory link, so it is left to the port.
        set(SDL2_LIBRARY )
      elseif(MINGW AND CMAKE_CROSSCOMPILING)
        # Cross-building for Windows from Linux: no system SDL2 to link against, so fetch SDL2's official
        # prebuilt MinGW binaries. Under MSYS2 on Windows this branch is skipped (not cross-compiling) and
        # pacman's mingw-w64-i686-SDL2 answers -lSDL2.
        include(SDL2Mingw)
      elseif(ANDROID)
        # SDL2 from source, the shared libSDL2.so the app loads (cmake/SDL2Android.cmake).
        include(SDL2Android)
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
        # Cross-building for Windows: no system OpenAL to link against, so fetch OpenAL Soft's official
        # prebuilt Windows binaries (cmake/OpenALMingw.cmake).
        include(OpenALMingw)
      elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND UE_BUNDLED_OPENAL)
        # Native Linux links the fork's own libopenal.so.1 (the `OpenAL` target, built below by
        # cmake/OpenALSoft.cmake NATIVE and copied beside the binary), not the system library.
        set(OPENAL_LIBRARY OpenAL)
      elseif(ANDROID)
        # Android links the fork as a static archive into libUnreal.so (cmake/OpenALSoft.cmake ANDROID, below).
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

# The WebRTC net driver is a web-only package (its transport is the host's ue_net_* ABI).
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

# NullDrv: a no-op URenderDevice with no GL context, selected with -NULLRENDERER for a headless run.
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
    # The web plays through openal-soft (5.1, HRTF, EFX reverb). EAX OFF: NOpenALDrv never calls the EAX API;
    # its "EAX" reverb is EFX's AL_EFFECT_EAXREVERB, which openal-soft always has. openal-soft throws for every
    # AL error, so its objects are -fexceptions and the link enables catching (Unreal/CMakeLists.txt); the
    # engine's own objects keep the default (see the exceptions note above).
    include(OpenALSoft)
    ufront_openal_soft(PLATFORM WEB EAX OFF)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND UE_BUNDLED_OPENAL)
    # Native Linux carries the same fork as a shared library beside the binary (cmake/OpenALSoft.cmake), so it
    # plays through the same library as the web rather than the distro's.
    include(OpenALSoft)
    ufront_openal_soft(PLATFORM NATIVE EAX OFF)
  elseif(ANDROID)
    # Android links the same fork statically, playing through the NDK's OpenSL ES.
    include(OpenALSoft)
    ufront_openal_soft(PLATFORM ANDROID EAX OFF)
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

# The Android app's process set-up (Android/app/src/main/cpp/android_bridge.cpp): a small JNI library that
# GameActivity calls before SDL starts, to give the engine its game folder (UNREAL_ANDROID_BASEDIR, the working
# directory, $HOME).
if(ANDROID)
  add_library(unreal1_android_bridge SHARED ${CMAKE_SOURCE_DIR}/../Android/app/src/main/cpp/android_bridge.cpp)
  set_target_properties(unreal1_android_bridge PROPERTIES PREFIX "lib")
  target_link_libraries(unreal1_android_bridge PRIVATE android log)
endif()

# assemble all modules and pdbs in one dir

set(INSTALL_FILES )

foreach(TGT ${INSTALL_TARGETS})
  list(APPEND INSTALL_FILES "$<TARGET_FILE:${TGT}>")
  if(MSVC)
    list(APPEND INSTALL_FILES "$<TARGET_PDB_FILE:${TGT}>")
  endif()
endforeach()

install(FILES ${INSTALL_FILES} DESTINATION "${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}")
