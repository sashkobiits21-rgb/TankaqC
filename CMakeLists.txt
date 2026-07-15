cmake_minimum_required(VERSION 3.24)
project(Tankaq CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(STEAM_SDK "${CMAKE_SOURCE_DIR}/third_party/steamworks/sdk")
if(NOT EXISTS "${STEAM_SDK}/public/steam/steam_api.h")
  message(FATAL_ERROR "Steamworks SDK not found at ${STEAM_SDK}")
endif()

add_executable(TankaqClient WIN32
  src/Main.cpp
  src/Game.cpp
  src/GpuDetect.cpp
  src/AssetLoad.cpp
  src/Sound.cpp
  src/Ui.cpp
  src/net/Net.cpp
  src/render/RendererD3D11.cpp
  src/render/RendererD3D12.cpp
)

target_compile_definitions(TankaqClient PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(TankaqClient PRIVATE /W3 /MP /utf-8)
target_include_directories(TankaqClient PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/third_party"
  "${STEAM_SDK}/public"
)
target_link_libraries(TankaqClient PRIVATE
  d3d11 d3d12 dxgi d3dcompiler dxguid user32 delayimp xaudio2 ole32
  "${STEAM_SDK}/redistributable_bin/win64/steam_api64.lib"
)
# D3D12 is delay-loaded so machines without d3d12.dll can still start (D3D11 path).
target_link_options(TankaqClient PRIVATE /DELAYLOAD:d3d12.dll)

set_target_properties(TankaqClient PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/$<CONFIG>"
)

add_custom_command(TARGET TankaqClient POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${STEAM_SDK}/redistributable_bin/win64/steam_api64.dll"
          "$<TARGET_FILE_DIR:TankaqClient>/steam_api64.dll"
  COMMAND ${CMAKE_COMMAND} -E echo 480 > "$<TARGET_FILE_DIR:TankaqClient>/steam_appid.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:TankaqClient>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:TankaqClient>/shaders"
)
