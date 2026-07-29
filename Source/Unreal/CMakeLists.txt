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
  # ufront 2.6a (web): under Emscripten, SDL2 comes from the -sUSE_SDL=2 port (link flags below) and
  # NSDLDrv is pulled in via the --whole-archive static set — linking it again here double-defines every
  # NSDLDrv symbol (GNU ld tolerates it; wasm-ld does not).
  if(NOT EMSCRIPTEN)
    target_link_libraries(${PROJECT_NAME} ${SDL2_LIBRARY} NSDLDrv)
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
  # ufront step 2.6a/2.6b (web port) — emit a BARE LOADER (SDLLaunch.{js,wasm,data}); no --shell-file. The
  # monorepo owns the host page and injects the heavy glue (FetchFS/IDB/WebRTC) at runtime; only the thin
  # webrtc_* shim is baked (vendor/glue/webrtc-shim.js). Renderer = NOpenGLESDrv (GLES2->WebGL) + SoftDrv
  # (both in the --whole-archive static set). SDL2 comes from the Emscripten port, not system libs.
  set(UNREAL_ASSETS_DIR "/home/ion/unreal-assets" CACHE PATH
      "Unreal v200 assets (System/*.ini preloaded into MEMFS; the rest streams via FetchFS)")
  target_link_options(${PROJECT_NAME} PRIVATE
    -sASYNCIFY
    -sASYNCIFY_STACK_SIZE=262144
    # emscripten_sleep/set_main_loop = main-loop inversion (the live Asyncify use). __syscall_openat is
    # listed for FetchFS on-demand open (kept ABI-consistent with engine-ut2004; the open is sync-XHR).
    "-sASYNCIFY_IMPORTS=['emscripten_sleep','emscripten_set_main_loop','__syscall_openat']"
    -sFULL_ES2=1
    -sUSE_SDL=2
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=268435456
    -sSTACK_SIZE=2097152
    -sFORCE_FILESYSTEM
    # ufront 2.13.1 — WasmFS + the JS-implemented FS backend (ported from engine-ut2004, the pilot).
    #
    # -sWASMFS moves the VFS into wasm, which BREAKS FetchFS's old seam: it intercepted file access by
    # monkey-patching the JS FS.open (legal only because the legacy filesystem implements
    # __syscall_openat in JS), and under WasmFS a C open() never routes through it. The replacement is a
    # WasmFS JSImpl backend whose file ops are implemented in JS (vendor/glue/wasmfs-shim.js forwards
    # them to the host-injected Module.UE.fs.backend), which is both the on-demand hook and the way to
    # keep asset bytes in JS typed arrays instead of a std::vector inside linear memory.
    #
    # -sFORCE_FILESYSTEM (above) stays REQUIRED — WasmFS omits the JS FS.* API otherwise, and the host
    # glue still uses it for user data, mods and the file browser.
    #
    # The EXPORTED_FUNCTIONS entries let the HOST stand the backend up entirely from JS, so this block
    # plus the vendored shim is the whole per-engine cost — no C++ in any engine repo.
    # NB: no _webrtc_deferred_open in this list — the UE1 engines deliberately do not use the
    # deferred-connect pair (see WebRTCDrv.cpp); it is a ut2004 thing. Listing an undefined symbol is a
    # hard wasm-ld error: 'symbol exported via --export not found'.
    -sWASMFS
    # The worker-side bootstrap in wasmfs-shim.js hangs off a __postset, and a postset is only EMITTED if
    # its $symbol is referenced. Nothing references it (it is pure side effect), so without this it is
    # dead-stripped SILENTLY — no error, no warning, and the engine dies on its first asset read.
    "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$ueWorkerBoot']"
    # ufront 2.13.2 — run the engine on a WORKER (threaded-only, decision D2).
    #
    # WHY: synchronous per-file restore-on-read from OPFS (2.13.4, the actual point of stage 2.13) needs
    # FileSystemSyncAccessHandle, which browsers expose ONLY on a worker; and a thread blocking on
    # Atomics.wait cannot do so on the browser main thread. So main() has to leave the main thread.
    #
    # -sOFFSCREENCANVASES_TO_PTHREAD transfers #canvas to that worker, which is what makes the direct
    # emscripten_webgl_create_context in NSDLViewport.cpp legal there. SDL2 keeps window + input; only
    # GL-context creation and the frame present are taken off SDL (its GL path is EGL, and Emscripten
    # proxies EGL to a main thread that can no longer see the transferred canvas — Emscripten #24792).
    #
    # -sASYNCIFY deliberately STAYS this step: dropping it is 2.13.3, after the worker move is proven.
    -pthread
    -sPROXY_TO_PTHREAD
    -sOFFSCREENCANVAS_SUPPORT
    # QUOTED: CMake treats an unquoted '#' as a comment, which silently truncated this to
    # "OFFSCREENCANVASES_TO_PTHREAD=" and failed the link with "string index out of range".
    "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas"
    # main() itself is one of these; the engine spawns no others on the web (UnThread's music thread is
    # NOPENAL_WEB-guarded off), so a small pool avoids paying for workers nothing will use.
    -sPTHREAD_POOL_SIZE=2
    "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_close','_ue_exec_command','_wasmfs_create_jsimpl_backend','_wasmfs_create_file','_ufront_wasmfs_register','_ufront_wasmfs_set_pending']"
    # PThread (2.13.2): the host needs the worker list to hand the FS sync channel to the engine thread.
    # Runtime methods are NOT on Module unless exported — omitting it makes the host silently no-op.
    "-sEXPORTED_RUNTIME_METHODS=['callMain','stringToUTF8','UTF8ToString','HEAPU8','stackSave','stackAlloc','stackRestore','lengthBytesUTF8','PThread']"
    # The thin webrtc_* shim (step 2.6b): defines the ABI's C-imported symbols, forwarding to the
    # host-injected Module.UE.net.*. Heavy glue is NOT baked. Unreferenced until the WebRTC net driver
    # lands (2.6c), so it's dead-stripped now — harmless.
    "SHELL:--js-library ${CMAKE_SOURCE_DIR}/../vendor/glue/webrtc-shim.js"
    "SHELL:--js-library ${CMAKE_SOURCE_DIR}/../vendor/glue/wasmfs-shim.js"
  )
  if(UT_WEB_PERF)
    target_link_options(${PROJECT_NAME} PRIVATE -O3 -g0 -sASSERTIONS=0)
  else()
    target_link_options(${PROJECT_NAME} PRIVATE -g -sASSERTIONS=1)
  endif()
  # Preload the boot INI so appInit has config before FetchFS streams the rest. It comes from the
  # TRACKED, WEB-SPECIFIC set in this repo (Engine/Config/web) — NOT the out-of-tree asset dir, whose
  # inis belong to an untracked game install and silently revert when that image is rebuilt (they also
  # lack the web-only [WebRTCDrv.WebRTCNetDriver] section). Engine/Config/*.ini (no subdir) stays the
  # platform-agnostic / native reference set. appInit derives Unreal.ini from this, and that derived
  # name is excluded from the ufront asset manifest so a stale install copy cannot shadow it.
  if(EXISTS "${CMAKE_SOURCE_DIR}/../Engine/Config/web/Default.ini")
    set_property(TARGET ${PROJECT_NAME} APPEND_STRING PROPERTY LINK_FLAGS
      " --preload-file ${CMAKE_SOURCE_DIR}/../Engine/Config/web/Default.ini@/System/Default.ini")
  endif()
  # The preloaded inis are INPUTS to the link: without LINK_DEPENDS, editing the web config leaves a
  # stale .data baked (cmake sees no source change and skips the relink) — a silent trap.
  set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_SOURCE_DIR}/../Engine/Config/web/Default.ini")
  # The --js-library shims are link INPUTS too, and CMake does not infer that from a link option: editing
  # a shim left a STALE .js baked while the build reported success, so the change appeared to have no
  # effect (2.13.2: the worker never received the target descriptor and silently cached nothing). Same
  # class of trap as the preloaded ini above, same fix.
  set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_SOURCE_DIR}/../vendor/glue/webrtc-shim.js"
    "${CMAKE_SOURCE_DIR}/../vendor/glue/wasmfs-shim.js")
  # Bare loader: SDLLaunch.js (+ .wasm/.data), no .html. Output into a build-<cfg>/SDLLaunch/ subdir so the
  # artifact path matches engine-ut2004's convention (engine-<target>/build-wasm-perf/SDLLaunch/), which is
  # what the monorepo's packages/build derives per target.
  set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME SDLLaunch
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/SDLLaunch)

  # Emit the artifact descriptor meta.json (docs/05 §4) — packages/build validates it (schema + abiVersion
  # === glue ABI_VERSION + checksums + loaderStyle) before assembling serve-web. abiVersion is read from the
  # vendored frozen ABI header (single source); provenance/toolchain are best-effort.
  file(STRINGS ${CMAKE_SOURCE_DIR}/../vendor/glue/ue_abi.h _ue_abi_line REGEX "define[ \t]+UE_ABI_VERSION")
  string(REGEX MATCH "([0-9]+)" UE_ABI_VERSION "${_ue_abi_line}")
  execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR}/.. rev-parse --short HEAD
    OUTPUT_VARIABLE META_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  string(TIMESTAMP META_BUILT_AT "%Y-%m-%dT%H:%M:%SZ" UTC)
  execute_process(COMMAND emcc --version
    OUTPUT_VARIABLE _emcc_ver OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" META_EMCC "${_emcc_ver}")
  find_program(NODE_EXECUTABLE node REQUIRED)
  # ufront 2.13.2 — the emitter is an INPUT to the artifact, and CMake cannot infer that from a
  # POST_BUILD COMMAND: editing it changes nothing the build watches, so the target does not relink,
  # POST_BUILD never re-runs, and a STALE meta.json ships while the build reports success. Hit for real
  # when buildFlags was corrected. Same class of trap as the preloaded config and the vendored shims,
  # same fix — LINK_DEPENDS forces the relink that re-runs POST_BUILD.
  set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/emit-artifact-meta.mjs")
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/emit-artifact-meta.mjs
      --out=$<TARGET_FILE_DIR:${PROJECT_NAME}>
      --target=unreal1 --abi=${UE_ABI_VERSION}
      --gitSha=${META_GIT_SHA} --builtAt=${META_BUILT_AT} --emcc=${META_EMCC}
    COMMENT "Emitting artifact meta.json (docs/05 §4)"
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

if(VITA)
  vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
  vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/Res/sce_sys sce_sys
  )
endif()
