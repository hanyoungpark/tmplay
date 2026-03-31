cmake_minimum_required(VERSION 3.20)
project(tmplay VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(tmplay src/main.cpp)
target_include_directories(tmplay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/conan_toolchain.cmake")
  # Conan build: toolchain already injected via -DCMAKE_TOOLCHAIN_FILE
  find_package(ffmpeg REQUIRED)
  find_package(ftxui REQUIRED)
  target_link_libraries(tmplay PRIVATE ffmpeg::ffmpeg ftxui::ftxui)
else()
  # System / Homebrew build
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswscale libswresample)
  find_package(ftxui REQUIRED)
  target_link_libraries(tmplay PRIVATE
    PkgConfig::FFMPEG
    ftxui::screen
    ftxui::dom)
endif()

# macOS: link frameworks if needed
if(APPLE)
  find_library(CORE_FOUNDATION CoreFoundation)
  find_library(AUDIO_TOOLBOX AudioToolbox)
  if(CORE_FOUNDATION)
    target_link_libraries(tmplay PRIVATE ${CORE_FOUNDATION})
  endif()
  if(AUDIO_TOOLBOX)
    target_link_libraries(tmplay PRIVATE ${AUDIO_TOOLBOX})
  endif()
endif()
