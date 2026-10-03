######################################################################
# OpenCascade (OCCT)
#
# OCCT's own CMake scripts assume they are the top-level project (they use
# CMAKE_BINARY_DIR everywhere and rewrite the global compiler flags) so it
# can't be pulled in with add_subdirectory(). Instead the source is fetched
# here and built + installed as a separate CMake project at configure time,
# then consumed via the OpenCASCADEConfig.cmake package it installs.
#
# Only the toolkits needed to import models (STEP, IGES, STL, OBJ, glTF,
# VRML, BREP, XBF - with names/colors via XCAF) and mesh them are built,
# as static libraries. TKXCAF depends on TKV3d/TKService (Visualization)
# so those come along too, but without OpenGL, FreeType or X11.
# glTF needs RapidJSON (header only, fetched here). Draco compressed glTF
# isn't supported.
#
# The first configure takes a long time (it's compiling OCCT). After that a
# stamp file short-circuits it unless something relevant changes.
#
# Defines:
#   OCCT_LIBRARIES - the toolkits to link against
#
# Note: OCCT is LGPL 2.1 (with an exception), static linking has implications
# for distribution, see https://dev.opencascade.org/resources/licensing

include(FetchContent)

set(OCCT_VERSION_TAG V8_0_1)

FetchContent_Declare(
        occt
        URL https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/${OCCT_VERSION_TAG}.tar.gz
        URL_HASH SHA256=0d6913eae4bcc09a3653ceced6dda1aec11c35a1513d4c06762c9b002092c68a
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR .dummy    # just download it, don't add_subdirectory()
)

FetchContent_MakeAvailable(occt)

# RapidJSON for the glTF reader, master because the last release (2016) doesn't build with current compilers
FetchContent_Declare(
        rapidjson
        URL https://github.com/Tencent/rapidjson/archive/24b5e7a8b27f42fa16b96fc70aade9106cf7102f.tar.gz
        URL_HASH SHA256=2d2601a82d2d3b7e143a3c8d43ef616671391034bc46891a9816b79cf2d3e7a8
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR .dummy    # just download it
)

FetchContent_MakeAvailable(rapidjson)

# Toolkits we ask for, OCCT pulls in their dependencies
set(OCCT_TOOLKITS TKDESTEP TKDEIGES TKDESTL TKDEOBJ TKDEGLTF TKDEVRML TKDECascade TKXCAF TKMesh)

set(OCCT_ROOT "${CMAKE_BINARY_DIR}/_occt")
set(OCCT_INSTALL_DIR "${OCCT_ROOT}/install")

######################################################################
# Which configuration(s) of OCCT to build
#
# MSVC: Debug and Release use different runtimes and iterator debug levels
# so OCCT must be built to match. It installs Debug libs into libd/ and the
# rest into lib/ so both can live in the same prefix.
#
# Everything else: always Release (it's ABI compatible with Debug builds and
# debug OCCT is painfully slow on real STEP files).

