cmake_minimum_required(VERSION 3.21)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/bin/version.ini" _vb_version_ini)
string(REGEX MATCH "version[ \t]*=[ \t]*([0-9]+)\\.([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${_vb_version_ini}")
if(NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "version.ini must contain version=Major.Minor.Build.Revision")
endif()
set(VB_VER_MAJOR "${CMAKE_MATCH_1}")
set(VB_VER_MINOR "${CMAKE_MATCH_2}")
set(VB_VER_PATCH "${CMAKE_MATCH_3}")
set(VB_VER_TWEAK "${CMAKE_MATCH_4}")
set(VB_VERSION "${VB_VER_MAJOR}.${VB_VER_MINOR}.${VB_VER_PATCH}.${VB_VER_TWEAK}")
project(VideoBlackoutCore VERSION ${VB_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENCV_ROOT "${CMAKE_SOURCE_DIR}/opencv")
set(ORT_ROOT "${CMAKE_SOURCE_DIR}/third_party/onnxruntime")
set(OUTPUT_BIN "${CMAKE_SOURCE_DIR}/bin")

if(NOT EXISTS "${OPENCV_ROOT}/include/opencv2/core.hpp")
  message(FATAL_ERROR "OpenCV not found at ${OPENCV_ROOT}. Expected include/opencv2/core.hpp under appwpf/opencv.")
endif()
if(NOT EXISTS "${ORT_ROOT}/include/onnxruntime_c_api.h")
  message(FATAL_ERROR "ONNX Runtime not found at ${ORT_ROOT}. Expected headers under appwpf/third_party/onnxruntime.")
endif()

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${OUTPUT_BIN}")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} "${OUTPUT_BIN}")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${OUTPUT_BIN}")
endforeach()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_BIN}")

add_library(VideoBlackoutCore SHARED
  native/src/fs.cpp
  native/src/Settings.cpp
  native/src/RedactionFilter.cpp
  native/src/VideoDecoder.cpp
  native/src/VideoEncoder.cpp
  native/src/OrtRuntime.cpp
  native/src/FaceDetectorDnn.cpp
  native/src/FaceLandmarks.cpp
  native/src/FaceReId.cpp
  native/src/IdentityGallery.cpp
  native/src/KeepFacePolicy.cpp
  native/src/ByteTrack.cpp
  native/src/YoloDetectorDnn.cpp
  native/src/CsrtTracker.cpp
  native/src/TrackManager.cpp
  native/src/PreviewEngine.cpp
  native/src/ExportEngine.cpp
  native/src/FfmpegAudioTools.cpp
  native/src/HardwareInfo.cpp
  native/src/HardwareLoad.cpp
  native/src/videoblackout_c.cpp
  native/src/platform/ChildProcess.cpp
  native/src/platform/HwId.cpp
)

target_compile_definitions(VideoBlackoutCore PRIVATE VB_CORE_EXPORTS)
target_include_directories(VideoBlackoutCore PUBLIC
  "${CMAKE_SOURCE_DIR}/native/include"
  "${OPENCV_ROOT}/include"
  "${ORT_ROOT}/include"
)

if(MSVC)
  target_compile_options(VideoBlackoutCore PRIVATE /utf-8 /EHsc)
endif()

set(_ocv_mods core imgproc imgcodecs video videoio dnn)
foreach(mod ${_ocv_mods})
  add_library(vb_opencv_${mod} SHARED IMPORTED)
  set_target_properties(vb_opencv_${mod} PROPERTIES
    IMPORTED_CONFIGURATIONS "DEBUG;RELEASE;RELWITHDEBINFO;MINSIZEREL"
    IMPORTED_IMPLIB_RELEASE "${OPENCV_ROOT}/lib/Release/opencv_${mod}4100.lib"
    IMPORTED_LOCATION_RELEASE "${OPENCV_ROOT}/bin/Release/opencv_${mod}4100.dll"
    IMPORTED_IMPLIB_RELWITHDEBINFO "${OPENCV_ROOT}/lib/Release/opencv_${mod}4100.lib"
    IMPORTED_LOCATION_RELWITHDEBINFO "${OPENCV_ROOT}/bin/Release/opencv_${mod}4100.dll"
    IMPORTED_IMPLIB_MINSIZEREL "${OPENCV_ROOT}/lib/Release/opencv_${mod}4100.lib"
    IMPORTED_LOCATION_MINSIZEREL "${OPENCV_ROOT}/bin/Release/opencv_${mod}4100.dll"
    IMPORTED_IMPLIB_DEBUG "${OPENCV_ROOT}/lib/Debug/opencv_${mod}4100d.lib"
    IMPORTED_LOCATION_DEBUG "${OPENCV_ROOT}/bin/Debug/opencv_${mod}4100d.dll"
  )
endforeach()

target_link_libraries(VideoBlackoutCore PRIVATE
  vb_opencv_core vb_opencv_imgproc vb_opencv_imgcodecs vb_opencv_video
  vb_opencv_videoio vb_opencv_dnn
  "${ORT_ROOT}/lib/onnxruntime.lib"
)

if(WIN32)
  target_link_libraries(VideoBlackoutCore PRIVATE bcrypt advapi32 dxgi pdh)
  set(VB_VERSION_RC "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/native/version.rc.in" "${VB_VERSION_RC}" @ONLY)
  target_sources(VideoBlackoutCore PRIVATE "${VB_VERSION_RC}")
endif()

add_custom_command(TARGET VideoBlackoutCore POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTPUT_BIN}/models"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${OPENCV_ROOT}/bin/$<IF:$<CONFIG:Debug>,Debug,Release>"
    "${OUTPUT_BIN}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ORT_ROOT}/bin/onnxruntime.dll"
    "${OUTPUT_BIN}/onnxruntime.dll"
  COMMENT "Copy OpenCV and ORT runtime next to VideoBlackoutCore.dll"
)

set(_vb_onnx
  "${CMAKE_SOURCE_DIR}/models/yolo11n-face.onnx"
  "${CMAKE_SOURCE_DIR}/models/yolov8n.onnx"
)
foreach(_m ${_vb_onnx})
  if(EXISTS "${_m}")
    add_custom_command(TARGET VideoBlackoutCore POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTPUT_BIN}/models"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_m}" "${OUTPUT_BIN}/models"
    )
  endif()
endforeach()

set(_ffmpeg "${CMAKE_SOURCE_DIR}/tools/ffmpeg/ffmpeg.exe")
if(NOT EXISTS "${_ffmpeg}")
  file(GLOB _ffmpeg_alt "${CMAKE_SOURCE_DIR}/tools/ffmpeg_extract/*/bin/ffmpeg.exe")
  if(_ffmpeg_alt)
    list(GET _ffmpeg_alt 0 _ffmpeg)
  endif()
endif()
if(NOT EXISTS "${_ffmpeg}" AND EXISTS "${OUTPUT_BIN}/ffmpeg.exe")
  set(_ffmpeg "${OUTPUT_BIN}/ffmpeg.exe")
endif()
if(EXISTS "${_ffmpeg}")
  add_custom_command(TARGET VideoBlackoutCore POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ffmpeg}" "${OUTPUT_BIN}/ffmpeg.exe"
  )
endif()
