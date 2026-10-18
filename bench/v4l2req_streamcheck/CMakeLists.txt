# v4l2req_streamcheck -- board-side proof that streamed decode through the
# patched libav matches whole-AU decode (see main.cpp). Only in the
# V4L2-request HW build, where libav comes from the Buildroot sysroot.
if(NOT MABUR_PLAYER_HW OR NOT MABUR_PLAYER_DECODER STREQUAL "v4l2req")
  return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(STREAMCHECK_LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil)
add_executable(v4l2req_streamcheck main.cpp)
target_link_libraries(v4l2req_streamcheck PRIVATE mabur_common PkgConfig::STREAMCHECK_LIBAV)
