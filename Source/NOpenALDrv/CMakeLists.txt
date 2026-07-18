project(NOpenALDrv C CXX)

set(SRC_FILES "NOpenALDrv.cpp")

# ufront 2.12 (web audio): on Emscripten, OpenAL is the engine-provided built-in (Web Audio-backed, core
# 1.1 only — the driver compiles out EFX/HRTF under NOPENAL_WEB) linked via -lopenal. libxmp (the tracker
# decoder for Unreal's .umx music) is NOT shipped by Emscripten, so fetch + build it as a static wasm
# archive. libxmp is portable C (LANGUAGES C) -> cross-compiles cleanly under emcc. Depackers/ProWizard are
# disabled (Unreal music is raw modules; depackers pull archive deps + bloat the wasm). NOTE: libxmp's
# BUILD_STATIC option collides by name with the engine's own BUILD_STATIC ("single binary"); under libxmp's
# capped policy (CMP0077 OLD) its option() would reset the inherited value and disable the static build
# (-> its "both disabled" FATAL_ERROR), so we force the cache var for its configure and restore the
# engine's afterward (see the save/restore around BUILD_STATIC below). UT99's NOpenALDrv carries the
# identical recipe + fix.
#
# ufront 2.12 Task B (native audio parity): this box ships no system libxmp (-lxmp fails), so build it
# from source via cmake on NATIVE Linux too (portable C -> compiles fine at -m32). OpenAL stays the
# system libopenal on native (the parent's -lopenal); only the FetchContent'd xmp_static differs from web.
if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(FetchContent)
  set(LIBXMP_SOURCE_DIR "" CACHE PATH "Local libxmp checkout. Empty => fetch from git.")
  set(LIBXMP_GIT_REPOSITORY "https://github.com/libxmp/libxmp.git" CACHE STRING "Upstream libxmp git repo")
  set(LIBXMP_GIT_TAG "libxmp-4.7.1" CACHE STRING "libxmp git tag/commit to fetch (pin for reproducibility)")
  if(LIBXMP_SOURCE_DIR)
    message(STATUS "libxmp: using local source ${LIBXMP_SOURCE_DIR}")
    FetchContent_Declare(libxmp SOURCE_DIR "${LIBXMP_SOURCE_DIR}")
  else()
    message(STATUS "libxmp: fetching ${LIBXMP_GIT_REPOSITORY}@${LIBXMP_GIT_TAG}")
    FetchContent_Declare(libxmp GIT_REPOSITORY "${LIBXMP_GIT_REPOSITORY}" GIT_TAG "${LIBXMP_GIT_TAG}" GIT_SHALLOW TRUE)
  endif()
  # libxmp's BUILD_STATIC (its own option) collides with the engine's BUILD_STATIC. Save the engine's
  # cached value, force ON for libxmp's configure, and RESTORE it after MakeAvailable (below) — otherwise
  # the FORCE persists and poisons the engine on the next *reconfigure*: the top-level if(BUILD_STATIC)
  # flips LIB_TYPE to STATIC (native build unexpectedly links --whole-archive/single-binary). libxmp runs
  # under CMP0077 OLD, so the value must be in the cache before its option() (a normal variable won't take).
  set(_UFRONT_SAVED_BUILD_STATIC "$CACHE{BUILD_STATIC}")
  set(BUILD_STATIC ON  CACHE BOOL "" FORCE)   # libxmp static lib (restored after MakeAvailable)
  set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(BUILD_LITE   OFF CACHE BOOL "" FORCE)
  set(LIBXMP_DISABLE_DEPACKERS ON CACHE BOOL "" FORCE)
  set(LIBXMP_DISABLE_PROWIZARD ON CACHE BOOL "" FORCE)
  set(WITH_UNIT_TESTS OFF CACHE BOOL "" FORCE)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)  # libxmp's old cmake_minimum_required vs CMake 4.x
  FetchContent_MakeAvailable(libxmp)
  unset(CMAKE_POLICY_VERSION_MINIMUM)
  # Restore the engine's BUILD_STATIC so a reconfigure of this build dir isn't poisoned by libxmp's FORCE.
  set(BUILD_STATIC "${_UFRONT_SAVED_BUILD_STATIC}" CACHE BOOL "Link everything into a single binary" FORCE)
  if(NOT TARGET xmp_static)
    message(FATAL_ERROR "libxmp: expected static target 'xmp_static' was not created.")
  endif()
  # Web: Emscripten's built-in OpenAL. Native Linux: keep the parent's system -lopenal. Both: fetched libxmp.
  if(EMSCRIPTEN)
    set(OPENAL_LIBRARY "-lopenal")
  endif()
  set(LIBXMP_LIBRARY xmp_static)
endif()

add_library(${PROJECT_NAME} ${LIB_TYPE} ${SRC_FILES})

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
  PRIVATE
  ${OPENAL_INCLUDE_DIR}
  ${LIBXMP_INCLUDE_DIR}
)

if(MSVC)
  # openal-soft release libs are built without safeseh for some reason?
  target_link_options(${PROJECT_NAME} PRIVATE "/SAFESEH:NO")
endif()

target_link_libraries(${PROJECT_NAME} ${OPENAL_LIBRARY} ${LIBXMP_LIBRARY})

target_link_libraries(${PROJECT_NAME} Engine Core)

target_compile_definitions(${PROJECT_NAME} PRIVATE NOPENALDRV_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})
