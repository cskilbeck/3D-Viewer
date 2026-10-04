#pragma once

#include <atomic>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include "gpu_window.h"
#include "gpu_device.h"
#include "camera.h"
#include "model_renderer.h"
#include "settings.h"
#include "step_model.h"

//////////////////////////////////////////////////////////////////////

struct step_viewer : gpu_window
{
    double last_frame_cpu_time{};

    // window screen coordinates
    int window_width{};
    int window_height{};

    // viewport - area of the window left over after ImGui docking (in window coordinates)
    int viewport_xpos{};
    int viewport_ypos{};
    int viewport_width{};
    int viewport_height{};

    // SDL_GPU backend
    gpu::device gpu_dev{};

    // Per-frame GPU state (shared between gpu_render and on_gpu_imgui)
    SDL_GPUCommandBuffer *gpu_cmd{};
    SDL_GPUTexture *gpu_swapchain_texture{};

    void gpu_render();
    void on_gpu_imgui() override;

    // 3D view
    model_renderer renderer;
    camera cam;

    enum class drag_mode
    {
        none,
        click,    // LMB down, becomes orbit if it moves far enough
        orbit,
        pan,
        zoom
    };

    drag_mode dragging{ drag_mode::none };
    float mouse_x{};
    float mouse_y{};
    float click_x{};
    float click_y{};
    int ignore_mouse_moves{};    // switching the cursor mode can produce a bogus move or two
    float zoom_x{};              // where the (hidden) cursor is during MMB zoom
    float zoom_y{};

    // direction of the ray through a point in the viewport
    gpu::vec3 mouse_ray(float x, float y) const;

    // how far one zoom click moves the camera from eye
    float zoom_click_distance(gpu::vec3 const &eye) const;

    // a point on the plane through the camera target (at right angles to the view) under the mouse
    gpu::vec3 view_plane_point(float x, float y) const;

    // zoom by some (fractional) wheel clicks, keeping what's under x, y where it is
    void apply_zoom(float clicks, float x, float y);

    // smoothed wheel zoom: clicks still to do, used up over a few frames
    float pending_zoom{};
    float pending_zoom_x{};
    float pending_zoom_y{};
    double pending_zoom_time{};
    void update_zoom(double now);
    void stop_zoom()
    {
        pending_zoom = 0;
    }

    void handle_shortcuts();
    void add_recent_file(std::filesystem::path const &path);
    float pan_depth{};           // depth of the point grabbed when panning started

    void start_pan();

    // the 3D point under the mouse: the nearest surface, or if there isn't one,
    // where the mouse ray crosses the plane through the camera target
    gpu::vec3 point_under_mouse(float x, float y) const;

    // selection - a node in the tree (part or assembly) and the parts that are in it
    int selected_node{ -1 };
    std::vector<int> selected_parts;
    bool reveal_selection{ false };    // expand + scroll the tree to show it

    void select_node(int node, bool reveal);

    // View > Isolate hides everything but the selection, Unisolate shows everything again
    bool isolated{ false };
    void toggle_isolate();
    void pick(float x, float y);

    bool mouse_in_viewport() const;
    // bounding box of the selected parts / whole model (in vertex space), false if there's nothing
    bool selection_bounds(gpu::vec3 &box_min, gpu::vec3 &box_max) const;
    bool model_bounds(gpu::vec3 &box_min, gpu::vec3 &box_max) const;

    float viewport_aspect() const;

    // fit the selection (or everything if nothing's selected) in the view
    void fit_to_view();

    // isometric, whole model
    void reset_view();

    // the current model
    std::unique_ptr<step_model> model;

    // background loading admin
    std::filesystem::path loading_path;
    std::atomic<float> load_progress{};
    std::atomic<bool> loading{ false };

    // loader thread hands the result over via these
    std::mutex loaded_mutex;
    std::unique_ptr<step_model> loaded_model;
    std::string load_error;
    bool load_finished{ false };

    // declared after the things it touches so it's joined before they're destroyed
    std::jthread loader;

    void check_loaded();
    void model_tree_ui(int node_index, bool parent_visible);

    settings_t settings;

    // settings dialog, Revert goes back to how things were when it was opened
    bool settings_open{ false };
    settings_t settings_snapshot;

    void open_settings();
    void revert_settings();
    void default_settings();
    void settings_ui();

    // "Loading <file>" + progress bar, goes in the Info window, or the toolbar, or its own window
    std::string loading_text() const;
    void loading_window_ui();

    void open_file(std::filesystem::path const &path);

    void file_open();

    static std::expected<std::filesystem::path, std::error_code> load_file_dialog();

    void save_settings(std::filesystem::path const &path);
    void load_settings(std::filesystem::path const &path);

    void on_window_size(int w, int h) override;
    void on_window_refresh() override;

    bool on_init() override;
    void on_render() override;
    void on_closed() override;
    void on_key(int key, int scancode, int action, int mods) override;
    void on_drop(int count, const char **paths) override;
    void on_mouse_button(int button, int action, int mods) override;
    void on_mouse_move(double xpos, double ypos) override;
    void on_scroll(double xoffset, double yoffset) override;

    // whenever mouse moved or clicked or key pressed, call this
    void set_active();

    // call this to decide whether to poll or wait for events
    bool is_idle() override;
    int idle_timeout_ms() override;

    double idle_timestamp{};

    void ui();

    std::string window_name() const override;
};
