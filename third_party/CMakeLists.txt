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

FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.8)

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
        occt
)
