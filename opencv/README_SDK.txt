OpenCV 4.10.0 CPU Slim SDK
==========================
Modules: core, imgproc, imgcodecs, video, videoio, dnn  [NO opencv_world]
CPU: AVX2 + OpenMP, FFMPEG ON, CUDA/OpenCL OFF

Folder layout:
  include\       -> C/C++ Additional Include Directories
  lib\Release\   -> Linker Additional Library Directories [Release]
  lib\Debug\     -> Linker Additional Library Directories [Debug]
  bin\Release\   -> copy DLLs next to your Release .exe
  bin\Debug\     -> copy DLLs next to your Debug .exe

Linker Input [Release]:
  opencv_core4100.lib;opencv_imgproc4100.lib;opencv_imgcodecs4100.lib;opencv_video4100.lib;opencv_videoio4100.lib;opencv_dnn4100.lib

Linker Input [Debug]:
  opencv_core4100d.lib;opencv_imgproc4100d.lib;opencv_imgcodecs4100d.lib;opencv_video4100d.lib;opencv_videoio4100d.lib;opencv_dnn4100d.lib

Also ship opencv_videoio_ffmpeg4100_64.dll with the exe.
Requires AVX2 CPU.
