#=============================================================================
# NullDrv - null render device (GPU-free). A no-op URenderDevice: no GL context, no rasterization, draws
# nothing. Selected via -NULLRENDERER for a headless run; the NSDL viewport opens no GL context for it because
# the device name lacks "OPENGL".
#=============================================================================

project(NullDrv CXX)

set(SRC_FILES
  "Src/NullDrv.cpp"
)

add_library(${PROJECT_NAME} ${LIB_TYPE} ${SRC_FILES})

target_include_directories(${PROJECT_NAME}
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/Src
  # RenderPrivate.h (FSceneNode / FSurfaceInfo / FSpanBuffer decls) comes from ../Render/Src, as SoftDrv does.
  ${CMAKE_CURRENT_SOURCE_DIR}/../Render/Src
)

target_link_libraries(${PROJECT_NAME} Render Engine Core)

target_compile_definitions(${PROJECT_NAME} PRIVATE NULLDRV_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})
