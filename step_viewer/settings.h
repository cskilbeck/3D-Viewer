#pragma once

#include <filesystem>
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
    X(bool, view_toolbar, true)                                    \
    X(bool, show_edges, true)                                      \
    X(settings::color_t, selection_color, { 1.0f, 0.85f, 0.0f, 0.6f }) \
    X(float, fit_border, 0.08f)                                    \
    X(float, fit_duration, 0.4f)                                   \
    X(float, zoom_step, 0.15f)                                     \
    X(float, zoom_floor, 0.3f)                                     \
    X(float, zoom_smooth_time, 0.1f)

struct settings_t
{
#define X(type, name, ...) type name = __VA_ARGS__;
    SETTINGS_FIELDS
#undef X

    void save(std::filesystem::path const &path);
    bool load(std::filesystem::path const &path);

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
