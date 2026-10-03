//////////////////////////////////////////////////////////////////////
// 3D Viewer
//
// X blank application (window, ImGui docking, settings, file dialog)
// X load STEP files with OpenCascade
// X model tree
// X 3D view (shaded + edges, orbit camera)
//
// up axis (Y up models)
// zoom to cursor
// select parts (tree <-> view)
// show/hide parts
// transparency
//

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#endif

#include "step_viewer.h"

LOG_CONTEXT("main", info);

//////////////////////////////////////////////////////////////////////

int flushed_puts(char const *s)
{
    int x = puts(s);
    fflush(stdout);
    return x;
}

#ifdef _WIN32
int output_debug_string(char const *s)
{
    OutputDebugStringA(s);
    OutputDebugStringA("\n");
    return 0;
}
#endif

//////////////////////////////////////////////////////////////////////
// first command line argument, if there is one, is a file to open

std::filesystem::path command_line_file(int argc, char **argv)
{
#ifdef _WIN32
    // argv is in the ANSI codepage, get the wide version for non-ascii paths
    int wargc;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::filesystem::path path;
    if(wargv != nullptr) {
        if(wargc > 1) {
            path = wargv[1];
        }
        LocalFree(wargv);
    }
    return path;
#else
    if(argc > 1) {
        return argv[1];
    }
    return {};
#endif
}

//////////////////////////////////////////////////////////////////////

int main(int argc, char **argv)
{
#ifdef _DEBUG
    logging::log_set_level(logging::log_level_debug);
#else
    logging::log_set_level(logging::log_level_warning);
#endif

#ifdef _WIN32
    if(!IsDebuggerPresent()) {
        if(AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE *dummy;
            freopen_s(&dummy, "CONOUT$", "w", stdout);
            freopen_s(&dummy, "CONOUT$", "w", stderr);
            freopen_s(&dummy, "CONIN$", "r", stdin);
        }
        logging::log_set_emitter_function(puts);
    } else {
#if defined(LOG_USE_OUTPUT_DEBUG_STRING)
        logging::log_set_emitter_function(output_debug_string);
#else
        logging::log_set_emitter_function(flushed_puts);
#endif
    }
#else
    logging::log_set_emitter_function(puts);
#endif

    step_viewer window;
    window.init();

    std::filesystem::path file = command_line_file(argc, argv);
    if(!file.empty()) {
        window.open_file(file);
    }

    while(window.update()) {}

    // Everything that matters (settings, GPU, SDL) has been shut down by now.
    // Skip static destructors: OCCT's teardown at exit is slow for big models
    // and its type registry asserts (Standard_Type.cxx) when statically linked
    fflush(stdout);
    fflush(stderr);
    std::quick_exit(0);
}
