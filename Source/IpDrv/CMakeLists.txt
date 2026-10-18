project(IpDrv CXX)

set(SRC_FILES
  "Src/TcpLink.cpp"
  "Src/UdpLink.cpp"
  "Src/IpDrv.cpp"
  "Src/UnSocket.cpp"
)

add_library(${PROJECT_NAME} ${LIB_TYPE} ${SRC_FILES})

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/Src
  # AUdpLink calls the synthetic LAN in WebRTCDrv (WebRTCLan.h). Header only: the dependency is one-way
  # and resolved at the final --whole-archive link, so IpDrv does not link WebRTCDrv (no module cycle).
  ${CMAKE_CURRENT_SOURCE_DIR}/../WebRTCDrv
)

target_link_libraries(${PROJECT_NAME} Engine Core)

if(TARGET_IS_WINDOWS)
  target_link_libraries(${PROJECT_NAME} ws2_32)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE IPDRV_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})
