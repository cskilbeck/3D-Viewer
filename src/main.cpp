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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#endif

#include "model_check.h"
#include "settings.h"
#include "single_instance.h"
#include "util.h"
#include "viewer.h"

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
// the command line arguments (not the program name) as paths

std::vector<std::filesystem::path> command_line_args(int argc, char **argv)
{
    std::vector<std::filesystem::path> args;
#ifdef _WIN32
    // argv is in the ANSI codepage, get the wide version for non-ascii paths
    (void)argc;
    (void)argv;
    int wargc;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if(wargv != nullptr) {
        for(int i = 1; i < wargc; ++i) {
            args.emplace_back(wargv[i]);
        }
        LocalFree(wargv);
    }
#else
    for(int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
#endif
    return args;
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
        // a GUI app has no console: use the parent's, unless output is already going somewhere (a pipe or a file)
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        bool has_stdout = out != nullptr && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN;
        if(!has_stdout && AttachConsole(ATTACH_PARENT_PROCESS)) {
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

    std::vector<std::filesystem::path> args = command_line_args(argc, argv);

    // --check <directory> [--update]: test loading the models, no window
    if(!args.empty() && args[0] == "--check") {
        logging::log_set_level(logging::log_level_error);
        bool update = std::find(args.begin(), args.end(), std::filesystem::path("--update")) != args.end();
        std::filesystem::path directory = args.size() > 1 && args[1] != "--update" ? args[1] : std::filesystem::path("models");
        int result = check_models(directory, update);
        fflush(stdout);
        std::quick_exit(result);
    }

    // reuse window: if one's running already, it opens the file (or just comes to the front)
    {
        settings_t settings;
        settings.load(settings_path());
        if(settings.reuse_window && single_instance::send_to_running(args.empty() ? std::filesystem::path() : args[0])) {
            fflush(stdout);
            std::quick_exit(0);
        }
    }

    viewer window;
    window.init();

    // the first argument, if there is one, is a file to open
    if(!args.empty()) {
        window.open_file(args[0]);
    }

    while(window.update()) {}

    // Everything that matters (settings, GPU, SDL) has been shut down by now.
    // Skip static destructors: OCCT's teardown at exit is slow for big models
    // and its type registry asserts (Standard_Type.cxx) when statically linked
    fflush(stdout);
    fflush(stderr);
    std::quick_exit(0);
}
