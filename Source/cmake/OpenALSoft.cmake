# OpenALSoft.cmake — openal-soft, the ufront fork, built from source for the web and for native Linux.
#
#   ufront_openal_soft(PLATFORM <WEB|NATIVE> EAX <ON|OFF>)   -> defines the target `OpenAL`
#     WEB    a STATIC archive linked into the wasm, mixed on its own pthread and played by an AudioWorklet
#            (the `web` backend on the fork's ufront/web branch): 5.1/7.1 where the device has it, HRTF on
#            request, EFX reverb, the launcher's audio buffer settings (Module.alsoftConfig).
#     NATIVE a SHARED libopenal.so.1 that NOpenALDrv links (DT_NEEDED) and the build copies beside the
#            binary, found through $ORIGIN, so native plays through the same library as the web rather than
#            the distro's. Its backends are whatever the host's headers allowed at configure time, each
#            dlopen'd at run time.
#     ANDROID a STATIC archive linked into libUnreal.so, on the NDK's OpenSL ES (its one backend).
#
# The fork: https://github.com/10-p/openal-soft-web `ufront/web` = openal-soft 1.25.2 + the Web Audio backend
# + the wasm build patches (all under __EMSCRIPTEN__). A branch, so a rebuild takes its tip
# (cmake/ArtifactDeps.cmake records it in meta.json).
#
# What the function takes care of:
#  1. Flags. -fpack-struct=4 and -Wno-address-of-packed-member are removed from every openal-soft target's
#     compile options, and four clang-compat flags from CMAKE_CXX_FLAGS for its configure (this engine sets
#     none of them). openal-soft pins its own CXX_STANDARD 20 per target.
#  2. Exceptions (web). openal-soft reports every AL error by throwing and catching it at the API boundary,
#     so its objects are compiled with -fexceptions and the engine link adds -sNO_DISABLE_EXCEPTION_CATCHING;
#     the engine's own objects keep Emscripten's default -fignore-exceptions (no invoke_ trampolines through
#     the package loader). Wasm-native exceptions cannot coexist with ASYNCIFY. Natively g++ has exceptions on.
#  3. SIMD (web). Its SSE mixers compile through Emscripten's translation headers (-msimd128 -msse4.1).
#     NEON is OFF: its header check passes on Emscripten and the runtime would prefer it. Natively the
#     mixers are selected by cpuid at run time.
#
# Call it after the engine's global compile options (a shared-memory web link needs atomics/bulk-memory on
# every object) and before the target that links `OpenAL`.
function(ufront_openal_soft)
  cmake_parse_arguments(ARG "" "PLATFORM;EAX" "" ${ARGN})
  if(NOT ARG_EAX)
    set(ARG_EAX OFF)
  endif()
  if(NOT ARG_PLATFORM MATCHES "^(WEB|NATIVE|ANDROID)$")
    message(FATAL_ERROR "ufront_openal_soft: PLATFORM must be WEB, NATIVE or ANDROID (got '${ARG_PLATFORM}')")
  endif()
  include(FetchContent)
  set(OPENALSOFT_GIT_REPOSITORY "https://github.com/10-p/openal-soft-web.git"
      CACHE STRING "openal-soft git remote; its ufront/web branch carries the Web Audio backend")
  set(OPENALSOFT_GIT_TAG "ufront/web" CACHE STRING "openal-soft git ref (a branch: a rebuild takes its tip)")
  FetchContent_Declare(
    OpenALSoft
    GIT_REPOSITORY ${OPENALSOFT_GIT_REPOSITORY}
    GIT_TAG        ${OPENALSOFT_GIT_TAG}
    GIT_SHALLOW    TRUE
  )
  # Function scope: these reach the subproject's configure and die with the function.
  string(REGEX REPLACE "-fdelayed-template-parsing|-Wno-writable-strings|-Wno-reserved-user-defined-literal|-Wno-incompatible-pointer-types"
         "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  if(ARG_PLATFORM STREQUAL "WEB")
    string(APPEND CMAKE_CXX_FLAGS " -msimd128 -msse4.1 -fexceptions")
    string(APPEND CMAKE_C_FLAGS " -msimd128 -msse4.1")
    set(LIBTYPE STATIC CACHE STRING "" FORCE)
    set(ALSOFT_DLOPEN OFF CACHE BOOL "" FORCE)
    set(ALSOFT_CPUEXT_NEON OFF CACHE BOOL "" FORCE)
    set(ALSOFT_BACKEND_WEB ON CACHE BOOL "" FORCE)
    set(ALSOFT_REQUIRE_SSE4_1 ON CACHE BOOL "" FORCE)  # fail rather than fall back to the slower C mixers
  elseif(ARG_PLATFORM STREQUAL "ANDROID")
    set(LIBTYPE STATIC CACHE STRING "" FORCE)
    set(ALSOFT_DLOPEN OFF CACHE BOOL "" FORCE)
    set(ALSOFT_BACKEND_OPENSL ON CACHE BOOL "" FORCE)
    set(ALSOFT_REQUIRE_OPENSL ON CACHE BOOL "" FORCE)
    # The desktop backends never probe the NDK's sysroot for headers it does not have.
    foreach(_al_be PIPEWIRE PULSEAUDIO ALSA OSS JACK SNDIO)
      set(ALSOFT_BACKEND_${_al_be} OFF CACHE BOOL "" FORCE)
    endforeach()
    # No C++20 modules: the NDK's clang rejects libc++'s range adaptors instantiated across a module boundary.
    # No RTKit: Android has no D-Bus.
    set(ALSOFT_ENABLE_MODULES OFF CACHE BOOL "" FORCE)
    set(ALSOFT_RTKIT OFF CACHE BOOL "" FORCE)
  else()
    set(LIBTYPE SHARED CACHE STRING "" FORCE)           # -> libopenal.so.1, beside the binary
    set(ALSOFT_DLOPEN ON CACHE BOOL "" FORCE)           # backends load libpulse/libasound at run time
  endif()
  foreach(_al_opt ALSOFT_UTILS ALSOFT_EXAMPLES ALSOFT_TESTS ALSOFT_INSTALL ALSOFT_INSTALL_CONFIG
          ALSOFT_INSTALL_HRTF_DATA ALSOFT_INSTALL_AMBDEC_PRESETS ALSOFT_BACKEND_WAVE
          ALSOFT_UPDATE_BUILD_VERSION)
    set(${_al_opt} OFF CACHE BOOL "" FORCE)
  endforeach()
  set(ALSOFT_EAX ${ARG_EAX} CACHE BOOL "" FORCE)
  set(ALSOFT_EMBED_HRTF_DATA ON CACHE BOOL "" FORCE)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
  FetchContent_MakeAvailable(OpenALSoft)

  # Remove the packing flags (see 1. above) from every target in openal-soft's tree, subdirectories included
  # (its bundled fmt is one).
  set(_al_dirs "${openalsoft_SOURCE_DIR}")
  while(_al_dirs)
    list(POP_FRONT _al_dirs _dir)
    get_property(_subdirs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
    list(APPEND _al_dirs ${_subdirs})
    get_property(_al_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t ${_al_targets})
      get_target_property(_type ${_t} TYPE)
      if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
        continue()
      endif()
      get_target_property(_opts ${_t} COMPILE_OPTIONS)
      if(_opts)
        list(REMOVE_ITEM _opts -fpack-struct=4 -Wno-address-of-packed-member)
        set_target_properties(${_t} PROPERTIES COMPILE_OPTIONS "${_opts}")
      endif()
    endforeach()
  endwhile()
  if(NOT TARGET OpenAL)
    message(FATAL_ERROR "openal-soft: expected target 'OpenAL' was not created")
  endif()
  if(ARG_PLATFORM STREQUAL "NATIVE")
    # The engine sets an empty CMAKE_SHARED_LIBRARY_PREFIX (Engine.so, NOpenALDrv.so) and the subproject
    # inherits it, which would name this `openal.so.1`; give it back its `lib` prefix.
    set_target_properties(OpenAL PROPERTIES PREFIX "lib")
  endif()
  message(STATUS "openal-soft (${ARG_PLATFORM}): ${OPENALSOFT_GIT_REPOSITORY}@${OPENALSOFT_GIT_TAG}, EAX=${ARG_EAX}")
endfunction()
