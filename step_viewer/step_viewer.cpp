#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <expected>
#include <format>

#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include <nfd.h>

#include "step_viewer.h"
#include "util.h"

#include "assets/matsym_codepoints_utf8.h"

using namespace sdl_compat;

LOG_CONTEXT("step_viewer", info);

namespace
{
    // everything which can be loaded (OpenCascade and Assimp), as "step,stp,..."
    std::string const &supported_file_spec()
    {
        static std::string const spec = [] {
            std::string result;
            for(std::string const &extension : supported_file_extensions()) {
                result += (result.empty() ? "" : ",") + extension;
            }
            return result;
        }();
        return spec;
    }

}    // namespace

//////////////////////////////////////////////////////////////////////
// If the user is interacting, it's not idle
// But also... suppress idleness for a short while

void step_viewer::set_active()
{
    idle_timestamp = get_time();
}

bool step_viewer::is_idle()
{
    if(cam.is_animating() || pending_zoom != 0) {
        return false;
    }
    // poll events for this much time after last call to set_active()
    double constexpr idle_timer = 0.1f;
    return get_time() - idle_timestamp > idle_timer;
}

int step_viewer::idle_timeout_ms()
{
    // keep the progress bar moving while loading (~30fps)
    return loading ? 33 : -1;
}

//////////////////////////////////////////////////////////////////////

std::string step_viewer::window_name() const
{
    return app_friendly_name;
}

//////////////////////////////////////////////////////////////////////

void step_viewer::on_drop(int count, const char **paths)
{
    // only one model at a time, take the last one
    if(count > 0) {
        open_file(paths[count - 1]);
    }
}

//////////////////////////////////////////////////////////////////////

bool step_viewer::mouse_in_viewport() const
{
    return mouse_x >= viewport_xpos && mouse_x < viewport_xpos + viewport_width && mouse_y >= viewport_ypos && mouse_y < viewport_ypos + viewport_height;
}

//////////////////////////////////////////////////////////////////////
// LMB click select, LMB drag orbit, RMB/Shift+LMB pan, MMB drag zoom, wheel zoom

void step_viewer::on_mouse_button(int button, int action, int mods)
{
    set_active();

    if(action == ACTION_RELEASE) {
        if(dragging == drag_mode::click && button == MOUSE_BUTTON_LEFT) {
            pick(mouse_x, mouse_y);
        }
        if(dragging == drag_mode::zoom) {
            // the cursor goes back to where the zoom started
            set_input_mode_cursor_normal();
            mouse_x = saved_cursor_x;
            mouse_y = saved_cursor_y;
        }
        dragging = drag_mode::none;
        return;
    }

    if(!mouse_in_viewport() || model == nullptr) {
        return;
    }

    cam.stop_animation();
    stop_zoom();

    if(button == MOUSE_BUTTON_LEFT) {
        if((mods & KMOD_SHIFT_FLAG) != 0) {
            start_pan();
        } else {
            dragging = drag_mode::click;
            click_x = mouse_x;
            click_y = mouse_y;
        }
    } else if(button == MOUSE_BUTTON_RIGHT) {
        start_pan();
    } else if(button == MOUSE_BUTTON_MIDDLE) {
        // hide the cursor and use relative motion so the window edge doesn't stop it
        dragging = drag_mode::zoom;
        zoom_x = mouse_x;
        zoom_y = mouse_y;
        set_input_mode_cursor_disabled();
        ignore_mouse_moves = 2;
    }
}

//////////////////////////////////////////////////////////////////////

gpu::vec3 step_viewer::mouse_ray(float x, float y) const
{
    float ndc_x = viewport_width > 0 ? (x - viewport_xpos) / viewport_width * 2.0f - 1.0f : 0.0f;
    float ndc_y = viewport_height > 0 ? 1.0f - (y - viewport_ypos) / viewport_height * 2.0f : 0.0f;
    gpu::vec3 origin;
    gpu::vec3 direction;
    cam.ray(ndc_x, ndc_y, viewport_aspect(), origin, direction);
    return direction;
}

//////////////////////////////////////////////////////////////////////
// A click moves a fraction of the distance from the eye to the model's bounding
// box, so far away every click magnifies by about the same amount. That distance
// doesn't depend on what's under the mouse, so the speed is predictable. Near or
// inside the box it would shrink to nothing (the squishy cushion), so it has a
// floor based on the size of the selection (or the model) which makes close up
// zooming a steady crawl at a speed which suits whatever's being looked at

float step_viewer::zoom_click_distance(gpu::vec3 const &eye) const
{
    gpu::vec3 box_min;
    gpu::vec3 box_max;
    if(!model_bounds(box_min, box_max)) {
        return cam.scene_radius * settings.zoom_step;
    }

    auto outside = [](float v, float lo, float hi) { return std::max({ lo - v, 0.0f, v - hi }); };
    gpu::vec3 gap{ outside(eye.x, box_min.x, box_max.x), outside(eye.y, box_min.y, box_max.y), outside(eye.z, box_min.z, box_max.z) };
    float box_distance = gap.length();

    float size = (box_max - box_min).length();
    gpu::vec3 selection_min;
    gpu::vec3 selection_max;
    if(selection_bounds(selection_min, selection_max)) {
        size = (selection_max - selection_min).length();
    }
    size = std::max(size, cam.scene_radius * 1e-4f);

    return settings.zoom_step * std::max(box_distance, settings.zoom_floor * size);
}

//////////////////////////////////////////////////////////////////////

gpu::vec3 step_viewer::view_plane_point(float x, float y) const
{
    gpu::vec3 origin;
    gpu::vec3 direction;
    float ndc_x = viewport_width > 0 ? (x - viewport_xpos) / viewport_width * 2.0f - 1.0f : 0.0f;
    float ndc_y = viewport_height > 0 ? 1.0f - (y - viewport_ypos) / viewport_height * 2.0f : 0.0f;
    cam.ray(ndc_x, ndc_y, viewport_aspect(), origin, direction);
    gpu::vec3 forward = cam.basis().forward;
    float along = gpu::dot(direction, forward);
    return along > 1e-6f ? origin + direction * (gpu::dot(cam.target - origin, forward) / along) : cam.target;
}

//////////////////////////////////////////////////////////////////////
// Perspective moves the camera along the ray through the mouse, orthographic
// can't (moving doesn't change anything) so it scales the view around the mouse

void step_viewer::apply_zoom(float clicks, float x, float y)
{
    if(cam.orthographic) {
        cam.scale_view(clicks, view_plane_point(x, y), (float)viewport_height);
    } else {
        cam.move(mouse_ray(x, y) * (clicks * zoom_click_distance(cam.eye())), (float)viewport_height);
    }
}

//////////////////////////////////////////////////////////////////////
// Use up some of the pending zoom each frame (exponential ease out)

