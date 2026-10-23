project(Unreal)

if(USE_SDL)
  set(SRC_FILES "Src/SDLLaunch.cpp")
else()
  set(SRC_FILES "Src/WinLaunch.cpp")
endif()

if(VITA)
  set(VITA_APP_NAME "Unreal")
  set(VITA_TITLEID  "UNRL00001")
  set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
  include("${VITASDK}/share/vita.cmake" REQUIRED)
endif()

if(TARGET_IS_WINDOWS)
  list(APPEND SRC_FILES "Src/Res/LaunchRes.rc")
  add_executable(${PROJECT_NAME} WIN32 ${SRC_FILES})
  # The .rc names its icon relative to itself; give the resource compiler that directory.
  set_source_files_properties("Src/Res/LaunchRes.rc" PROPERTIES
    COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/Src/Res")
elseif(ANDROID)
  # libUnreal.so: the library SDLActivity loads after libSDL2.so (GameActivity.getLibraries) and whose SDL_main it
  # calls. The root CMakeLists clears the shared-library prefix for the packages; the app loads this one by name.
  add_library(${PROJECT_NAME} SHARED ${SRC_FILES})
  set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "lib" OUTPUT_NAME "Unreal")
  # bionic has pthread and libm in libc; log is the logcat sink, android the native activity API SDL's video uses.
  target_link_libraries(${PROJECT_NAME} log android)
else()
  add_executable(${PROJECT_NAME} ${SRC_FILES})
endif()

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/Src
)

if(USE_SDL)
  # Under Emscripten, SDL2 comes from the -sUSE_SDL=2 port (link flags below) and NSDLDrv from the
  # --whole-archive static set; linking NSDLDrv again here would double-define its symbols, which wasm-ld rejects.
  if(NOT EMSCRIPTEN)
    target_link_libraries(${PROJECT_NAME} ${SDL2_LIBRARY})
    # A static build has NSDLDrv in the --whole-archive set below; listing its archive here too would make lld
    # (Android) define its objects twice.
    if(NOT BUILD_STATIC)
      target_link_libraries(${PROJECT_NAME} NSDLDrv)
    endif()
  endif()
  target_include_directories(${PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIR})
elseif(TARGET_IS_WINDOWS)
  target_link_libraries(${PROJECT_NAME} Window)
else()
  message(FATAL_ERROR "Non-Windows platforms can only use NSDLDrv.")
endif()

if(BUILD_STATIC)
  set(LINK_PACKAGES ${INSTALL_TARGETS})
  list(REMOVE_ITEM LINK_PACKAGES ${PROJECT_NAME})
  target_link_libraries(${PROJECT_NAME} -Wl,--whole-archive ${LINK_PACKAGES} -Wl,--no-whole-archive)
else()
  target_link_libraries(${PROJECT_NAME} Engine Core)
endif()

