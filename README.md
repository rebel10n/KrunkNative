# KrunkNative

Krunker.io - except it's awesome. A rewrite of the game in native C using minimal libraries, focused on freedom and performance.

![Demo](/doc/demo.png)

# Building

There is currently only one build target - the client. This project relies on CMake as its build toolchain, platform-specific instructions below.

## Windows

CMake + w64devkit/MinGW should work out of the box

## Linux

Please install the following Debian packages to build GLFW:

- libwayland-dev
- wayland-protocols
- pkg-config
- libx11-dev
- libxcursor-dev
- libxinerama-dev
- libxrandr-dev
- libxi-dev
- libgl1-mesa-dev
- libglu1-mesa-dev
- libxkbcommon-dev

Full command: `apt install -y libwayland-dev wayland-protocols pkg-config libx11-dev libxcursor-dev libxinerama-dev libxrandr-dev libxi-dev libgl1-mesa-dev libglu1-mesa-dev libxkbcommon-dev`

## Acknowledgements

- OpenGL - graphics backend
- GLFW - cross-platform OpenGL helper
- GLAD - OpenGL loader
- FreeType - font rasterizer
- STB Image - single-header image loading library
- tinyobjloader-c - single-header OBJ loading library
- verstable - single-header hash table library
- cJSON - lightweight JSON parser
- pcg_basic - pseudorandom number generator

# FRVR Slander (we ❤️ Yendis)

![Brian](/doc/brianGOD.png)
![You were never a creator](/doc/email.png)