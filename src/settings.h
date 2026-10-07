#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "log.h"

namespace settings
{
    struct color_t
    {
        float r{ 0 };
        float g{ 0 };
        float b{ 0 };
        float a{ 1 };
        explicit operator float *()
        {
            return &r;
        }
        explicit operator float const *() const
        {
            return &r;
        }
        bool operator==(color_t const &) const = default;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(color_t, r, g, b, a)
    };
}    // namespace settings

#define SETTINGS_FIELDS                                            \
    X(bool, window_maximized, false)                               \
    X(settings::color_t, background_color, { 0.1f, 0.1f, 0.12f }) \
    X(int, window_width, 1600)                                     \
    X(int, window_height, 900)                                     \
    X(int, window_xpos, 100)                                       \
    X(int, window_ypos, 100)                                       \
    X(bool, reuse_window, true)                                    \
    X(bool, view_toolbar, true)                                    \
    X(bool, view_tree, true)                                       \
    X(bool, view_info, true)                                       \
    X(bool, show_edges, true)                                      \
    X(float, edge_width, 1.5f)                                     \
    X(bool, realistic_shading, false)                              \
    X(float, exposure, 1.0f)                                       \
    X(int, antialiasing, 2)                                        \
    X(int, transparency, 1)                                        \
    X(int, transparency_layers, 8)                                 \
    X(settings::color_t, selection_color, { 1.0f, 0.85f, 0.0f, 0.6f }) \
    X(float, fit_border, 0.08f)                                    \
    X(float, fit_duration, 0.4f)                                   \
    X(float, zoom_step, 0.15f)                                     \
    X(float, zoom_floor, 0.3f)                                     \
    X(float, zoom_smooth_time, 0.1f)                               \
    X(bool, orthographic, false)                                   \
    X(bool, trackball, false)                                      \
    X(bool, show_grid, false)                                      \
    X(float, grid_spacing, 10.0f)                                  \
    X(settings::color_t, grid_color, { 0.5f, 0.5f, 0.5f, 0.6f })  \
    X(bool, show_axes, false)                                      \
    X(std::vector<std::string>, recent_files, {})                  \
    X(std::vector<std::string>, settings_sections_open, {})

struct settings_t
{
#define X(type, name, ...) type name = __VA_ARGS__;
    SETTINGS_FIELDS
#undef X

    void save(std::filesystem::path const &path);
    bool load(std::filesystem::path const &path);

    bool operator==(settings_t const &) const = default;

    // the window position/size, recent files and which sections of the settings dialog are expanded
    // aren't settings the dialog edits, Revert/Defaults keep them
    void copy_non_dialog_state(settings_t const &other)
    {
        window_maximized = other.window_maximized;
        window_width = other.window_width;
        window_height = other.window_height;
        window_xpos = other.window_xpos;
        window_ypos = other.window_ypos;
        recent_files = other.recent_files;
        settings_sections_open = other.settings_sections_open;
    }

    // same as far as the settings dialog is concerned
    bool same_dialog_settings(settings_t const &other) const
    {
        settings_t a = *this;
        a.copy_non_dialog_state(other);
        return a == other;
    }

    void to_json(nlohmann::json &j)
    {
#define X(type, name, ...) j[#name] = name;
        SETTINGS_FIELDS
#undef X
    }

    void from_json(const nlohmann::json &j)
    {
        LOG_CONTEXT("from_json", debug);
#define X(type, name, ...)              \
    if(j.contains(#name)) {             \
        name = j.at(#name).get<type>(); \
    } else {                            \
        name = __VA_ARGS__;             \
    }
        SETTINGS_FIELDS
#undef X
    }
};
