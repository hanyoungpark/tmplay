# tmplay

A terminal video player for macOS.  
Uses C++20, CMake, Conan (dependencies), and FFmpeg (decoding).

## Full pixel playback (image mode)

**Kitty**, **WezTerm**, **Warp**, and **iTerm2** use terminal image protocols for **pixel-accurate video** playback.

- **Kitty / WezTerm / Warp**: Kitty graphics protocol (RGB), up to 1920×1080
- **iTerm2**: PNG inline images (OSC limit, up to ~1200×676)
- Other terminals: block characters + 256 colors

The terminal is auto-detected via `TERM`, `TERM_PROGRAM` (e.g. WarpTerminal, iTerm), `KITTY_WINDOW_ID`, etc.  
In image mode, terminal pixel size (`ws_xpixel`/`ws_ypixel`) is used when available; otherwise resolution is estimated from cell count.

## Requirements

- macOS
- CMake 3.20+
- Conan 2.x
- C++20-capable compiler (Xcode / Apple Clang)

## Build

```bash
# 1. Install dependencies with Conan (first run may take several minutes for FFmpeg etc.)
conan install . --output-folder=build --build=missing -s compiler.cppstd=20

# 2. Configure CMake (Conan toolchain)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release

# 3. Build
cmake --build build
```

Executable: `build/tmplay`

## Usage

```bash
./build/tmplay <video_file>
```

- **Space**: pause / resume  
- **q**: quit  

Video is rendered with block characters and 256 colors on unsupported terminals, or as full pixel images where supported.

## Project layout

- `conanfile.txt` — Conan dependencies (ffmpeg)
- `CMakeLists.txt` — CMake build (C++20, Conan integration)
- `src/main.cpp` — Video decoding (FFmpeg) + terminal rendering (ANSI / image protocols)