get_property(_occt_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(MSVC)
    if(_occt_multi_config)
        set(OCCT_CONFIGS Debug Release)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(OCCT_CONFIGS Debug)
    else()
        set(OCCT_CONFIGS Release)
    endif()
else()
    set(OCCT_CONFIGS Release)
endif()

######################################################################
# Arguments for the OCCT configure step

# OCCT wants this space separated (and a ;-list would get split up below)
string(REPLACE ";" " " _occt_toolkits_arg "${OCCT_TOOLKITS}")

set(_occt_args
        -DCMAKE_INSTALL_PREFIX=${OCCT_INSTALL_DIR}
        -DINSTALL_DIR=${OCCT_INSTALL_DIR}
        -DINSTALL_DIR_LAYOUT=Unix
        -DBUILD_LIBRARY_TYPE=Static
        -DBUILD_CPP_STANDARD=C++17
        -DBUILD_MODULE_FoundationClasses=OFF
        -DBUILD_MODULE_ModelingData=OFF
        -DBUILD_MODULE_ModelingAlgorithms=OFF
        -DBUILD_MODULE_Visualization=OFF
        -DBUILD_MODULE_ApplicationFramework=OFF
        -DBUILD_MODULE_DataExchange=OFF
        -DBUILD_MODULE_Draw=OFF
        "-DBUILD_ADDITIONAL_TOOLKITS=${_occt_toolkits_arg}"
        -DBUILD_DOC_Overview=OFF
        -DBUILD_DOC_RefMan=OFF
        -DBUILD_GTEST=OFF
        -DBUILD_RESOURCES=OFF
        -DBUILD_USE_PCH=OFF
        -DBUILD_YACCLEX=OFF
        -DINSTALL_TEST_CASES=OFF
        -DUSE_TK=OFF
        -DUSE_TCL=OFF
        -DUSE_FREETYPE=OFF
        -DUSE_FREEIMAGE=OFF
        -DUSE_OPENGL=OFF
        -DUSE_GLES2=OFF
        -DUSE_XLIB=OFF
        -DUSE_D3D=OFF
        -DUSE_TBB=OFF
        -DUSE_VTK=OFF
        -DUSE_DRACO=OFF
        -DUSE_RAPIDJSON=ON
        "-D3RDPARTY_RAPIDJSON_INCLUDE_DIR=${rapidjson_SOURCE_DIR}/include"
        -DUSE_FFMPEG=OFF
        -DUSE_OPENVR=OFF
        -DUSE_EIGEN=OFF
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

# OCCT's cmake_minimum_required predates CMP0091, force it so that
# CMAKE_MSVC_RUNTIME_LIBRARY is honoured (static vs dynamic runtime must match ours)
if(MSVC)
    list(APPEND _occt_args
            -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()

foreach(_var CMAKE_TOOLCHAIN_FILE CMAKE_MAKE_PROGRAM CMAKE_AR CMAKE_RANLIB
        CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT CMAKE_OSX_ARCHITECTURES CMAKE_SYSROOT)
    if(${_var})
        string(REPLACE ";" "\;" _value "${${_var}}")
        list(APPEND _occt_args "-D${_var}=${_value}")
    endif()
endforeach()

# Static libgcc/libstdc++ (Linux/MinGW) is a link-time thing, nothing to pass down

set(_occt_generator_args -G "${CMAKE_GENERATOR}")
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND _occt_generator_args -A "${CMAKE_GENERATOR_PLATFORM}")
endif()
if(CMAKE_GENERATOR_TOOLSET)
    list(APPEND _occt_generator_args -T "${CMAKE_GENERATOR_TOOLSET}")
endif()

######################################################################
# Build it (unless the stamp says it's already done with these settings)

string(SHA256 _occt_signature "${OCCT_VERSION_TAG};${OCCT_CONFIGS};${_occt_generator_args};${_occt_args};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION}")

set(_occt_stamp "${OCCT_ROOT}/occt.stamp")
set(_occt_stamp_contents "")
if(EXISTS "${_occt_stamp}")
    file(READ "${_occt_stamp}" _occt_stamp_contents)
endif()

if(NOT _occt_stamp_contents STREQUAL _occt_signature)

    cmake_host_system_information(RESULT _occt_jobs QUERY NUMBER_OF_LOGICAL_CORES)

    file(REMOVE_RECURSE "${OCCT_INSTALL_DIR}")

    foreach(_config ${OCCT_CONFIGS})

        if(_occt_multi_config)
            set(_occt_build_dir "${OCCT_ROOT}/build")
            set(_occt_config_args)
        else()
            set(_occt_build_dir "${OCCT_ROOT}/build-${_config}")
            set(_occt_config_args -DCMAKE_BUILD_TYPE=${_config})
        endif()

        message(STATUS "OCCT: building ${OCCT_VERSION_TAG} (${_config}), this takes a while... (logs in ${OCCT_ROOT})")

        file(MAKE_DIRECTORY "${_occt_build_dir}")

        set(_occt_step_configure ${CMAKE_COMMAND} ${_occt_generator_args} ${_occt_args} ${_occt_config_args} -S "${occt_SOURCE_DIR}" -B "${_occt_build_dir}")
        set(_occt_step_build ${CMAKE_COMMAND} --build "${_occt_build_dir}" --config ${_config} --parallel ${_occt_jobs})
        set(_occt_step_install ${CMAKE_COMMAND} --install "${_occt_build_dir}" --config ${_config})

        foreach(_step configure build install)
            set(_occt_log "${OCCT_ROOT}/${_config}-${_step}.log")
            execute_process(
                    COMMAND ${_occt_step_${_step}}
                    OUTPUT_FILE "${_occt_log}"
                    ERROR_FILE "${_occt_log}"
                    RESULT_VARIABLE _occt_result)
            if(NOT _occt_result EQUAL 0)
                message(FATAL_ERROR "OCCT: ${_step} failed (${_config}), see ${_occt_log}")
            endif()
        endforeach()

    endforeach()

    # the intermediate build is several GB and isn't needed once it's installed
    file(GLOB _occt_build_dirs "${OCCT_ROOT}/build*")
    file(REMOVE_RECURSE ${_occt_build_dirs})

    file(WRITE "${_occt_stamp}" "${_occt_signature}")
    message(STATUS "OCCT: done")
endif()

######################################################################
# Import it

find_package(OpenCASCADE REQUIRED CONFIG
        PATHS "${OCCT_INSTALL_DIR}"
        NO_DEFAULT_PATH)

set(OCCT_LIBRARIES ${OCCT_TOOLKITS})

# Map configurations onto whichever OCCT build(s) we made
foreach(_toolkit ${OpenCASCADE_LIBRARIES})
    if(TARGET ${_toolkit})
        if(MSVC)
            set_target_properties(${_toolkit} PROPERTIES
                    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
                    MAP_IMPORTED_CONFIG_MINSIZEREL Release)
        else()
            set_target_properties(${_toolkit} PROPERTIES
                    MAP_IMPORTED_CONFIG_DEBUG Release
                    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
                    MAP_IMPORTED_CONFIG_MINSIZEREL Release)
        endif()
    endif()
endforeach()

add_library(occt INTERFACE)

target_link_libraries(occt INTERFACE ${OCCT_LIBRARIES})

# Standard_EXPORT is dllimport on Windows unless this is defined
target_compile_definitions(occt INTERFACE OCCT_STATIC_BUILD)

message(STATUS "OCCT: ${OpenCASCADE_MAJOR_VERSION}.${OpenCASCADE_MINOR_VERSION}.${OpenCASCADE_MAINTENANCE_VERSION} from ${OCCT_INSTALL_DIR}")
