cmake_minimum_required(VERSION 3.23.0)
project(Encodec LANGUAGES C CXX)

# Helpers
set(IS_GCC      "$<CXX_COMPILER_ID:GNU>")
set(IS_CLANG    "$<CXX_COMPILER_ID:Clang>")
set(IS_MSVC     "$<CXX_COMPILER_ID:MSVC>")
set(IS_NOT_MSVC "$<NOT:${IS_MSVC}>")

# Dependencies
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CPM_SOURCE_CACHE ${CMAKE_SOURCE_DIR}/.cache/cpm CACHE PATH "Directory to cache CPM packages" FORCE)
include(cmake/CPM.cmake)
include(CMakeDependentOption)
include(CheckIPOSupported)
find_package(AVX)
find_package(NEON)

CPMAddPackage(
  NAME Eigen3
  VERSION 5.0.1
  URL https://gitlab.com/libeigen/eigen/-/archive/5.0.1/eigen-5.0.1.tar.gz
  OPTIONS "EIGEN_BUILD_DOC OFF" "EIGEN_BUILD_PKGCONFIG OFF" "EIGEN_BUILD_TESTING OFF" "EIGEN_BUILD_DEMOS OFF" "EIGEN_BUILD_SPBENCH OFF")

# Options
option(ENCODEC_ENABLE_SANITIZERS "Build unit tests with sanitizers" OFF)
option(ENCODEC_ENABLE_IPO "Enable interprocedural optimization in Release builds" ON)
option(ENCODEC_ENABLE_OPENMP "Enable intra-model CPU threading with OpenMP" ON)

# Weights
function(add_zipped_weight_source out_var zip_file output_name)
  set(output_file "${CMAKE_CURRENT_BINARY_DIR}/generated/weights/${output_name}")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/weights")

  add_custom_command(
      OUTPUT "${output_file}"
      COMMAND ${CMAKE_COMMAND} -E tar xf "${zip_file}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/weights"
      DEPENDS "${zip_file}"
      COMMENT "Extracting ${output_name}"
      VERBATIM
  )

  set(${out_var} "${output_file}" PARENT_SCOPE)
endfunction()

add_zipped_weight_source(ENCODER_WEIGHTS_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/weights/encoder.cpp.gz" "encoder.cpp")
add_zipped_weight_source(DECODER_WEIGHTS_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/weights/decoder.cpp.gz" "decoder.cpp")
add_zipped_weight_source(RVQ_WEIGHTS_CPP     "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/weights/rvq.cpp.gz"     "rvq.cpp")

# Lib
add_library(encodec)
add_library(encodec::encodec ALIAS encodec)
target_compile_features(encodec PUBLIC cxx_std_20)

target_sources(encodec PRIVATE
  src/lib/encodec.cpp
  ${ENCODER_WEIGHTS_CPP}
  ${DECODER_WEIGHTS_CPP}
  ${RVQ_WEIGHTS_CPP})

target_include_directories(encodec
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/lib>
  PUBLIC $<INSTALL_INTERFACE:include>)

target_compile_options(encodec 
  PRIVATE $<${IS_NOT_MSVC}:-Wall -Wextra -Wpedantic -Werror>
  PRIVATE $<$<CXX_COMPILER_ID:AppleClang>:-Wno-nan-infinity-disabled>
  PRIVATE $<$<BOOL:${AVX_FOUND}>:${AVX_CFLAGS}>
  PRIVATE $<$<BOOL:${NEON_FOUND}>:${NEON_CFLAGS}>
  PRIVATE $<$<CONFIG:Release>:-ffast-math>)
  
target_link_libraries(encodec PRIVATE Eigen3::Eigen)

if (ENCODEC_ENABLE_OPENMP)
  find_package(OpenMP COMPONENTS CXX)
  if (OpenMP_CXX_FOUND)
    target_link_libraries(encodec PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(encodec PRIVATE ENCODEC_HAS_OPENMP=1)
  else()
    message(WARNING "OpenMP is unavailable; encoder/decoder intra-model threading is disabled")
  endif()
endif()

# The decoder consists mostly of small, fixed neural-network layers spread
# across this translation unit. Letting the linker optimize across object-file
# boundaries produces a measurable speedup and is supported by both Apple
# Clang and the Android NDK Clang toolchain. Keep a clean fallback for less
# capable toolchains.
if (ENCODEC_ENABLE_IPO)
  check_ipo_supported(RESULT ENCODEC_IPO_SUPPORTED OUTPUT ENCODEC_IPO_ERROR)
  if (ENCODEC_IPO_SUPPORTED)
    # Apply it to targets declared below as well, so the final executable link
    # performs LTO instead of merely archiving bitcode in libencodec.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET encodec PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  else()
    message(WARNING "IPO/LTO is unavailable: ${ENCODEC_IPO_ERROR}")
  endif()
endif()

# Example
add_executable(encodec_example src/examples/main.cpp)
target_link_libraries(encodec_example PRIVATE encodec::encodec)

add_executable(encodec_decode_packet src/examples/decode_packet.cpp)
target_link_libraries(encodec_decode_packet PRIVATE encodec::encodec)

add_executable(encodec_decompress src/examples/decompress_ecdc.cpp)
target_link_libraries(encodec_decompress PRIVATE encodec::encodec)

add_executable(encodec_compress src/examples/compress_ecdc.cpp)
target_link_libraries(encodec_compress PRIVATE encodec::encodec)

# Test
add_executable(encodec_tests 
  src/tests/doctest.cpp
  src/tests/encodec.cpp)
target_link_libraries(encodec_tests PRIVATE encodec::encodec)

if (ENCODEC_ENABLE_SANITIZERS)
  message(STATUS "Enabling sanitizers on unit tests")
  target_compile_options(encodec PUBLIC $<$<OR:${IS_GCC},${IS_CLANG}>:-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined>)
  target_link_options(encodec PUBLIC $<$<OR:${IS_GCC},${IS_CLANG}>:-fsanitize=address,undefined>)
endif()
