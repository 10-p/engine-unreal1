project(SoftDrv CXX)

set(SRC_FILES
  "Src/DrawPoly.cpp"
  "Src/DrawSurf.cpp"
  "Src/DrawTile.cpp"
  "Src/Hit.cpp"
  "Src/Line2D.cpp"
  "Src/SoftDrv.cpp"
)

add_library(${PROJECT_NAME} ${LIB_TYPE} ${SRC_FILES})

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/Src
)

target_link_libraries(${PROJECT_NAME} Render Engine Core)

target_compile_definitions(${PROJECT_NAME} PRIVATE SOFTDRV_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})

# ufront 2.19f: the SizeX/SizeY-indexed scratch tables are STATIC arrays, so their bounds cost .bss at
# every resolution — 368 KB at the 4096 default, whether the viewport is 4K or 240x240. Lower this for
# memory-constrained targets (a 256 ceiling costs 23 KB); viewports above it degrade fail-soft rather
# than corrupting. See the rationale in Src/SoftDrvPrivate.h.
# ufront 2.71: the wasm preset raises this to 8192 (cacheVariables, so existing build trees pick it up on
# re-configure): the web canvas is the display's DEVICE pixel size now (5120 on a 125% 4K-class desktop)
# and the UE1 resize path bypasses SetRes's clamp, so 4096 would let the X-indexed tables overrun.
set(SOFTDRV_MAX_SCREEN "4096" CACHE STRING "Max software-renderer viewport dimension (static table bound)")
target_compile_definitions(${PROJECT_NAME} PRIVATE SOFTDRV_MAX_SCREEN=${SOFTDRV_MAX_SCREEN})