void step_viewer::update_zoom(double now)
{
    if(pending_zoom == 0) {
        return;
    }
    float dt = (float)(now - pending_zoom_time);
    pending_zoom_time = now;

    float time_constant = std::max(settings.zoom_smooth_time, 0.001f) / 3.0f;    // ~95% done after zoom_smooth_time
    float clicks = pending_zoom * (1.0f - std::exp(-dt / time_constant));
    if(std::abs(pending_zoom - clicks) < 1e-3f) {
        clicks = pending_zoom;
    }
    apply_zoom(clicks, pending_zoom_x, pending_zoom_y);
    pending_zoom -= clicks;
    if(std::abs(pending_zoom) < 1e-3f) {
        pending_zoom = 0;
    }
}

//////////////////////////////////////////////////////////////////////

gpu::vec3 step_viewer::point_under_mouse(float x, float y) const
{
    float ndc_x = viewport_width > 0 ? (x - viewport_xpos) / viewport_width * 2.0f - 1.0f : 0.0f;
    float ndc_y = viewport_height > 0 ? 1.0f - (y - viewport_ypos) / viewport_height * 2.0f : 0.0f;

    gpu::vec3 origin;
    gpu::vec3 direction;
    cam.ray(ndc_x, ndc_y, viewport_aspect(), origin, direction);

    if(model != nullptr) {
        std::vector<pick_hit> hits = model->pick(origin, direction);
        if(!hits.empty()) {
            return origin + direction * hits[0].distance;
        }
    }

    gpu::vec3 forward = cam.basis().forward;
    float along = gpu::dot(direction, forward);
    if(along <= 1e-6f) {
        return cam.target;
    }
    return origin + direction * (gpu::dot(cam.target - origin, forward) / along);
}

//////////////////////////////////////////////////////////////////////
// Grab whatever's under the mouse, panning keeps it under the mouse

void step_viewer::start_pan()
{
    dragging = drag_mode::pan;

    gpu::vec3 eye = cam.eye();
    gpu::vec3 forward = cam.basis().forward;

    // over the model: the surface under the mouse
    // over empty space: the middle of the model (the origin, vertices are relative to it), not the camera
    // target which zooming leaves just in front of the camera
    gpu::vec3 origin;
    gpu::vec3 direction;
    float ndc_x = viewport_width > 0 ? (mouse_x - viewport_xpos) / viewport_width * 2.0f - 1.0f : 0.0f;
    float ndc_y = viewport_height > 0 ? 1.0f - (mouse_y - viewport_ypos) / viewport_height * 2.0f : 0.0f;
    cam.ray(ndc_x, ndc_y, viewport_aspect(), origin, direction);

    std::vector<pick_hit> hits;
    if(model != nullptr) {
        hits = model->pick(origin, direction);
    }
    if(!hits.empty()) {
        pan_depth = gpu::dot(origin + direction * hits[0].distance - eye, forward);
    } else {
        pan_depth = gpu::dot(gpu::vec3{} - eye, forward);
    }

    // in (or past) the model the depth gets tiny, don't let it crawl
    pan_depth = std::max(pan_depth, cam.scene_radius * 0.1f);
}

//////////////////////////////////////////////////////////////////////
// Select the part under the mouse. Clicking again in the same place
// selects the next part along the ray (wrapping around)

void step_viewer::pick(float x, float y)
{
    if(model == nullptr || viewport_width <= 0 || viewport_height <= 0) {
        return;
    }

    float ndc_x = (x - viewport_xpos) / viewport_width * 2.0f - 1.0f;
    float ndc_y = 1.0f - (y - viewport_ypos) / viewport_height * 2.0f;
    float aspect = (float)viewport_width / (float)viewport_height;

    gpu::vec3 origin;
    gpu::vec3 direction;
    cam.ray(ndc_x, ndc_y, aspect, origin, direction);

    std::vector<pick_hit> hits = model->pick(origin, direction);

    if(hits.empty()) {
        select_node(-1, false);
        return;
    }

    // if a part under the mouse is already selected, move on to the one behind it
    size_t next = 0;
    if(selected_parts.size() == 1) {
        auto current = std::find_if(hits.begin(), hits.end(), [&](pick_hit const &hit) { return hit.part == selected_parts[0]; });
        if(current != hits.end()) {
            next = (current - hits.begin() + 1) % hits.size();
        }
    }

    select_node(model->parts[hits[next].part].node, true);
}

//////////////////////////////////////////////////////////////////////

void step_viewer::toggle_isolate()
{
    if(model == nullptr) {
        return;
    }
    if(isolated) {
        model->show_all();
        isolated = false;
    } else if(selected_node >= 0) {
        model->isolate(selected_node);
        isolated = true;
    }
    set_active();
}

//////////////////////////////////////////////////////////////////////

void step_viewer::select_node(int node, bool reveal)
{
    selected_node = node;
    selected_parts.clear();
    if(model != nullptr && node >= 0) {
        model->get_parts(node, selected_parts);
    }
    reveal_selection = reveal && node >= 0;
    set_active();
}

//////////////////////////////////////////////////////////////////////

void step_viewer::on_mouse_move(double xpos, double ypos)
{
    set_active();

    float dx = (float)xpos - mouse_x;
    float dy = (float)ypos - mouse_y;
    mouse_x = (float)xpos;
    mouse_y = (float)ypos;

    if(ignore_mouse_moves > 0) {
        ignore_mouse_moves -= 1;
        return;
    }

    // move far enough with LMB down and it's an orbit, not a click
    float constexpr click_threshold = 4.0f;

    // orbit around the middle of the selection, or the whole model if nothing's selected
    gpu::vec3 pivot;
    gpu::vec3 pivot_max;
    gpu::vec3 const *orbit_pivot = nullptr;
    if(selection_bounds(pivot, pivot_max) || model_bounds(pivot, pivot_max)) {
        pivot = (pivot + pivot_max) * 0.5f;
        orbit_pivot = &pivot;
    }

    switch(dragging) {
    case drag_mode::click:
        if(std::abs(mouse_x - click_x) > click_threshold || std::abs(mouse_y - click_y) > click_threshold) {
            dragging = drag_mode::orbit;
            cam.orbit(mouse_x - click_x, mouse_y - click_y, orbit_pivot);
        }
        break;
    case drag_mode::orbit:
        cam.orbit(dx, dy, orbit_pivot);
        break;
    case drag_mode::pan:
        cam.pan(dx, dy, (float)viewport_height, pan_depth);
        break;
    case drag_mode::zoom: {
        // right/up zooms in, left/down zooms out, about one wheel click per 50 pixels
        float constexpr pixels_per_click = 50.0f;
        apply_zoom((dx - dy) / pixels_per_click, zoom_x, zoom_y);
    } break;
    case drag_mode::none:
        break;
    }
}

