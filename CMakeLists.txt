cmake_minimum_required(VERSION 3.20)
project(tmplay VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Conan: use toolchain from conan install
if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/conan_toolchain.cmake")
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_BINARY_DIR}/conan_toolchain.cmake")
endif()

find_package(ffmpeg REQUIRED)

add_executable(tmplay src/main.cpp)
target_include_directories(tmplay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(tmplay PRIVATE ffmpeg::ffmpeg)

# macOS: link frameworks if needed
if(APPLE)
  find_library(CORE_FOUNDATION CoreFoundation)
  find_library(COCOA_FOUNDATION Cocoa)
  if(CORE_FOUNDATION)
    target_link_libraries(tmplay PRIVATE ${CORE_FOUNDATION})
  endif()
endif()
