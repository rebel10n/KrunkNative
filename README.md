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

# Running

If you wish to run KrunkNative, you must first add game assets, please read [the assets readme](/assets/README.md) for more information. The client looks for the `assets` folder in the same directory in release mode, or in the parent directory in development mode (when the binary is inside the `cmake-build-release` or `cmake-build-debug` folder).

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

Apparently we're less clever than FRVR, we could NEVER be creators...  
![You were never a creator](/doc/email.png)

We tried to negotiate with Brian to work for FRVR and give them every script/exploit we had for ~$10k (shared across ~10 people)  
![Negotiations](/doc/negotiations.png)  
![Negotiations](/doc/negotiations_brian.png)
