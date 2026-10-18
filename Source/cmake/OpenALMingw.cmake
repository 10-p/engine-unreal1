# OpenALMingw.cmake — OpenAL Soft for the MinGW-w64 (Windows) build.
#
# A cross build from Linux has no OpenAL to link against, so this fetches OpenAL Soft's official prebuilt
# Windows binaries (nothing binary is vendored into git). They are a DLL plus its import library, so OpenAL
# is the one library the otherwise static Windows build ships beside the exe.
#
# Sets OPENAL_INCLUDE_DIR + OPENAL_LIBRARY, and defines OPENAL_MINGW_RUNTIME_DLL — the implementation DLL
# that has to sit next to the .exe. Callers copy it there (see Unreal/CMakeLists.txt).

set(OPENAL_MINGW_VERSION "1.25.2" CACHE STRING "OpenAL Soft release to fetch for the MinGW build")
set(OPENAL_MINGW_SHA256  "67a0c4b800bd860c93c04f38caf8cbe4875f9c84700ac430efc451f70e265434"
    CACHE STRING "SHA256 of openal-soft-<version>-bin.zip")

if(MINGW_TARGET_ARCH STREQUAL "x86_64")
  set(_OPENAL_MINGW_ARCHDIR "Win64")
else()
  set(_OPENAL_MINGW_ARCHDIR "Win32")
endif()

include(FetchContent)
FetchContent_Declare(OpenALMingw
  URL "https://github.com/kcat/openal-soft/releases/download/${OPENAL_MINGW_VERSION}/openal-soft-${OPENAL_MINGW_VERSION}-bin.zip"
  URL_HASH "SHA256=${OPENAL_MINGW_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
message(STATUS "OpenAL Soft (MinGW): fetching prebuilt ${OPENAL_MINGW_VERSION} for ${_OPENAL_MINGW_ARCHDIR}")
FetchContent_MakeAvailable(OpenALMingw)

set(_OPENAL_MINGW_ROOT "${openalmingw_SOURCE_DIR}")
if(NOT EXISTS "${_OPENAL_MINGW_ROOT}/libs/${_OPENAL_MINGW_ARCHDIR}/libOpenAL32.dll.a")
  message(FATAL_ERROR "OpenAL Soft (MinGW): libs/${_OPENAL_MINGW_ARCHDIR}/libOpenAL32.dll.a is missing — the zip layout changed.")
endif()

set(OPENAL_INCLUDE_DIR "${_OPENAL_MINGW_ROOT}/include")
set(OPENAL_LIBRARY     "${_OPENAL_MINGW_ROOT}/libs/${_OPENAL_MINGW_ARCHDIR}/libOpenAL32.dll.a")

# `soft_oal.dll` is the implementation; `router/` holds the pass-through OpenAL32.dll that dispatches to a
# system-installed driver. The implementation ships under the name the import library asks for.
# CACHE INTERNAL, not a plain variable, so the launcher's POST_BUILD copy (Unreal/CMakeLists.txt) sees it
# from whichever directory includes this file.
set(OPENAL_MINGW_RUNTIME_DLL "${_OPENAL_MINGW_ROOT}/bin/${_OPENAL_MINGW_ARCHDIR}/soft_oal.dll"
    CACHE INTERNAL "OpenAL Soft implementation DLL to ship beside the executable")
set(OPENAL_MINGW_RUNTIME_DLL_NAME "OpenAL32.dll"
    CACHE INTERNAL "Name the import library expects the OpenAL Soft DLL to have")
