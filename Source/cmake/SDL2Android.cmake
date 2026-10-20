# SDL2Android.cmake — SDL2 for the Android build: fetched and built as the shared libSDL2.so that SDLActivity loads
# before the game (the Gradle build compiles SDL's Java classes from the same checkout, which it hands over as
# FETCHCONTENT_SOURCE_DIR_SDL2, so the two halves of SDL always match). Sets SDL2_INCLUDE_DIR and SDL2_LIBRARY.
include(FetchContent)
set(SDL2_GIT_REPOSITORY "https://github.com/libsdl-org/SDL.git" CACHE STRING "SDL2 git remote")
set(SDL2_GIT_TAG "release-2.32.x" CACHE STRING "SDL2 git ref (a branch: a rebuild takes its tip)")
FetchContent_Declare(
  SDL2
  GIT_REPOSITORY ${SDL2_GIT_REPOSITORY}
  GIT_TAG        ${SDL2_GIT_TAG}
  GIT_SHALLOW    TRUE
)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
# SDL2main is a WinMain shim; on Android SDLActivity calls SDL_main itself.
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)  # SDL2's old cmake_minimum_required vs CMake 4.x
FetchContent_MakeAvailable(SDL2)
unset(CMAKE_POLICY_VERSION_MINIMUM)
if(NOT TARGET SDL2)
  message(FATAL_ERROR "SDL2: expected shared target 'SDL2' was not created")
endif()
# The engine includes "SDL2/SDL.h", as a system SDL2 is installed: a directory whose SDL2 entry is the checkout's
# include/ (its SDL_config.h picks SDL_config_android.h).
set(_sdl2_include_root "${CMAKE_BINARY_DIR}/sdl2-include")
file(MAKE_DIRECTORY "${_sdl2_include_root}")
file(CREATE_LINK "${sdl2_SOURCE_DIR}/include" "${_sdl2_include_root}/SDL2" SYMBOLIC COPY_ON_ERROR)
set(SDL2_INCLUDE_DIR "${_sdl2_include_root}")
set(SDL2_LIBRARY SDL2)
message(STATUS "SDL2 (shared, Android): ${SDL2_GIT_REPOSITORY}@${SDL2_GIT_TAG} in ${sdl2_SOURCE_DIR}")
