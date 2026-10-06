cmake_minimum_required(VERSION 3.24)
include(../cmake/utils.cmake)
include(FetchContent)

# Remove default MSVC warning levels for all third-party code
if(MSVC)
    string(REGEX REPLACE "/W[0-4]" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REGEX REPLACE "/W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

######################################################################
# SDL3

# patched: D3D12 descriptor heap overflow with lots of texture bindings, see cmake/patch_sdl.cmake
FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.8
        PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/../cmake/patch_sdl.cmake
        UPDATE_DISCONNECTED TRUE)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SDL3)

if(APPLE)
    # Apple Clang warns about SDL internals — silence everything
    target_silence_warnings(SDL3-static)
    target_silence_warnings(SDL_uclibc)
elseif(MSVC)
    # C5286: enum conversion in SDL's Vulkan renderer
    target_compile_options(SDL3-static PRIVATE /wd5286)
    # Note: D9025 from SDL_mslibc.c (/GL vs /GL-) is intentional and harmless — SDL
    # deliberately disables LTO for that file. Not worth disabling IPO for all of SDL3.
endif()

######################################################################
# IMGUI

FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.5-docking
)

FetchContent_MakeAvailable(imgui)

file(GLOB IMGUI_CORE_SOURCES "${imgui_SOURCE_DIR}/*.cpp")
set(IMGUI_BACKEND_SOURCES
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
)

add_library(imgui_lib STATIC ${IMGUI_CORE_SOURCES} ${IMGUI_BACKEND_SOURCES})

target_link_libraries(imgui_lib PUBLIC SDL3::SDL3-static)

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(imgui_lib PUBLIC X11::X11)
endif()

target_include_directories(imgui_lib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends)

target_compile_definitions(imgui_lib PUBLIC IMGUI_USE_WCHAR32)

######################################################################
# NativeFileDialog

FetchContent_Declare(
        nfd
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended
        GIT_TAG v1.3.0
)

FetchContent_MakeAvailable(nfd)

######################################################################
# JSON

FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(json)

######################################################################
# CMRC (CMake Resource Compiler)

FetchContent_Declare(
        cmrc
        GIT_REPOSITORY https://github.com/cskilbeck/cmrc_fork.git
        GIT_TAG release_21012026
)

FetchContent_MakeAvailable(cmrc)

set(cmrc_SOURCE_DIR "${cmrc_SOURCE_DIR}" PARENT_SCOPE)

######################################################################
# stb_image

FetchContent_Declare(
        stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG        f1c79c02822848a9bed4315b12c8c8f3761e1296
)

FetchContent_MakeAvailable(stb)

add_library(stb_image INTERFACE)

target_include_directories(stb_image INTERFACE ${stb_SOURCE_DIR})

######################################################################
# libwebp (just the decoder, for glTF WebP textures)

FetchContent_Declare(
        libwebp
        GIT_REPOSITORY https://github.com/webmproject/libwebp.git
        GIT_TAG v1.5.0
        GIT_SHALLOW TRUE
)

foreach(_webp_option ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO WEBPMUX EXTRAS LIBWEBPMUX FUZZTEST)
    set(WEBP_BUILD_${_webp_option} OFF CACHE BOOL "" FORCE)
endforeach()

FetchContent_MakeAvailable(libwebp)

set(WEBP_INCLUDE_DIR "${libwebp_SOURCE_DIR}/src")

######################################################################
# Assimp, for mesh formats (glTF, FBX, OBJ, DAE, 3MF...), OpenCascade does the CAD ones
# Import only, static, with its own zlib and Draco (for compressed glTF)

FetchContent_Declare(
        assimp
        GIT_REPOSITORY https://github.com/assimp/assimp.git
        GIT_TAG v6.0.5
        GIT_SHALLOW TRUE
)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DRACO ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DRACO_STATIC ON CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)

# The Blender importer is deprecated and can't read anything from Blender 3.0 on (zstd
# compression, then new mesh storage and file header), export glTF from Blender instead
set(ASSIMP_BUILD_BLEND_IMPORTER OFF CACHE BOOL "" FORCE)

# Draco's cmake_minimum_required predates CMP0091, which it needs to use the same runtime library as us
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
FetchContent_MakeAvailable(assimp)
unset(CMAKE_POLICY_DEFAULT_CMP0091)

# Draco generates draco/draco_features.h in the top level build directory, which Assimp
# only includes when it's the top level project
target_include_directories(assimp PRIVATE "${CMAKE_BINARY_DIR}")

######################################################################
# OpenCascade (built at configure time, see cmake/occt.cmake)

include(../cmake/occt.cmake)

######################################################################
# Enable IPO/LTO for some libs

set(IPO_LIBS
        SDL3-static
        imgui_lib
        nfd
)

foreach(LIB ${IPO_LIBS})
    target_enable_ipo(${LIB})
endforeach()

######################################################################
# main 3rd party library

add_library(third_party INTERFACE)

target_link_libraries(third_party INTERFACE
        imgui_lib
        nfd
        nlohmann_json::nlohmann_json
        stb_image
        webpdecoder
        assimp
        occt
)

# webp/decode.h (libwebp's own include directories are only for building it)
target_include_directories(third_party INTERFACE "${WEBP_INCLUDE_DIR}")
