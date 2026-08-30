cmake_minimum_required(VERSION 2.8)
set(CMAKE_C_STANDARD 99)
project(GreenFFTs)


add_executable( ffttiming   fftTiming.c )
add_executable( rffttiming  rfftTiming.c )

if (UNIX)
  target_compile_definitions(ffttiming PRIVATE HAVE_SYS_TIMES=1)
  target_compile_definitions(rffttiming PRIVATE HAVE_SYS_TIMES=1)
endif (UNIX)

target_link_libraries(ffttiming  GreenFFT)
target_link_libraries(rffttiming GreenFFT)
