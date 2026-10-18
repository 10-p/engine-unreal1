project(NOpenALDrv C CXX)

set(SRC_FILES "NOpenALDrv.cpp")

# libxmp (the tracker decoder for Unreal's .umx music) is fetched and built from source as a static library
# on the web, Linux and MinGW, so no system libxmp is needed; it is portable C. Depackers and ProWizard are
# disabled: Unreal music is raw modules.
if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Linux" OR (MINGW AND CMAKE_CROSSCOMPILING))
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
  # libxmp's own BUILD_STATIC option shares its name with the engine's. libxmp runs under CMP0077 OLD, so the
  # value must be in the cache before its option(): save the engine's cached value, force ON for libxmp's
  # configure and restore it after MakeAvailable. Left forced, the next reconfigure would switch the engine's
  # LIB_TYPE to STATIC (a single --whole-archive binary).
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
  # OpenAL. Native Linux keeps the parent's OPENAL_LIBRARY: the fork's bundled libopenal.so.1 when
  # UT_BUNDLED_OPENAL is ON (the default), else the system libopenal. MinGW keeps the prebuilt OpenAL Soft
  # import library from OpenALMingw.cmake.
  if(EMSCRIPTEN)
    # the openal-soft `OpenAL` target the parent builds with Source/cmake/OpenALSoft.cmake (EFX and HRTF included)
    set(OPENAL_LIBRARY OpenAL)
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