//////////////////////////////////////////////////////////////////////

void step_viewer::on_scroll(double xoffset, double yoffset)
{
    set_active();
    if(model != nullptr && mouse_in_viewport()) {
        cam.stop_animation();
        if(settings.zoom_smooth_time <= 0) {
            apply_zoom((float)yoffset, mouse_x, mouse_y);
        } else {
            if(pending_zoom == 0) {
                pending_zoom_time = get_time();
            }
            pending_zoom += (float)yoffset;
            pending_zoom_x = mouse_x;
            pending_zoom_y = mouse_y;
        }
    }
}

//////////////////////////////////////////////////////////////////////

bool step_viewer::selection_bounds(gpu::vec3 &box_min, gpu::vec3 &box_max) const
{
    if(model == nullptr || selected_parts.empty()) {
        return false;
    }
    float constexpr big = 3.4e38f;
    box_min = { big, big, big };
    box_max = { -big, -big, -big };
    for(int part_index : selected_parts) {
        step_part const &part = model->parts[part_index];
        box_min = { std::min(box_min.x, part.bounds_min.x), std::min(box_min.y, part.bounds_min.y), std::min(box_min.z, part.bounds_min.z) };
        box_max = { std::max(box_max.x, part.bounds_max.x), std::max(box_max.y, part.bounds_max.y), std::max(box_max.z, part.bounds_max.z) };
    }
    return true;
}

//////////////////////////////////////////////////////////////////////

bool step_viewer::model_bounds(gpu::vec3 &box_min, gpu::vec3 &box_max) const
{
    if(model == nullptr) {
        return false;
    }
    // vertices are relative to the model center
    box_min = { (float)(model->extent_min[0] - model->center[0]), (float)(model->extent_min[1] - model->center[1]), (float)(model->extent_min[2] - model->center[2]) };
    box_max = { (float)(model->extent_max[0] - model->center[0]), (float)(model->extent_max[1] - model->center[1]), (float)(model->extent_max[2] - model->center[2]) };
    return true;
}

//////////////////////////////////////////////////////////////////////

float step_viewer::viewport_aspect() const
{
    return viewport_height > 0 ? (float)viewport_width / (float)viewport_height : 1.0f;
}

//////////////////////////////////////////////////////////////////////

void step_viewer::fit_to_view()
{
    gpu::vec3 box_min;
    gpu::vec3 box_max;
    if(selection_bounds(box_min, box_max) || model_bounds(box_min, box_max)) {
        gpu::vec3 new_target;
        float new_distance;
        cam.fit_box_target(box_min, box_max, viewport_aspect(), settings.fit_border, new_target, new_distance);
        stop_zoom();
        cam.animate_to(new_target, new_distance, get_time(), settings.fit_duration);
        set_active();
    }
}

//////////////////////////////////////////////////////////////////////

void step_viewer::axis_view(int axis)
{
    gpu::vec3 box_min;
    gpu::vec3 box_max;
    if(!selection_bounds(box_min, box_max) && !model_bounds(box_min, box_max)) {
        return;
    }

    // the same button again and the camera's exactly where it left it: other side
    bool unmoved = axis == last_axis_view && cam.target.x == last_axis_view_target.x && cam.target.y == last_axis_view_target.y &&
                   cam.target.z == last_axis_view_target.z && cam.forward.x == last_axis_view_forward.x &&
                   cam.forward.y == last_axis_view_forward.y && cam.forward.z == last_axis_view_forward.z && cam.distance == last_axis_view_distance;
    bool positive = unmoved ? !last_axis_view_positive : true;

    // from the positive side means looking towards negative
    float sign = positive ? -1.0f : 1.0f;
    gpu::vec3 direction{ axis == 0 ? sign : 0.0f, axis == 1 ? sign : 0.0f, axis == 2 ? sign : 0.0f };
    gpu::vec3 up_hint = axis == 2 ? gpu::vec3{ 0, 1, 0 } : gpu::vec3{ 0, 0, 1 };

    stop_zoom();
    cam.look_along(direction, up_hint);
    cam.fit_box(box_min, box_max, viewport_aspect(), settings.fit_border);
    set_active();

    last_axis_view = axis;
    last_axis_view_positive = positive;
    last_axis_view_target = cam.target;
    last_axis_view_forward = cam.forward;
    last_axis_view_distance = cam.distance;
}

//////////////////////////////////////////////////////////////////////

void step_viewer::reset_view()
{
    gpu::vec3 box_min;
    gpu::vec3 box_max;
    if(model_bounds(box_min, box_max)) {
        stop_zoom();
        cam.set_isometric();
        cam.fit_box(box_min, box_max, viewport_aspect(), settings.fit_border);
        set_active();
    }
}

//////////////////////////////////////////////////////////////////////

void step_viewer::on_key(int key, int scancode, int action, int mods)
{
    set_active();

    // shortcuts are done in handle_shortcuts(), keys don't get here when an ImGui window has focus
}

//////////////////////////////////////////////////////////////////////
// Keyboard shortcuts work whatever has focus, except while typing into something

void step_viewer::handle_shortcuts()
{
    ImGuiIO &io = ImGui::GetIO();
    if(io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        return;
    }
    if(ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        set_should_close();
    }
    if(ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
        file_open();
    }
    if(!io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !io.KeyShift) {
        if(ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            fit_to_view();
        }
        if(ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            settings.show_edges = !settings.show_edges;
        }
        if(ImGui::IsKeyPressed(ImGuiKey_I, false)) {
            toggle_isolate();
        }
        if(ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            reset_view();
        }
        if(ImGui::IsKeyPressed(ImGuiKey_G, false)) {
            settings.show_grid = !settings.show_grid;
        }
        if(ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            settings.show_axes = !settings.show_axes;
        }
    }
}

//////////////////////////////////////////////////////////////////////
// most recent first, no duplicates, saved straight away

void step_viewer::add_recent_file(std::filesystem::path const &path)
{
    size_t constexpr max_recent_files = 10;
    std::u8string utf8 = path.u8string();
    std::string name(utf8.begin(), utf8.end());
    auto &recent = settings.recent_files;
    recent.erase(std::remove(recent.begin(), recent.end(), name), recent.end());
    recent.insert(recent.begin(), name);
    if(recent.size() > max_recent_files) {
        recent.resize(max_recent_files);
    }
    save_settings(settings_path());
}

//////////////////////////////////////////////////////////////////////

void step_viewer::on_closed()
{
    if(loader.joinable()) {
        loader.request_stop();
        loader.join();
    }
    model.reset();
    loaded_model.reset();
    loaded_textures.release();    // before the device goes
    save_settings(settings_path());
    NFD_Quit();
    gpu_window::on_closed();
    renderer.cleanup();
    gpu_dev.shutdown();
}

//////////////////////////////////////////////////////////////////////