if(EMSCRIPTEN)
  # Emits a bare loader (SDLLaunch.{js,wasm,data}), no --shell-file: the host page injects the glue (file system,
  # storage, WebRTC) at run time. Baked in are only the thin --js-library forwarders: vendor/glue/net-shim.js,
  # vendor/glue/wasmfs-shim.js and msaa-backbuffer.js. Renderers: NOpenGLESDrv (GLES2 -> WebGL), SoftDrv and
  # NullDrv, in the --whole-archive static set. SDL2 comes from the Emscripten port.
  target_link_options(${PROJECT_NAME} PRIVATE
    -sASYNCIFY
    -sASYNCIFY_STACK_SIZE=262144
    # emscripten_sleep is WebRTCDrv's handshake wait; EM_ASYNC_JS imports (the frame wait) register themselves.
    "-sASYNCIFY_IMPORTS=['emscripten_sleep']"
    -sFULL_ES2=1
    -sUSE_SDL=2
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=268435456
    -sSTACK_SIZE=2097152
    -sFORCE_FILESYSTEM
    # WasmFS with a JS-implemented (JSImpl) backend: vendor/glue/wasmfs-shim.js forwards its file operations
    # to the host-injected Module.UE.fs.backend, which serves game files on demand and keeps their bytes in JS
    # typed arrays rather than in linear memory. -sFORCE_FILESYSTEM (above) is still needed: WasmFS omits the
    # JS FS.* API otherwise, and the host uses it for user data, mods and the file browser.
    #
    # The EXPORTED_FUNCTIONS entries let the host create the backend from JS.
    -sWASMFS
    # The worker-side bootstrap in wasmfs-shim.js is a __postset, emitted only if its $symbol is included.
    # Nothing references it, so it is included here; without it the first asset read fails.
    "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$ueWorkerBoot']"
    # The engine runs on a worker (-sPROXY_TO_PTHREAD): synchronous per-file reads from OPFS need
    # FileSystemSyncAccessHandle, which browsers expose only on a worker, and Atomics.wait cannot block the
    # browser main thread.
    #
    # -sOFFSCREENCANVASES_TO_PTHREAD transfers #canvas to that worker, so NSDLViewport.cpp creates the WebGL
    # context there directly (emscripten_webgl_create_context). SDL2 keeps the window and input; GL context
    # creation and the frame present bypass SDL, whose EGL path Emscripten proxies to the main thread, which no
    # longer has the canvas.
    #
    # -sASYNCIFY is the frame-wait mechanism (EM_ASYNC_JS in SDLLaunch.cpp).
    -pthread
    -sPROXY_TO_PTHREAD
    -sOFFSCREENCANVAS_SUPPORT
    # Quoted: CMake reads an unquoted '#' as the start of a comment.
    "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas"
    # The viewport creates the WebGL context with explicitSwapControl=1 + renderViaOffscreenBackBuffer=1 and
    # presents each frame with emscripten_webgl_commit_frame(); Emscripten implements both only under
    # -sOFFSCREEN_FRAMEBUFFER. Without it commit_frame is a stub and presentation relies on the worker's task
    # ending, which on Windows/D3D11 shows only a fraction of the frames. A built SDLLaunch.js contains
    # blitOffscreenFramebuffer.
    -sOFFSCREEN_FRAMEBUFFER
    # Workers created up front: main() runs on one, openal-soft starts two (the web backend's mixer thread and
    # each context's event thread), one is spare; NOpenALDrv starts no music thread on the web. A thread created
    # with the pool empty still starts, but only once the main thread has spun up a worker.
    -sPTHREAD_POOL_SIZE=4
    # openal-soft reports every AL error by throwing and catching it, so catching is on (the first AL error
    # would otherwise abort). Only its objects are -fexceptions; see Source/CMakeLists.txt.
    -sNO_DISABLE_EXCEPTION_CATCHING
    "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_close','_ue_exec_command','_wasmfs_create_jsimpl_backend','_wasmfs_create_file','_ufront_wasmfs_register','_ufront_wasmfs_set_pending']"
    # PThread: the host needs the worker list to hand the file system's sync channel to the engine thread.
    # Runtime methods are on Module only when exported.
    "-sEXPORTED_RUNTIME_METHODS=['callMain','stringToUTF8','UTF8ToString','HEAPU8','stackSave','stackAlloc','stackRestore','lengthBytesUTF8','PThread']"
    # net-shim.js defines the ue_net_* functions WebRTCDrv imports (vendor/glue/ue_abi.h) and forwards them to
    # the host-injected Module.UE.net.*; wasmfs-shim.js is the WasmFS backend's forwarder (above).
    "SHELL:--js-library ${CMAKE_SOURCE_DIR}/../vendor/glue/net-shim.js"
    "SHELL:--js-library ${CMAKE_SOURCE_DIR}/../vendor/glue/wasmfs-shim.js"
    # WebGL 2 (NOpenGLESDrv's GLES2 calls run on it unchanged), for the multisampled emulated back buffer
    # (msaa-backbuffer.js: ufront_backbuffer_samples) that is NOpenGLESDrv's antialiasing; WebGL 1 stays the fallback
    # of a browser without WebGL 2 (NSDLViewport.cpp).
    -sMAX_WEBGL_VERSION=2
    -sMIN_WEBGL_VERSION=1
    "SHELL:--js-library ${CMAKE_CURRENT_SOURCE_DIR}/msaa-backbuffer.js"
  )
  if(UE_WEB_PERF)
    # --emit-symbol-map writes a side file (SDLLaunch.js.symbols) and leaves the wasm as it is; with it a crash
    # offset in the shipped wasm resolves to a function name offline. DWARF cannot ride along (it limits
    # binaryen's optimizations), so line numbers stay a debug-build job.
    # The map names every private engine symbol, so it must never reach the published tree.
    target_link_options(${PROJECT_NAME} PRIVATE -O3 -g0 -sASSERTIONS=0 --emit-symbol-map)
  else()
    target_link_options(${PROJECT_NAME} PRIVATE -g -sASSERTIONS=1)
  endif()
  # Preload the boot ini so appInit has config before the host's file system serves the rest. It is the
  # tracked web set (Engine/Config/web), not a game install's ini, which lacks the web-only
  # [WebRTCDrv.WebRTCNetDriver] section; Engine/Config/*.ini (no subdirectory) is the native reference set.
  # appInit derives Unreal.ini from it, and the asset manifest excludes that name so an install copy cannot
  # shadow it.
  if(EXISTS "${CMAKE_SOURCE_DIR}/../Engine/Config/web/Default.ini")
    set_property(TARGET ${PROJECT_NAME} APPEND_STRING PROPERTY LINK_FLAGS
      " --preload-file ${CMAKE_SOURCE_DIR}/../Engine/Config/web/Default.ini@/System/Default.ini")
  endif()
  # The preloaded ini is a link input CMake does not infer from the flag: LINK_DEPENDS relinks when it changes.
  set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_SOURCE_DIR}/../Engine/Config/web/Default.ini")
  # So are the --js-library files.
  set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_SOURCE_DIR}/../vendor/glue/net-shim.js"
    "${CMAKE_SOURCE_DIR}/../vendor/glue/wasmfs-shim.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/msaa-backbuffer.js")
  # Bare loader: SDLLaunch.js (+ .wasm/.data), no .html, in build-<cfg>/SDLLaunch/, the artifact path the
  # host's build derives per target.
  set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME SDLLaunch
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/SDLLaunch)

  # 24-bit depth for the emulated default framebuffer: Emscripten allocates -sOFFSCREEN_FRAMEBUFFER's depth as
  # GL_DEPTH_COMPONENT16 whatever the context attributes say (z-fighting in bands on NOpenGLESDrv).
  # offscreen-depth24.sh rewrites the emitted loader to D24S8 and asserts the rewrite count, so a toolchain
  # change fails the build. POST_BUILD, so it re-runs on every relink.
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/offscreen-depth24.sh" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SDLLaunch.js"
    COMMENT "offscreen-depth24: D24S8 for the emulated default framebuffer")

  # Emit the artifact descriptor meta.json, which the host's build validates (schema, abiVersion against the
  # glue's ABI_VERSION, checksums, loaderStyle). abiVersion is read from the vendored vendor/glue/ue_abi.h;
  # provenance and toolchain are best-effort.
  file(STRINGS ${CMAKE_SOURCE_DIR}/../vendor/glue/ue_abi.h _ue_abi_line REGEX "define[ \t]+UE_ABI_VERSION")
  string(REGEX MATCH "([0-9]+)" UE_ABI_VERSION "${_ue_abi_line}")
  execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR}/.. rev-parse --short HEAD
    OUTPUT_VARIABLE META_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  # The Emscripten version comes from the toolchain's EMSCRIPTEN_ROOT_PATH: a preset build has only $EMSDK
  # set, so emcc need not be on PATH.
  set(META_EMCC "unknown")
  if(EXISTS "${EMSCRIPTEN_ROOT_PATH}/emscripten-version.txt")
    file(READ "${EMSCRIPTEN_ROOT_PATH}/emscripten-version.txt" _emcc_ver)
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" META_EMCC "${_emcc_ver}")
  endif()
  # Every fetched library as repo@branch|tag, from the cache variables the fetch itself read
  # (the web build always has NOpenALDrv, hence openal-soft and libxmp).
  include(${CMAKE_SOURCE_DIR}/cmake/ArtifactDeps.cmake)
  set(META_DEPS)
  ufront_artifact_dep(META_DEPS openal-soft OpenALSoft "${OPENALSOFT_GIT_REPOSITORY}" "${OPENALSOFT_GIT_TAG}")
  ufront_artifact_dep(META_DEPS libxmp libxmp "${LIBXMP_GIT_REPOSITORY}" "${LIBXMP_GIT_TAG}" "${LIBXMP_SOURCE_DIR}")
  find_program(NODE_EXECUTABLE node REQUIRED)
  # The emitter is an input to the artifact that CMake does not infer from a POST_BUILD command: LINK_DEPENDS
  # relinks when it changes, which re-runs POST_BUILD.
  set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/emit-artifact-meta.mjs")
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/emit-artifact-meta.mjs
      --out=$<TARGET_FILE_DIR:${PROJECT_NAME}>
      --target=unreal1 --abi=${UE_ABI_VERSION}
      --gitSha=${META_GIT_SHA} --emcc=${META_EMCC}
      ${META_DEPS}
    COMMENT "Emitting artifact meta.json"
    VERBATIM
  )
