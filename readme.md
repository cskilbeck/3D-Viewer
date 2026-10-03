# 3D Viewer

A cross-platform (Windows, macOS, Linux) viewer for 3D models (STEP, IGES, STL, OBJ, glTF, VRML...). Same application structure as [Gerber Explorer](https://github.com/cskilbeck/gerber_explorer): SDL3 + SDL_GPU for windowing and rendering, Dear ImGui for the UI.

This is a work in progress. STEP files are loaded with [OpenCascade](https://github.com/Open-Cascade-SAS/OCCT) (assembly tree, names and colors), meshed and drawn with SDL_GPU.

## Supported formats

Everything OpenCascade can import:

| Format | Extensions | Notes |
|---|---|---|
| STEP | `.step` `.stp` `.stpz` | names, colors, transparency, assemblies |
| IGES | `.iges` `.igs` | |
| STL | `.stl` | mesh only |
| OBJ | `.obj` | mesh only, assumed to be in millimeters |
| glTF | `.gltf` `.glb` | mesh only, Draco compressed files aren't supported |
| VRML | `.wrl` `.vrml` | mesh only, units are taken as-is (KiCad models are in 0.1" units) |
| OpenCascade BREP | `.brep` | |
| OpenCascade XCAF | `.xbf` | |

Mesh formats have no CAD edges, so the sharp ones (where the surface bends by more than 35 degrees, and open boundaries) are drawn instead. If the file has no normals, they're smoothed only across angles under 35 degrees so sharp edges stay sharp.

## Controls

| | |
|---|---|
| Left click | Select the part under the mouse, click again to cycle through the parts behind it |
| Left drag | Orbit around the selection (or the whole model) |
| Right / Shift+left drag | Pan |
| Middle drag | Zoom (right/up zooms in, left/down zooms out) |
| Wheel | Zoom |
| F / Fit button | Fit the selection (or the whole model) in the view |
| E / Edges checkbox | Toggle edges |
| Ctrl+O | Open |

A file can also be dropped on the window or passed on the command line.

## Build Instructions

### Install CMake and Ninja

- Get CMake from [here](https://cmake.org/download/)
- Get Ninja from [here](https://ninja-build.org/)

Or you can use `make` instead of `ninja`, in which case... install that.

### Windows/MSVC

#### Get to a 64 bit developer Command Prompt

One way to do this is open a Command Prompt and enter this (assuming your Visual Studio installation is in the default location).

```
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
```

#### Clone the repository

```
> cd <your dev folder>
> git clone https://github.com/cskilbeck/step_viewer
> cd step_viewer
```

#### Build it

```
> cmake -G Ninja -B build
> cmake --build build
```

The executable should be in `build/step_viewer/step_viewer.exe`

#### Alternatively, if you want to use Visual Studio

```
> cmake -B build
```

Then open Visual Studio and load the `.sln` file in the `build` folder.

#### Other targets

I've tested building it with these toolchains on Windows via CLion.

- MSVC
- clang-cl
- gcc

### MacOS

Open a terminal, and make sure you have the XCode Command Line tools installed:

```
$ xcode-select --install
```

LLVM (version 21 or later) is also required, one way to get this is via Homebrew:

```
brew install llvm@21
```

Then, to build it:

```
$ cd <your dev folder>
$ git clone https://github.com/cskilbeck/step_viewer
$ cd step_viewer
$ cmake -G Ninja -B build
$ cmake --build build
```

The result should be in `build/step_viewer`

Note that debug builds create a bare executable, release builds create an app package.

### Linux

#### Prerequisites

You need CMake 3.24+, Ninja (or Make), and a C++23-capable compiler (GCC 13+ or Clang 21+).

The project uses SDL3 (built statically from source via FetchContent) for windowing and the GPU backend, which targets Vulkan on Linux. The native file dialog uses GTK3. All other dependencies are fetched automatically by CMake.

**Debian/Ubuntu** (tested on Ubuntu 24.04):

```
$ sudo apt install build-essential cmake ninja-build pkg-config \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
    libxi-dev libxfixes-dev libxss-dev \
    libxkbcommon-dev libxkbcommon-x11-dev \
    libwayland-dev wayland-protocols libdecor-0-dev \
    libdrm-dev libgbm-dev libudev-dev libdbus-1-dev libibus-1.0-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev \
    libgtk-3-dev \
    libvulkan-dev mesa-vulkan-drivers
```

`vulkan-tools` (provides `vulkaninfo`) is optional but useful to confirm Vulkan is working.

**Fedora** (untested — package names translated from the Ubuntu list):

```
$ sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \
    libX11-devel libXext-devel libXrandr-devel libXcursor-devel \
    libXi-devel libXfixes-devel libXScrnSaver-devel \
    libxkbcommon-devel libxkbcommon-x11-devel \
    wayland-devel wayland-protocols-devel libdecor-devel \
    libdrm-devel mesa-libgbm-devel systemd-devel dbus-devel ibus-devel \
    alsa-lib-devel pulseaudio-libs-devel pipewire-devel \
    gtk3-devel \
    vulkan-loader-devel vulkan-headers mesa-vulkan-drivers
```

**Arch** (untested — package names translated from the Ubuntu list):

```
$ sudo pacman -S base-devel cmake ninja pkgconf \
    libx11 libxext libxrandr libxcursor libxi libxfixes libxss \
    libxkbcommon \
    wayland wayland-protocols libdecor \
    libdrm libudev0-shim dbus libibus \
    alsa-lib libpulse pipewire \
    gtk3 \
    vulkan-icd-loader vulkan-headers vulkan-mesa-layers
```

#### Clone and build

```
$ git clone https://github.com/cskilbeck/step_viewer
$ cd step_viewer
$ cmake -G Ninja -B build
$ cmake --build build
```

The executable will be at `build/step_viewer/step_viewer`.

The first configure pulls down a handful of dependencies via CMake FetchContent (SDL3, Dear ImGui, nativefiledialog-extended, nlohmann/json, stb and cmrc). It takes a few minutes the first time; subsequent builds are incremental.

## OpenCascade

OpenCascade (OCCT 8.0.1) is built from source as part of the **first configure** - see `cmake/occt.cmake`. Expect that configure to take 10-20 minutes; progress goes to log files in `<build dir>/_occt`. After that it's skipped unless something relevant changes (OCCT version, compiler, build type or runtime library).

Only the toolkits needed for importing models and meshing them are built, as static libraries. RapidJSON (for glTF) is fetched as well.

- With MSVC, OCCT is built to match the configuration (Debug/Release) because the runtimes are incompatible. Visual Studio (multi-config) builds both.
- Everywhere else OCCT is always built in Release, including for Debug builds of the app.

OCCT is licensed under the LGPL 2.1 with an additional exception. Because it is linked statically here, check the [licensing terms](https://dev.opencascade.org/resources/licensing) before distributing binaries.