void step_viewer::load_settings(std::filesystem::path const &path)
{
    if(settings.load(path)) {
        LOG_DEBUG("Settings loaded...");
        window_state.width = settings.window_width;
        window_state.height = settings.window_height;
        window_state.x = settings.window_xpos;
        window_state.y = settings.window_ypos;
        window_state.isMaximized = settings.window_maximized;
    }
}

//////////////////////////////////////////////////////////////////////

void step_viewer::save_settings(std::filesystem::path const &path)
{
    LOG_INFO("save settings");
    window_state = get_window_state();
    settings.window_width = window_state.width;
    settings.window_height = window_state.height;
    settings.window_xpos = window_state.x;
    settings.window_ypos = window_state.y;
    settings.window_maximized = window_state.isMaximized;
    settings.save(path);
}

//////////////////////////////////////////////////////////////////////

void step_viewer::on_window_size(int w, int h)
{
    gpu_window::on_window_size(w, h);
    window_width = w;
    window_height = h;
}

//////////////////////////////////////////////////////////////////////

void step_viewer::on_window_refresh()
{
    gpu_window::on_window_refresh();
    if(frames == 0) {
        return;    // needs a proper first frame before refresh renders
    }
    on_frame();
}

//////////////////////////////////////////////////////////////////////

bool step_viewer::on_init()
{

#ifdef WIN32
    HICON hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(1));
    if(hIcon) {
        HWND hwnd = (HWND)get_native_window_handle();
        if(hwnd) {
            SendMessage(hwnd, WM_SETICON, ICON_SMALL2, (LPARAM)hIcon);
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        }
    }
#endif

    NFD_Init();

    get_window_size(&window_width, &window_height);

    if(!gpu_dev.init(window)) {
        LOG_ERROR("GPU device init failed");
        return false;
    }

    SDL_GPUTextureFormat swapchain_format = SDL_GetGPUSwapchainTextureFormat(gpu_dev.gpu, window);

    // Initialize ImGui backends
    ImGui_ImplSDL3_InitForOther(window);

    ImGui_ImplSDLGPU3_InitInfo imgui_gpu_info{};
    imgui_gpu_info.Device = gpu_dev.gpu;
    imgui_gpu_info.ColorTargetFormat = swapchain_format;
    imgui_gpu_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&imgui_gpu_info);

    if(!renderer.init(gpu_dev, swapchain_format)) {
        LOG_ERROR("Renderer init failed");
        return false;
    }

    // settings used to be in the config directory, pick them up from there if there's nothing new yet
    std::filesystem::path path = settings_path();
    std::error_code error;
    if(!std::filesystem::exists(path, error)) {
        path = config_path(app_name, "settings.json");
    }
    load_settings(path);

    return true;
}

//////////////////////////////////////////////////////////////////////

void step_viewer::open_file(std::filesystem::path const &path)
{
    LOG_INFO("Open {}", path.string());

    // cancel any load in progress (blocks until it notices)
    if(loader.joinable()) {
        loader.request_stop();
        loader.join();
    }

    {
        std::lock_guard _(loaded_mutex);
        loaded_model.reset();
        loaded_textures.release();
        load_error.clear();
        load_finished = false;
    }

    loading_path = path;
    load_progress = 0;
    loading = true;

    loader = std::jthread([this, path](std::stop_token stop) {
        auto result = load_step_model(path, stop, load_progress);

        // big textures take a while to upload, do it here rather than stalling the main thread
        model_renderer::texture_set textures;
        if(result.has_value() && !stop.stop_requested()) {
            textures = renderer.upload_textures(*result.value(), stop);
        }
        {
            std::lock_guard _(loaded_mutex);
            if(result.has_value()) {
                loaded_model = std::move(result.value());
                loaded_textures = std::move(textures);
            } else {
                load_error = result.error();
            }
            load_finished = true;
        }
        loading = false;

        // wake up the main loop
        SDL_Event e{};
        e.type = SDL_EVENT_USER;
        SDL_PushEvent(&e);
    });

    set_active();
}

//////////////////////////////////////////////////////////////////////
// pick up the result of a background load

void step_viewer::check_loaded()
{
    std::lock_guard _(loaded_mutex);
    if(!load_finished) {
        return;
    }
    load_finished = false;
    set_active();
    if(loaded_model) {
        model = std::move(loaded_model);
        isolated = false;
        select_node(-1, false);
        renderer.set_model(*model, std::move(loaded_textures));
        cam.scene_radius = (float)model->radius;
        reset_view();
        model->edges = {};    // vertices/indices are kept for picking
        add_recent_file(model->path);
    } else if(load_error != "Cancelled") {
        LOG_ERROR("{}", load_error);
        ImGui::OpenPopup("Load failed");
    }
}

//////////////////////////////////////////////////////////////////////

