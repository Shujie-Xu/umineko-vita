# Include after creating the native engine target. The stock VitaSDK FFmpeg
# omits MPEG-2 video, MPEGvideo demuxing and H.264; it cannot run the game's
# masked .m2v movies or serve as a fallback for hardware-incompatible MP4s.
set(ONS_FFMPEG_ROOT "${ONS_NATIVE_DEPS_ROOT}" CACHE PATH
    "Native FFmpeg prefix produced by tools/build_vita.py")
function(ons_vita_add_media target)
  foreach(component avformat avcodec swresample swscale avutil)
    if(NOT EXISTS "${ONS_FFMPEG_ROOT}/lib/lib${component}.a")
      message(FATAL_ERROR "Native FFmpeg dependency missing: run tools/build_vita.py (or set ONS_FFMPEG_ROOT)")
    endif()
    list(APPEND media_libraries "${ONS_FFMPEG_ROOT}/lib/lib${component}.a")
  endforeach()
  target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/media.cpp")
  target_include_directories(${target} BEFORE PRIVATE "${ONS_FFMPEG_ROOT}/include")
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  set(ONS_VITA_MEDIA_LIBRARIES ${media_libraries}
    ass fribidi harfbuzz freetype z pthread m
    SceAvPlayer_stub SceSysmodule_stub SceSysmem_stub SceLibKernel_stub SceKernelDmacMgr_stub PARENT_SCOPE)
endfunction()
