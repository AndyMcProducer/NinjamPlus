cmake_minimum_required(VERSION 2.8)
set(CMAKE_C_STANDARD 99)
project(GreenFFTs)

########################################################################
# select the release build type by default to get optimization flags
########################################################################
if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE "Release")
   message(STATUS "Build type not specified: defaulting to release.")
endif(NOT CMAKE_BUILD_TYPE)


add_library(GreenFFT STATIC
  dxpose.c #dxpose.h
  fft2d.c  #fft2d.h
  fftext.c #fftext.h
  fftlib.c #fftlib.h
  matlib.c #matlib.h
)

set_property(TARGET GreenFFT APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(GreenFFT m)