void step_viewer::model_tree_ui(int node_index, bool parent_visible)
{
    step_node const &node = model->nodes[node_index];
    bool visible = parent_visible && node.visible;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if(node.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if(node_index == selected_node) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    ImGui::PushID(node_index);

    // show/hide - things which are hidden (themselves or by an ancestor) are dimmed
    if(!visible) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    if(ImGui::Button(node.visible ? MATSYM_visibility "##visible" : MATSYM_visibility_off "##visible")) {
        model->set_visible(node_index, !node.visible);
        set_active();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip(node.visible ? "Hide" : "Show");
    ImGui::SameLine();

    if(node.has_color) {
        ImGui::ColorButton("##color",
                           { node.color[0], node.color[1], node.color[2], 1.0f },
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
        ImGui::SameLine();
    }

    // selected in the 3D view - open the way down to it
    // (right before the tree node, any item in between would use up SetNextItemOpen)
    if(reveal_selection && !node.children.empty() && model->is_ancestor(node_index, selected_node) && node_index != selected_node) {
        ImGui::SetNextItemOpen(true);
    }

    char const *icon = node.is_assembly ? MATSYM_folder : MATSYM_deployed_code;
    bool open = ImGui::TreeNodeEx("##node", flags, "%s %s", icon, node.name.c_str());

    if(!visible) {
        ImGui::PopStyleColor();
    }

    if(ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        select_node(node_index, false);
    }

    if(reveal_selection && node_index == selected_node) {
        ImGui::SetScrollHereY(0.5f);
    }

    if(open && !node.children.empty()) {
        for(int child : node.children) {
            model_tree_ui(child, visible);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

//////////////////////////////////////////////////////////////////////

void step_viewer::file_open()
{
    auto path = load_file_dialog();
    if(path.has_value()) {
        open_file(path.value());
    }
}

//////////////////////////////////////////////////////////////////////

std::expected<std::filesystem::path, std::error_code> step_viewer::load_file_dialog()
{
    nfdu8char_t *path;
    nfdu8filteritem_t const filters[] = { { "All supported files", supported_file_spec().c_str() } };
    nfdresult_t result = NFD_OpenDialogU8(&path, filters, (nfdfiltersize_t)std::size(filters), nullptr);
    switch(result) {
    case NFD_OKAY: {
        std::filesystem::path p{ reinterpret_cast<char8_t const *>(path) };
        NFD_FreePathU8(path);
        return { p };
    }
    case NFD_CANCEL:
        LOG_DEBUG("Cancelled");
        return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    default:
        LOG_ERROR("Error: {}", NFD_GetError());
        break;
    }
    return std::unexpected(std::make_error_code(std::errc::io_error));
}

//////////////////////////////////////////////////////////////////////

void step_viewer::open_settings()
{
    settings_snapshot = settings;
    settings_open = true;
    ImGui::SetWindowFocus("Settings");
}

//////////////////////////////////////////////////////////////////////

void step_viewer::revert_settings()
{
    settings_t current = settings;
    settings = settings_snapshot;
    settings.copy_non_dialog_state(current);
}

//////////////////////////////////////////////////////////////////////

void step_viewer::default_settings()
{
    settings_t defaults;
    defaults.copy_non_dialog_state(settings);
    settings = defaults;
}

//////////////////////////////////////////////////////////////////////

void step_viewer::shading_control(char const *label, char const *cad, char const *realistic)
{
    if(model != nullptr && model->has_pbr_materials) {
        bool always = true;
        ImGui::BeginDisabled();
        SegmentedControl(label, &always, cad, realistic);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("This model has its own materials, it's always shaded realistically");
    } else {
        SegmentedControl(label, &settings.realistic_shading, cad, realistic);
        ImGui::SetItemTooltip("CAD (flat colors, headlight) or realistic (materials, studio lighting) shading");
    }
}

//////////////////////////////////////////////////////////////////////
// Changes take effect immediately (everything reads the settings every frame),
// so there's no Apply, just Revert (to how they were when the dialog opened),
// Defaults and Close. Settings are saved when it closes.

void step_viewer::settings_ui()
{
    if(!settings_open) {
        return;
    }

    bool still_open = true;

    // first time, in the middle of the 3D view (after that imgui.ini remembers where it was)
    ImGuiViewport const *main_viewport = ImGui::GetMainViewport();
    ImVec2 view_center(main_viewport->Pos.x + viewport_xpos + viewport_width * 0.5f, main_viewport->Pos.y + viewport_ypos + viewport_height * 0.5f);
    ImGui::SetNextWindowPos(view_center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 26, 0), ImVec2(FLT_MAX, FLT_MAX));
    if(ImGui::Begin("Settings", &still_open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {

        float const slider_width = ImGui::GetFontSize() * 12;
        ImGui::PushItemWidth(slider_width);

        ImGui::SeparatorText("Appearance");
        ImGui::ColorEdit3("Background", (float *)settings.background_color, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs);
        ImGui::Checkbox("Edges", &settings.show_edges);
        shading_control("Shading", "CAD", "Realistic");
        ImGui::SliderFloat("Exposure", &settings.exposure, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Brightness of realistic shading");
        SegmentedControl("Transparency", &settings.transparency, { "None", "Basic", "Advanced" });
        ImGui::SetItemTooltip("How transparent surfaces are put in order\n\n"
                              "None: whole parts, furthest first (fastest, often wrong where parts overlap)\n"
                              "Basic: every triangle, furthest first (mostly right)\n"
                              "Advanced: depth peeling, exactly right up to the number of layers (slowest)");
        if(settings.transparency == model_renderer::transparency_peeled) {
            if(renderer.peel_supported) {
                ImGui::SliderInt("Layers", &settings.transparency_layers, 2, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetItemTooltip("How many transparent surfaces deep it goes (more is slower)");
            } else {
                ImGui::TextDisabled("Not available on this GPU, using Basic");
            }
        }
        ImGui::Checkbox("Toolbar", &settings.view_toolbar);
        ImGui::Checkbox("Tree", &settings.view_tree);
        ImGui::Checkbox("Info", &settings.view_info);

        ImGui::SeparatorText("View");
        SegmentedControl("Projection", &settings.orthographic, "Perspective", "Orthographic");
        SegmentedControl("Rotation", &settings.trackball, "Turntable", "Trackball");
        ImGui::SetItemTooltip("Turntable keeps Z pointing up, trackball rotates freely in any direction");
        ImGui::Checkbox("Axes", &settings.show_axes);
        ImGui::SetItemTooltip("X, Y and Z axes through the origin (red, green, blue)");

        ImGui::SeparatorText("Grid");
        ImGui::Checkbox("Show grid", &settings.show_grid);
        ImGui::SetItemTooltip("Grid on the XY plane");
        {
            // the slider snaps to 1, 2, 5, 10, 20, 50... the box takes anything
            static float const nice_spacings[] = { 0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1.0f,  2.0f,   5.0f,   10.0f,
                                                   20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f };
            int const num_spacings = (int)std::size(nice_spacings);

            // nearest (in log terms) to the current value
            int index = 0;
            for(int i = 1; i < num_spacings; ++i) {
                if(std::abs(std::log(nice_spacings[i] / settings.grid_spacing)) < std::abs(std::log(nice_spacings[index] / settings.grid_spacing))) {
                    index = i;
                }
            }

            float const box_width = ImGui::GetFontSize() * 4;
            ImGui::SetNextItemWidth(slider_width - box_width - ImGui::GetStyle().ItemInnerSpacing.x);
            std::string label = std::format("{:g}", nice_spacings[index]);
            if(ImGui::SliderInt("##spacing_slider", &index, 0, num_spacings - 1, label.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
                settings.grid_spacing = nice_spacings[index];
            }
            ImGui::SetItemTooltip("Distance between grid lines (every 10th line is stronger)");
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::SetNextItemWidth(box_width);
            if(ImGui::InputFloat("Spacing", &settings.grid_spacing, 0, 0, "%g")) {
                settings.grid_spacing = std::clamp(settings.grid_spacing, 0.001f, 100000.0f);
            }
            ImGui::SetItemTooltip("Any spacing (e.g. 2.54 for 0.1\")");
        }
        ImGui::ColorEdit4("Grid color", (float *)settings.grid_color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);

        ImGui::SeparatorText("Selection");
        ImGui::ColorEdit3("Tint color", (float *)settings.selection_color, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs);
        ImGui::SliderFloat("Tint strength", &settings.selection_color.a, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("How much of the tint color is mixed into selected parts");

        ImGui::SeparatorText("Fit");
        float border_percent = settings.fit_border * 100.0f;
        if(ImGui::SliderFloat("Border", &border_percent, 0.0f, 30.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            settings.fit_border = border_percent / 100.0f;
        }
        ImGui::SetItemTooltip("Space left around the model (or selection) on each side");
        ImGui::SliderFloat("Animation", &settings.fit_duration, 0.0f, 2.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("How long Fit takes to get there (0 = instant)");

        ImGui::SeparatorText("Zoom");
        float step_percent = settings.zoom_step * 100.0f;
        if(ImGui::SliderFloat("Step", &step_percent, 2.0f, 50.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            settings.zoom_step = step_percent / 100.0f;
        }
        ImGui::SetItemTooltip("How far each wheel click moves, as a fraction of the distance to the model");
        float floor_percent = settings.zoom_floor * 100.0f;
        if(ImGui::SliderFloat("Close up speed", &floor_percent, 2.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            settings.zoom_floor = floor_percent / 100.0f;
        }
        ImGui::SetItemTooltip("Close to (or inside) the model, zoom speed is based on this fraction\nof the size of the selection (or the model if nothing's selected)");
        ImGui::SliderFloat("Smoothing", &settings.zoom_smooth_time, 0.0f, 0.5f, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("How long each wheel click glides for (0 = instant)");

        ImGui::PopItemWidth();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        RightAlignButtons({ "Revert", "Defaults", "Close" });

        ImGui::BeginDisabled(settings.same_dialog_settings(settings_snapshot));
        if(ImGui::Button("Revert")) {
            revert_settings();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Go back to the settings from when this was opened");
        ImGui::SameLine();

        if(ImGui::Button("Defaults")) {
            default_settings();
        }
        ImGui::SameLine();

        if(ImGui::Button("Close")) {
            still_open = false;
        }
    }
    ImGui::End();

    // closed with the button or the window's X
    if(!still_open) {
        settings_open = false;
        save_settings(settings_path());
    }
}

//////////////////////////////////////////////////////////////////////

std::string step_viewer::loading_text() const
{
    std::u8string name = loading_path.filename().u8string();
    return "Loading " + std::string(name.begin(), name.end());
}

//////////////////////////////////////////////////////////////////////
// in the middle of the 3D view, only when there's nowhere else to show it

void step_viewer::loading_window_ui()
{
    if(!loading || settings.view_info || settings.view_toolbar) {
        return;
    }
    ImGuiViewport const *main_viewport = ImGui::GetMainViewport();
    ImVec2 view_center(main_viewport->Pos.x + viewport_xpos + viewport_width * 0.5f, main_viewport->Pos.y + viewport_ypos + viewport_height * 0.5f);
    ImGui::SetNextWindowPos(view_center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if(ImGui::Begin("Loading", nullptr, flags)) {
        ImGui::TextUnformatted(loading_text().c_str());
        ImGui::ProgressBar(load_progress, ImVec2(ImGui::GetFontSize() * 16, 0));
    }
    ImGui::End();
}

//////////////////////////////////////////////////////////////////////

void step_viewer::ui()
{
    auto is_active = [] {
        ImGuiIO &io = ImGui::GetIO();
        if(io.Ctx->DimBgRatio != 0.0f && io.Ctx->DimBgRatio != 1.0f) {
            return true;
        }
        if(io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
            return true;
        }
        for(int i = 0; i < 5; ++i) {
            if(io.MouseClicked[i] || io.MouseReleased[i]) {
                return true;
            }
        }
        return false;
    };

    if(is_active()) {
        set_active();
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if(ImGui::BeginMainMenuBar()) {
        if(ImGui::BeginMenu("File")) {
            if(ImGui::MenuItem("Open", "Ctrl-O", nullptr)) {
                file_open();
            }
            if(ImGui::BeginMenu("Open Recent", !settings.recent_files.empty())) {
                std::string to_open;
                for(size_t i = 0; i < settings.recent_files.size(); ++i) {
                    std::string const &name = settings.recent_files[i];
                    std::filesystem::path path(std::u8string(name.begin(), name.end()));
                    std::u8string filename = path.filename().u8string();
                    ImGui::PushID((int)i);
                    if(ImGui::MenuItem(std::string(filename.begin(), filename.end()).c_str())) {
                        to_open = name;
                    }
                    ImGui::SetItemTooltip("%s", name.c_str());
                    ImGui::PopID();
                }
                ImGui::Separator();
                if(ImGui::MenuItem("Clear Recent")) {
                    settings.recent_files.clear();
                    save_settings(settings_path());
                }
                ImGui::EndMenu();
                if(!to_open.empty()) {
                    open_file(std::filesystem::path(std::u8string(to_open.begin(), to_open.end())));
                }
            }
            if(ImGui::MenuItem("Close", nullptr, nullptr, model != nullptr)) {
                select_node(-1, false);
                isolated = false;
                model.reset();
                renderer.clear_model();
            }
            ImGui::Separator();
            if(ImGui::MenuItem("Exit", "Esc", nullptr)) {
                set_should_close();
            }
            ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("View")) {
            if(ImGui::MenuItem("Fit to window", "F", nullptr, model != nullptr)) {
                fit_to_view();
            }
            if(ImGui::MenuItem("Reset view", "Space", nullptr, model != nullptr)) {
                reset_view();
            }
            if(ImGui::MenuItem(isolated ? "Unisolate" : "Isolate", "I", nullptr, model != nullptr && (isolated || selected_node >= 0))) {
                toggle_isolate();
            }
            ImGui::MenuItem("Edges", "E", &settings.show_edges);
            ImGui::MenuItem("Grid", "G", &settings.show_grid);
            ImGui::MenuItem("Axes", "X", &settings.show_axes);
            ImGui::Separator();
            ImGui::MenuItem("Toolbar", "", &settings.view_toolbar);
            ImGui::MenuItem("Tree", "", &settings.view_tree);
            ImGui::MenuItem("Info", "", &settings.view_info);
            if(ImGui::MenuItem("Settings...", nullptr, nullptr)) {
                open_settings();
            }

            // Background color
            ImVec2 pos = ImGui::GetCursorScreenPos();
            if(ImGui::Selectable("Background", false, ImGuiSelectableFlags_DontClosePopups)) {
                ImGui::OpenPopup("BackgroundColorPickerPopup");
            }
            float boxSize = ImGui::GetFontSize();
            float posX = pos.x + ImGui::GetItemRectSize().x - boxSize - ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorScreenPos(ImVec2(posX, pos.y));
            ImGui::ColorButton("##bgcolor_btn",
                               { settings.background_color.r, settings.background_color.g, settings.background_color.b, 1.0f },
                               ImGuiColorEditFlags_NoAlpha,
                               ImVec2(boxSize, boxSize));
            if(ImGui::BeginPopup("BackgroundColorPickerPopup")) {
                ImGui::ColorPicker3("##bgcolor_picker", (float *)settings.background_color, ImGuiColorEditFlags_NoAlpha);
                ImGui::EndPopup();
            }
            ImGui::EndMenu();
        }

#if defined(_DEBUG)
        std::string text = std::format("Frame {:07d} {:06.2f}ms {:06.2f}ms", frames, last_frame_elapsed_time * 1000.0, last_frame_cpu_time * 1000.0);
        float text_width = ImGui::CalcTextSize(text.c_str()).x;
        float posX = ImGui::GetWindowWidth() - text_width - ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(posX);
        ImGui::Text("%s", text.c_str());
#endif
        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar(1);

    if(settings.view_toolbar) {
        // a bar across the top of the window under the menu (like the menu bar), not a dockable window
        float toolbar_height = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
        ImGuiWindowFlags toolbar_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;
        ImGui::BeginViewportSideBar("##Toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, toolbar_height, toolbar_flags);
        {
            if(ImGui::Button("Open " MATSYM_file_open)) {
                file_open();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(model == nullptr);
            if(ImGui::Button("Fit " MATSYM_fit_screen)) {
                fit_to_view();
            }
            ImGui::SetItemTooltip("Fit the selection (or everything) in the view (F)");
            for(int axis = 0; axis < 3; ++axis) {
                char const *names[] = { "X", "Y", "Z" };
                ImGui::SameLine();
                if(ImGui::Button(names[axis])) {
                    axis_view(axis);
                }
                ImGui::SetItemTooltip("Look along %s at the selection (or everything)\nclick again for the other side", names[axis]);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(model == nullptr || (!isolated && selected_node < 0));
            bool isolate_checked = isolated;
            if(ImGui::Checkbox("Isolate", &isolate_checked)) {
                toggle_isolate();
            }
            ImGui::SetItemTooltip("Show only the selection (I)");
            ImGui::EndDisabled();
            ImGui::SameLine();
            bool anything_hidden = false;
            if(model != nullptr) {
                for(step_node const &node : model->nodes) {
                    if(!node.visible) {
                        anything_hidden = true;
                        break;
                    }
                }
            }
            ImGui::BeginDisabled(!anything_hidden);
            if(ImGui::Button("Show all " MATSYM_visibility)) {
                model->show_all();
                isolated = false;
                set_active();
            }
            ImGui::SetItemTooltip("Make everything visible again");
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Checkbox("Edges", &settings.show_edges);
            ImGui::SetItemTooltip("Show edges (E)");
            ImGui::SameLine();
            ImGui::Checkbox("Grid", &settings.show_grid);
            ImGui::SetItemTooltip("Show a grid on the XY plane (G)");
            ImGui::SameLine();
            ImGui::Checkbox("Axes", &settings.show_axes);
            ImGui::SetItemTooltip("Show the X, Y and Z axes (X)");
            ImGui::SameLine();
            shading_control("##shading", "CAD", "Realistic");
            ImGui::SameLine();
            SegmentedControl("##projection", &settings.orthographic, "Persp", "Ortho");
            ImGui::SetItemTooltip("Perspective or orthographic projection");
            ImGui::SameLine();
            SegmentedControl("##rotation", &settings.trackball, "Turntable", "Free");
            ImGui::SetItemTooltip("Turntable (Z stays up) or free (trackball) rotation");

            if(loading && !settings.view_info) {
                std::string text = loading_text();
                float const bar_width = ImGui::GetFontSize() * 10;
                float spacing = ImGui::GetStyle().ItemSpacing.x;
                float width = ImGui::CalcTextSize(text.c_str()).x + spacing + bar_width;
                ImGui::SameLine();
                float available = ImGui::GetContentRegionAvail().x;
                if(available > width) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - width);
                }
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(text.c_str());
                ImGui::SameLine();
                ImGui::ProgressBar(load_progress, ImVec2(bar_width, 0));
            }
        }
        ImGui::End();
    }

    // its own window (3) when there's no Info window or toolbar
    loading_window_ui();

    // the tree (it's called "Model" so the saved layout still knows where it goes)
    if(settings.view_tree) {
        if(ImGui::Begin("Model", &settings.view_tree)) {
            if(model == nullptr) {
                ImGui::TextDisabled("No model loaded");
            } else {
                for(int root : model->roots) {
                    model_tree_ui(root, true);
                }
            }
        }
        ImGui::End();
    }
    reveal_selection = false;

    if(settings.view_info) {
        if(ImGui::Begin("Info", &settings.view_info)) {
            if(loading) {
                ImGui::TextWrapped("%s", loading_text().c_str());
                ImGui::ProgressBar(load_progress);
                ImGui::Separator();
            }
            if(model == nullptr) {
                if(!loading) {
                    ImGui::Text("Open a model...");
                }
            } else {
                std::u8string filename = model->path.filename().u8string();
                ImGui::TextWrapped("%s", std::string(filename.begin(), filename.end()).c_str());
                std::u8string full_path = model->path.u8string();
                ImGui::SetItemTooltip("%s", std::string(full_path.begin(), full_path.end()).c_str());
                ImGui::Separator();
                if(ImGui::BeginTable("##model_info", 2, ImGuiTableFlags_SizingStretchProp)) {
                    auto row = [](char const *label, std::string const &value) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(label);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(value.c_str());
                    };
                    row("Solids", std::format("{}", model->num_solids));
                    row("Faces", std::format("{}", model->num_faces));
                    row("Triangles", std::format("{}", model->num_triangles));
                    row("Size",
                        std::format("{:.3f} x {:.3f} x {:.3f}",
                                    model->extent_max[0] - model->extent_min[0],
                                    model->extent_max[1] - model->extent_min[1],
                                    model->extent_max[2] - model->extent_min[2]));
                    ImGui::EndTable();
                }
                if(selected_node >= 0) {
                    ImGui::Separator();
                    uint32_t triangles = 0;
                    for(int part : selected_parts) {
                        triangles += (model->parts[part].num_indices + model->parts[part].num_transparent_indices) / 3;
                    }
                    ImGui::TextWrapped("Selected: %s", model->nodes[selected_node].name.c_str());
                    ImGui::Text("%zu part(s), %u triangles", selected_parts.size(), triangles);
                }
            }
        }
        ImGui::End();
    }

    settings_ui();

    if(ImGui::BeginPopupModal("Load failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(load_error.c_str());
        ImGui::Separator();
        if(ImGui::Button("OK")) {
            load_error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

//////////////////////////////////////////////////////////////////////

void step_viewer::on_render()
{
    check_loaded();

    handle_shortcuts();

    cam.orthographic = settings.orthographic;
    if(cam.trackball != settings.trackball) {
        cam.trackball = settings.trackball;
        if(!cam.trackball) {
            cam.level();    // turntable keeps Z up
        }
    }

    cam.update(get_time());
    update_zoom(get_time());

    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

    // On first run (no imgui.ini), set up a default docking layout
    if(frames == 0 && !std::filesystem::exists(ImGui::GetIO().IniFilename)) {

        ImGui::DockBuilderRemoveNodeChildNodes(dockspace_id);

        // Right info panel (25% width)
        ImGuiID dock_right_id;
        ImGuiID dock_middle_id;
        ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.25f, &dock_right_id, &dock_middle_id);

        // Left model panel
        ImGuiID dock_left_id;
        ImGuiID dock_center_id;
        ImGui::DockBuilderSplitNode(dock_middle_id, ImGuiDir_Left, 0.25f, &dock_left_id, &dock_center_id);

        ImGui::DockBuilderDockWindow("Info", dock_right_id);
        ImGui::DockBuilderDockWindow("Model", dock_left_id);

        // Hide tab bars on nodes with a single window
        auto hide_tab_bar = [](ImGuiID id) {
            ImGuiDockNode *n = ImGui::DockBuilderGetNode(id);
            if(n) {
                n->SetLocalFlags(n->LocalFlags | ImGuiDockNodeFlags_AutoHideTabBar);
            }
        };
        hide_tab_bar(dock_right_id);
        hide_tab_bar(dock_left_id);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGuiDockNode *central_node = ImGui::DockBuilderGetCentralNode(dockspace_id);

    if(central_node) {
        ImGuiViewport *main_viewport = ImGui::GetMainViewport();
        viewport_xpos = (int)(central_node->Pos.x - main_viewport->Pos.x);
        viewport_ypos = (int)(central_node->Pos.y - main_viewport->Pos.y);
        viewport_width = (int)central_node->Size.x;
        viewport_height = (int)central_node->Size.y;
    } else {
        // this should never happen
        viewport_xpos = 0;
        viewport_ypos = 0;
        viewport_width = window_width;
        viewport_height = window_height;
    }

    if(window_width == 0 || window_height == 0) {
        return;
    }

    double t = get_time();

    gpu_render();
    ui();
    last_frame_cpu_time = get_time() - t;
}

//////////////////////////////////////////////////////////////////////
// Nothing to draw yet, just clear to the background color
// ImGui is drawn over the top in on_gpu_imgui()

void step_viewer::gpu_render()
{
    gpu_cmd = SDL_AcquireGPUCommandBuffer(gpu_dev.gpu);
    if(!gpu_cmd) {
        LOG_ERROR("Failed to acquire command buffer: {}", SDL_GetError());
        return;
    }

    // Acquire swapchain texture (shared with on_gpu_imgui)
    gpu_swapchain_texture = nullptr;
    uint32_t sw_w, sw_h;
    // waits for vsync, which paces the frame rate when the main loop is active
    if(!SDL_WaitAndAcquireGPUSwapchainTexture(gpu_cmd, window, &gpu_swapchain_texture, &sw_w, &sw_h) || !gpu_swapchain_texture) {
        SDL_SubmitGPUCommandBuffer(gpu_cmd);
        gpu_cmd = nullptr;
        gpu_swapchain_texture = nullptr;
        return;
    }

    // viewport is in window coordinates, the swapchain is in pixels
    float scale_x = window_width > 0 ? (float)sw_w / (float)window_width : 1.0f;
    float scale_y = window_height > 0 ? (float)sw_h / (float)window_height : 1.0f;

    model_renderer::viewport_t vp{ viewport_xpos * scale_x, viewport_ypos * scale_y, viewport_width * scale_x, viewport_height * scale_y };

    float aspect = vp.h > 0 ? vp.w / vp.h : 1.0f;

    auto const &bg = settings.background_color;
    float background[3] = { bg.r, bg.g, bg.b };

    // the grid fades out at this distance from the camera target, orthographic needs the far plane beyond it
    float grid_radius = std::max(cam.scene_radius * 10.0f, cam.distance * 25.0f);
    cam.far_extent = ((settings.show_grid || settings.show_axes) && model != nullptr) ? grid_radius * 2.0f : 0.0f;

    model_renderer::draw_params params{};
    params.viewport = vp;
    std::copy(background, background + 3, params.background);
    params.view = cam.view_matrix();
    params.projection = cam.projection_matrix(aspect);
    params.eye = cam.eye();
    params.show_edges = settings.show_edges;
    params.realistic = model != nullptr && (model->has_pbr_materials || settings.realistic_shading);
    params.exposure = settings.exposure;
    params.transparency = settings.transparency;
    params.peel_layers = settings.transparency_layers;
    std::copy((float const *)settings.selection_color, (float const *)settings.selection_color + 4, params.selection_tint);
    params.parts = model != nullptr ? &model->parts : nullptr;
    params.selected_parts = &selected_parts;

    // grid on the file's XY plane (vertices are relative to the model center), a big square
    // under the camera target which fades out well before its edges
    params.show_grid = settings.show_grid && model != nullptr;
    params.show_axes = settings.show_axes && model != nullptr;
    if(params.show_grid) {
        double spacing = std::max((double)settings.grid_spacing, 1e-6);
        float plane_z = (float)-model->center[2];
        float grid_params[4][4] = {
            { cam.target.x, cam.target.y, plane_z, grid_radius },
            { settings.grid_color.r, settings.grid_color.g, settings.grid_color.b, settings.grid_color.a },
            // offset so the lines land on multiples of the spacing in file coordinates (mod 10 so it stays small)
            { (float)spacing, (float)std::fmod(model->center[0], spacing * 10), (float)std::fmod(model->center[1], spacing * 10), 0 },
            { cam.target.x, cam.target.y, grid_radius, 0 },
        };
        std::copy(grid_params[0], grid_params[0] + 4, params.grid_plane);
        std::copy(grid_params[1], grid_params[1] + 4, params.grid_color);
        std::copy(grid_params[2], grid_params[2] + 4, params.grid_lines);
        std::copy(grid_params[3], grid_params[3] + 4, params.grid_fade);
    }

    renderer.render(gpu_cmd, gpu_swapchain_texture, sw_w, sw_h, params);
}

//////////////////////////////////////////////////////////////////////

void step_viewer::on_gpu_imgui()
{
    if(!gpu_cmd) {
        // gpu_render() failed to acquire — nothing to do
        return;
    }

    ImDrawData *draw_data = ImGui::GetDrawData();
    if(draw_data) {
        // Must call PrepareDrawData BEFORE the render pass to upload vertex/index buffers
        ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, gpu_cmd);

        if(gpu_swapchain_texture) {
            SDL_GPUColorTargetInfo ct{};
            ct.texture = gpu_swapchain_texture;
            ct.load_op = SDL_GPU_LOADOP_LOAD;
            ct.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(gpu_cmd, &ct, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(draw_data, gpu_cmd, pass);
            SDL_EndGPURenderPass(pass);
        }
    }

    SDL_SubmitGPUCommandBuffer(gpu_cmd);
    gpu_cmd = nullptr;
    gpu_swapchain_texture = nullptr;
}