endif()

if(VITA)
  target_link_libraries(${PROJECT_NAME}
    -Wl,--whole-archive pthread -Wl,--no-whole-archive
    openal
    xmp
    vitaGL
    vitashark
    mathneon
    SceShaccCgExt
    taihen_stub
    SceNet_stub
    SceNetCtl_stub
    SceAppMgr_stub
    SceAppUtil_stub
    SceAudio_stub
    SceAudioIn_stub
    SceCtrl_stub
    SceCommonDialog_stub
    SceDisplay_stub
    SceFios2_stub
    SceGxm_stub
    SceIme_stub
    SceShaccCg_stub
    SceSysmodule_stub
    ScePower_stub
    SceTouch_stub
    SceMotion_stub
    SceHid_stub
    SceVshBridge_stub
    SceSblSsMgr_stub
    SceKernelDmacMgr_stub
  )
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE UNREAL_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})

# Tell the launcher whether NOpenGLDrv is linked, so its -*RENDERER switches and preference order offer it only
# then. Keyed off the CMake option rather than the platform.
if(BUILD_NOPENGLDRV)
  target_compile_definitions(${PROJECT_NAME} PRIVATE UE_HAS_NOPENGLDRV=1)
endif()

# OpenAL Soft is the one library the Windows build ships as a DLL (cmake/OpenALMingw.cmake). The build puts it
# beside the .exe, so the output directory is the runnable set.
if(OPENAL_MINGW_RUNTIME_DLL)
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENAL_MINGW_RUNTIME_DLL}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${OPENAL_MINGW_RUNTIME_DLL_NAME}"
    COMMENT "Copying ${OPENAL_MINGW_RUNTIME_DLL_NAME} (OpenAL Soft) next to the executable")
endif()

# Native Linux: the bundled openal-soft (cmake/OpenALSoft.cmake NATIVE) is DT_NEEDED by NOpenALDrv, so the
# binary finds it through $ORIGIN and the build puts it there, soname and all. A run tree that takes the
# binary takes libopenal.so.1 with it; without the file the loader stops before main.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT EMSCRIPTEN AND NOT ANDROID AND TARGET OpenAL)
  set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:OpenAL>"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/$<TARGET_SONAME_FILE_NAME:OpenAL>"
    COMMENT "Copying $<TARGET_SONAME_FILE_NAME:OpenAL> (openal-soft, the ufront fork) next to the executable")
endif()

if(VITA)
  vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
  vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/Res/sce_sys sce_sys
  )
endif()
