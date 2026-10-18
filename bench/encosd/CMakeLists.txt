# encosd -- MPP encoder-side OSD spike bench (report:
# docs/superpowers/specs/2026-08-04-mpp-enc-osd-spike.md).
#
# aarch64 cross build only: it drives the real vepu54x through
# librockchip_mpp, so it needs the MPP root exactly the way maburplay's MPP
# decoder build does -- and nothing on a V4L2-request (Cedrus) build.
# Reuses mabur_player_core for the OSD font/layout/rasterizer rather than
# carrying a second copy of the glyph blitter.
if(NOT MABUR_PLAYER_HW OR NOT MABUR_PLAYER_DECODER STREQUAL "mpp")
  return()
endif()

add_executable(encosd encosd_main.cpp osd_palette.cpp)
target_link_libraries(encosd PRIVATE
  mabur_player_core
  "${MABUR_MPP_ROOT}/lib/librockchip_mpp.a"
  pthread)
target_include_directories(encosd PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  "${MABUR_MPP_ROOT}/include")
