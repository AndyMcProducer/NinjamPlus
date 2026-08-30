cmake_minimum_required(VERSION 2.8)
set(CMAKE_C_STANDARD 99)
project(GreenFFTs)


add_executable( conv2dTest   conv2dTest.c )
add_executable( convTest     convTest.c )
add_executable( rfft2dTestML rfft2dTestML.c )

target_link_libraries(convTest      GreenFFT)
target_link_libraries(conv2dTest    GreenFFT)
target_link_libraries(rfft2dTestML  GreenFFT)

enable_testing()

add_test(convTestC convTest)
add_test(conv2dTestC conv2dTest)
add_test(rfft2dTestMLC rfft2dTestML)

add_test(NAME convTestM
  COMMAND octave "--no-history" "--quiet" "${CMAKE_CURRENT_SOURCE_DIR}/convtest.m"
  WORKING_DIRECTORY ${UNIT_TEST_BIN_OUTPUT_DIR}
)

add_test(NAME conv2dTestM
  COMMAND octave "--no-history" "--quiet" "${CMAKE_CURRENT_SOURCE_DIR}/conv2dtest.m"
  WORKING_DIRECTORY ${UNIT_TEST_BIN_OUTPUT_DIR}
)

add_test(NAME rfft2dTestMLM
  COMMAND octave "--no-history" "--quiet" "${CMAKE_CURRENT_SOURCE_DIR}/rfft2dTestML.m"
  WORKING_DIRECTORY ${UNIT_TEST_BIN_OUTPUT_DIR}
)
