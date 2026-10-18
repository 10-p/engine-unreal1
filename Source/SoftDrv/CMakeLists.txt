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

# Bound of the SizeX/SizeY-indexed scratch tables. They are static arrays, so the bound costs .bss at every
# resolution; lower it for memory-constrained targets. Above it in Y the renderer skips drawing rather than
# overrunning, but nothing guards the X-indexed tables, so it must cover the widest viewport (the wasm preset in
# CMakePresets.json sets 8192).
set(SOFTDRV_MAX_SCREEN "4096" CACHE STRING "Max software-renderer viewport dimension (static table bound)")
target_compile_definitions(${PROJECT_NAME} PRIVATE SOFTDRV_MAX_SCREEN=${SOFTDRV_MAX_SCREEN})
