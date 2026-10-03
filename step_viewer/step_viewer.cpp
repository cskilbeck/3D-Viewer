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
    // everything OCCT can import
    nfdu8filteritem_t const step_file_filters[] = {
        { "All supported files", "step,stp,stpz,iges,igs,stl,obj,gltf,glb,wrl,vrml,brep,xbf" },
        { "STEP", "step,stp,stpz" },
        { "IGES", "iges,igs" },
        { "STL", "stl" },
        { "OBJ", "obj" },
        { "glTF", "gltf,glb" },
        { "VRML", "wrl,vrml" },
        { "OpenCascade BREP", "brep" },
        { "OpenCascade XCAF", "xbf" },
    };

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
    if(cam.is_animating() || pending_zoom.length() != 0) {
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
// Use up some of the pending zoom each frame (exponential ease out)

void step_viewer::update_zoom(double now)
{
    if(pending_zoom.length() == 0) {
        return;
    }
    float dt = (float)(now - pending_zoom_time);
    pending_zoom_time = now;

    float time_constant = std::max(settings.zoom_smooth_time, 0.001f) / 3.0f;    // ~95% done after zoom_smooth_time
    gpu::vec3 step = pending_zoom * (1.0f - std::exp(-dt / time_constant));
    if((pending_zoom - step).length() < cam.scene_radius * 1e-5f) {
        step = pending_zoom;
    }
    cam.move(step, (float)viewport_height);
    pending_zoom = pending_zoom - step;
    if(pending_zoom.length() < cam.scene_radius * 1e-5f) {
        pending_zoom = {};
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
    gpu::vec3 grabbed = point_under_mouse(mouse_x, mouse_y);
    pan_depth = std::max(gpu::dot(grabbed - cam.eye(), cam.basis().forward), cam.distance * 1e-3f);
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
        float clicks = (dx - dy) / pixels_per_click;
        cam.move(mouse_ray(zoom_x, zoom_y) * (clicks * zoom_click_distance(cam.eye())), (float)viewport_height);
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
        // along the ray through the mouse so whatever's under it stays under it
        gpu::vec3 delta = mouse_ray(mouse_x, mouse_y) * ((float)yoffset * zoom_click_distance(cam.eye() + pending_zoom));
        if(settings.zoom_smooth_time <= 0) {
            cam.move(delta, (float)viewport_height);
        } else {
            if(pending_zoom.length() == 0) {
                pending_zoom_time = get_time();
            }
            pending_zoom = pending_zoom + delta;
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

    if(action != ACTION_PRESS) {
        return;
    }

    switch(key) {

    case KEY_ESCAPE:
        set_should_close();
        break;

    case KEY_O:
        if((mods & KMOD_CTRL_FLAG) != 0) {
            file_open();
        }
        break;

    case KEY_F:
        fit_to_view();
        break;

    case KEY_E:
        settings.show_edges = !settings.show_edges;
        break;
    }
}

//////////////////////////////////////////////////////////////////////

void step_viewer::on_closed()
{
    if(loader.joinable()) {
        loader.request_stop();
        loader.join();
    }
    model.reset();
    save_settings(config_path(app_name, settings_filename));
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

    load_settings(config_path(app_name, settings_filename));

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
        load_error.clear();
        load_finished = false;
    }

    loading_path = path;
    load_progress = 0;
    loading = true;

    loader = std::jthread([this, path](std::stop_token stop) {
        auto result = load_step_model(path, stop, load_progress);
        {
            std::lock_guard _(loaded_mutex);
            if(result.has_value()) {
                loaded_model = std::move(result.value());
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
        select_node(-1, false);
        renderer.set_model(*model);
        cam.scene_radius = (float)model->radius;
        reset_view();
        model->edges = {};    // vertices/indices are kept for picking
    } else if(load_error != "Cancelled") {
        LOG_ERROR("{}", load_error);
        ImGui::OpenPopup("Load failed");
    }
}

//////////////////////////////////////////////////////////////////////

void step_viewer::model_tree_ui(int node_index)
{
    step_node const &node = model->nodes[node_index];

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if(node.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if(node_index == selected_node) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // selected in the 3D view - open the way down to it
    if(reveal_selection && !node.children.empty() && model->is_ancestor(node_index, selected_node) && node_index != selected_node) {
        ImGui::SetNextItemOpen(true);
    }

    ImGui::PushID(node_index);

    if(node.has_color) {
        ImGui::ColorButton("##color",
                           { node.color[0], node.color[1], node.color[2], 1.0f },
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
        ImGui::SameLine();
    }

    char const *icon = node.is_assembly ? MATSYM_folder : MATSYM_deployed_code;
    bool open = ImGui::TreeNodeEx("##node", flags, "%s %s", icon, node.name.c_str());

    if(ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        select_node(node_index, false);
    }

    if(reveal_selection && node_index == selected_node) {
        ImGui::SetScrollHereY(0.5f);
    }

    if(open && !node.children.empty()) {
        for(int child : node.children) {
            model_tree_ui(child);
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
    nfdresult_t result = NFD_OpenDialogU8(&path, step_file_filters, (nfdfiltersize_t)std::size(step_file_filters), nullptr);
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
            if(ImGui::MenuItem("Close", nullptr, nullptr, model != nullptr)) {
                select_node(-1, false);
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
            if(ImGui::MenuItem("Reset view", nullptr, nullptr, model != nullptr)) {
                reset_view();
            }
            ImGui::MenuItem("Edges", "E", &settings.show_edges);
            ImGui::Separator();
            ImGui::MenuItem("Toolbar", "", &settings.view_toolbar);

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
        ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoDecoration);
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
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Checkbox("Edges", &settings.show_edges);
            ImGui::SetItemTooltip("Show edges (E)");
        }
        ImGui::End();
    }

    ImGui::Begin("Model");
    {
        if(model == nullptr) {
            ImGui::TextDisabled("No model loaded");
        } else {
            for(int root : model->roots) {
                model_tree_ui(root);
            }
            reveal_selection = false;
        }
    }
    ImGui::End();

    ImGui::Begin("Info");
    {
        if(loading) {
            ImGui::TextWrapped("Loading %s", loading_path.filename().string().c_str());
            ImGui::ProgressBar(load_progress);
            ImGui::Separator();
        }
        if(model == nullptr) {
            if(!loading) {
                ImGui::Text("Open a model...");
            }
        } else {
            ImGui::TextWrapped("%s", model->path.string().c_str());
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
                row("Read time", std::format("{:.2f}s", model->read_time));
                row("Mesh time", std::format("{:.2f}s", model->mesh_time));
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

    cam.update(get_time());
    update_zoom(get_time());

    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

    // On first run (no imgui.ini), set up a default docking layout
    if(frames == 0 && !std::filesystem::exists(ImGui::GetIO().IniFilename)) {

        ImGui::DockBuilderRemoveNodeChildNodes(dockspace_id);

        // Top toolbar (full width)
        ImGuiID dock_top_id;
        ImGuiID dock_rest_id;
        ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.045f, &dock_top_id, &dock_rest_id);

        // Right info panel (25% width)
        ImGuiID dock_right_id;
        ImGuiID dock_middle_id;
        ImGui::DockBuilderSplitNode(dock_rest_id, ImGuiDir_Right, 0.25f, &dock_right_id, &dock_middle_id);

        // Left model panel
        ImGuiID dock_left_id;
        ImGuiID dock_center_id;
        ImGui::DockBuilderSplitNode(dock_middle_id, ImGuiDir_Left, 0.25f, &dock_left_id, &dock_center_id);

        ImGui::DockBuilderDockWindow("Toolbar", dock_top_id);
        ImGui::DockBuilderDockWindow("Info", dock_right_id);
        ImGui::DockBuilderDockWindow("Model", dock_left_id);

        // Hide tab bars on nodes with a single window
        auto hide_tab_bar = [](ImGuiID id) {
            ImGuiDockNode *n = ImGui::DockBuilderGetNode(id);
            if(n) {
                n->SetLocalFlags(n->LocalFlags | ImGuiDockNodeFlags_AutoHideTabBar);
            }
        };
        hide_tab_bar(dock_top_id);
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

    model_renderer::draw_params params{};
    params.viewport = vp;
    std::copy(background, background + 3, params.background);
    params.view = cam.view_matrix();
    params.projection = cam.projection_matrix(aspect);
    params.eye = cam.eye();
    params.show_edges = settings.show_edges;
    std::copy((float const *)settings.selection_color, (float const *)settings.selection_color + 4, params.selection_tint);
    params.parts = model != nullptr ? &model->parts : nullptr;
    params.selected_parts = &selected_parts;

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
