//////////////////////////////////////////////////////////////////////
// Reuse window: opening a file when 3D Viewer is already running sends it to that
// one (which brings itself to the front) instead of starting another
//
// The running one listens on a local (Unix domain) socket, 3D-Viewer-Active in the
// config directory (with the settings), which works the same way on Windows (10 and
// later), macOS and Linux. A request is the UTF-8 path of the file to open, or nothing
// to just come to the front

#pragma once

#include <filesystem>
#include <functional>

namespace single_instance
{
    // Send a file (empty: just come to the front) to the running instance.
    // Returns false if there isn't one
    bool send_to_running(std::filesystem::path const &file);

    // Become the running instance: on_request is called (on a background thread)
    // with each file asked for. Returns false if another instance is listening already
    bool start_listening(std::function<void(std::filesystem::path const &)> on_request);

    void stop_listening();

}    // namespace single_instance
