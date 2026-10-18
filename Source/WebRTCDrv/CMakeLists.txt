project(WebRTCDrv)

# The UE1 WebRTC net driver (Emscripten/browser multiplayer). Its transport is the glue's webrtc_* ABI (a
# shim baked in at link time), not BSD sockets.
set(SRC_FILES
  "WebRTCDrv.cpp"
  # The synthetic LAN that makes the stock "Find Local Servers" menu work over WebRTC. IpDrv's AUdpLink
  # calls into it (one-way; see WebRTCLan.h), so this package's root is a PUBLIC include dir below.
  "WebRTCLan.cpp"
)

add_library(${PROJECT_NAME} ${LIB_TYPE} ${SRC_FILES})

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/Src
)

target_link_libraries(${PROJECT_NAME} Engine Core)

target_compile_definitions(${PROJECT_NAME} PRIVATE WEBRTCDRV_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})
