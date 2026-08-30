cmake_minimum_required(VERSION 2.8)
set(CMAKE_C_STANDARD 99)
project(KissFFT)

########################################################################
# select the release build type by default to get optimization flags
########################################################################
if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE "Release")
   message(STATUS "Build type not specified: defaulting to release.")
endif(NOT CMAKE_BUILD_TYPE)


add_library(KissFFT STATIC
  kiss_fft.c
  tools/kiss_fftr.c
  tools/kfc.c
)

target_include_directories(KissFFT PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set_property(TARGET KissFFT APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

set_property(TARGET KissFFT APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools>
)

target_link_libraries(KissFFT m)
