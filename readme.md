# 3D Viewer

A cross-platform (Windows, macOS, Linux) viewer for 3D models (STEP, IGES, glTF, FBX, OBJ, STL, 3MF and many more). Same application structure as [Gerber Explorer](https://github.com/cskilbeck/gerber_explorer): SDL3 + SDL_GPU for windowing and rendering, Dear ImGui for the UI.

This is a work in progress. CAD formats are loaded with [OpenCascade](https://github.com/Open-Cascade-SAS/OCCT) (assembly tree, names and colors) and meshed, mesh formats are loaded with [Assimp](https://github.com/assimp/assimp), and it's all drawn with SDL_GPU.

## Supported formats

| Format | Extensions | Notes |
|---|---|---|
| STEP | `.step` `.stp` `.stpz` | assemblies, names, colors, transparency |
| IGES | `.iges` `.igs` | |
| OpenCascade BREP | `.brep` | |
| OpenCascade XCAF | `.xbf` | |
| glTF | `.gltf` `.glb` `.vrm` | PBR materials, textures (PNG, JPEG, WebP), texture transforms, Draco compressed meshes |
| Autodesk FBX | `.fbx` | |
| Wavefront OBJ | `.obj` | materials and textures from the `.mtl`, assumed to be in millimeters |
| Collada | `.dae` `.zae` `.xml` | |
| STL | `.stl` | assumed to be in millimeters |
| 3MF | `.3mf` | |
| AMF | `.amf` | |
| Stanford PLY | `.ply` | |
| VRML | `.wrl` `.vrml` | units are taken as-is (KiCad models are in 0.1" units) |
| X3D | `.x3d` `.x3db` | |
| 3D Studio | `.3ds` `.prj` | |
| LightWave | `.lwo` `.lxo` `.lws` `.mot` | |
| DirectX | `.x` | |
| Industry Foundation Classes (IFC) | `.ifc` `.ifczip` | |
| AutoCAD DXF | `.dxf` | 3D faces only |
| AC3D | `.ac` `.acc` `.ac3d` | |
| 3ds Max ASE | `.ase` `.ask` | |
| Milkshape 3D | `.ms3d` | |
| Blitz3D | `.b3d` | |
| Quake / Doom models | `.md2` `.md3` `.md5mesh` `.mdc` `.mdl` | |
| Quake III BSP | `.bsp` `.pk3` | |
| Inter-Quake Model | `.iqm` | |
| Unreal | `.3d` `.uc` | |
| Valve SMD | `.smd` `.vta` | |
| Ogre | `.mesh` `.mesh.xml` | |
| OpenGEX | `.ogex` | |
| Irrlicht | `.irr` `.irrmesh` | |
| MikuMikuDance | `.pmx` | |
| Neutral File Format | `.nff` `.enff` | |
| Object File Format | `.off` | |
| trueSpace | `.cob` `.scn` | |
| Nendo | `.ndo` | |
| Quick3D | `.q3o` `.q3s` | |
| Silo | `.sib` | |
| XGL | `.xgl` `.zgl` | |
| Raw triangles | `.raw` | |
| 3D GameStudio heightmap | `.hmp` | |
| Terragen heightmap | `.ter` | |
| Assimp binary | `.assbin` | |

Blender files aren't supported (export glTF from Blender instead).

Motion capture and animation files (`.bvh`, `.csm`, `.md5anim`, `.md5camera`) open too, but they have no geometry to show.

Everything is converted to millimeters with Z up. Formats with units (glTF, FBX, Collada etc) are scaled, others are assumed to be in millimeters. Y up formats (glTF, FBX, OBJ, PLY...) are turned so their front faces the front of the default view.

## Shading

**CAD** shading draws everything in its own color with a light from the camera. **Realistic** shading uses physically based (metallic/roughness) materials lit by a built in studio environment (sky gradient, floor and soft box lights) with blurrier reflections on rougher surfaces, and tone mapping.

Files with real materials (glTF, or anything with textures) are always shaded realistically: base color, metallic/roughness, normal, occlusion and emissive textures, and opaque/mask/blend alpha. For files which just have colors (STEP etc) it's a choice on the toolbar or in the settings: `[CAD|Realistic]`, where realistic shading treats every color as a slightly rough plastic.

Settings > Transparency picks how transparent surfaces are put in order: **None** sorts whole parts (fastest, wrong where parts overlap), **Basic** sorts every triangle (mostly right) and **Advanced** uses depth peeling, which is exactly right up to a number of layers (8 by default) but slower. Advanced needs a GPU which can sample depth textures and render to float targets, otherwise it falls back to Basic.

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
| G / Grid checkbox | Grid on the XY plane |
| X / Axes checkbox | X/Y/Z axes through the origin |
| Space | Reset view (isometric, whole model) |
| I | Isolate the selection (hide everything else) / Unisolate (show everything) |
| Esc | Quit |
| Ctrl+O | Open |

A file can also be dropped on the window or passed on the command line. With **Reuse window** on (Settings > General, the default) opening a file while 3D Viewer is already running opens it in that window instead of starting another. File > Open Recent has the last 10.

Rotation is turntable (Z stays up) by default, trackball (free rotation) is in the settings, as is orthographic projection.

## Settings

View > Settings... opens the settings window (shading, anti-aliasing, transparency, projection, rotation style, grid, selection tint, zoom...). Changes take effect straight away; **Revert** goes back to how things were when the window was opened and **Defaults** resets everything.

Settings (and the window position) are saved in `3D-Viewer.settings` in the config directory, along with the window layout (`imgui.ini`) and, while it's running, the reuse window socket (`3D-Viewer-Active`):

| | |
|---|---|
| Windows | `%LOCALAPPDATA%\3D-Viewer` |
| macOS | `~/Library/Application Support/3D-Viewer` |
| Linux | `~/.config/3D-Viewer` (or `$XDG_CONFIG_HOME/3D-Viewer`) |

## Testing

`models/` has a small set of test files covering every format and feature, see [models/README.md](models/README.md). To check they all load as expected (no window, about a second):

```
3D-Viewer --check models
```

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
> git clone https://github.com/cskilbeck/3D-Viewer
> cd 3D-Viewer
```

#### Build it

```
> cmake -G Ninja -B build
> cmake --build build
```

The executable should be in `build/src/3D-Viewer.exe`

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
$ git clone https://github.com/cskilbeck/3D-Viewer
$ cd 3D-Viewer
$ cmake -G Ninja -B build
$ cmake --build build
```

The result should be in `build/src/3D-Viewer` (or `build/src/3D-Viewer.app`)

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
$ git clone https://github.com/cskilbeck/3D-Viewer
$ cd 3D-Viewer
$ cmake -G Ninja -B build
$ cmake --build build
```

The executable will be at `build/src/3D-Viewer`.

The first configure pulls down a handful of dependencies via CMake FetchContent (SDL3, Dear ImGui, nativefiledialog-extended, nlohmann/json, stb and cmrc). It takes a few minutes the first time; subsequent builds are incremental.

## OpenCascade

OpenCascade (OCCT 8.0.1) is built from source as part of the **first configure** - see `cmake/occt.cmake`. Expect that configure to take 10-20 minutes; progress goes to log files in `<build dir>/_occt`. After that it's skipped unless something relevant changes (OCCT version, compiler, build type or runtime library).

Only the toolkits needed for importing the CAD formats and meshing them are built, as static libraries.

- With MSVC, OCCT is built to match the configuration (Debug/Release) because the runtimes are incompatible. Visual Studio (multi-config) builds both.
- Everywhere else OCCT is always built in Release, including for Debug builds of the app.

OCCT is licensed under the LGPL 2.1 with an additional exception. Because it is linked statically here, check the [licensing terms](https://dev.opencascade.org/resources/licensing) before distributing binaries.

## Other libraries

- Assimp (BSD license) is built as part of the normal build, import only, with its own zlib and Draco (compressed glTF meshes).
- libwebp decodes WebP textures, stb_image the rest.
- SDL's D3D12 backend is patched so that lots of texture bindings in one frame don't overflow its descriptor heaps, see `cmake/patch_sdl.cmake`.
