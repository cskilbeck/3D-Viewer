//////////////////////////////////////////////////////////////////////
// Orbit camera, looking at a target point
//
// Orientation is a forward + up pair so it can rotate freely (trackball)
// as well as the usual turntable around Z (STEP models are usually Z up)
//
// distance is how far the eye is from the target. In orthographic mode it
// sets the scale instead (the view is the same size as a perspective view
// at the target's depth) and the eye is pulled back out of the scene

#pragma once

#include "gpu_math.h"

struct camera
{
    gpu::vec3 target{};
    float distance{ 1 };

    // unit vectors, always at right angles
    gpu::vec3 forward{ 0, 1, 0 };    // from the eye towards the target
    gpu::vec3 up{ 0, 0, 1 };

    float fov_y{ 0.5235988f };    // 30 degrees

    bool orthographic{ false };
    bool trackball{ false };    // free rotation rather than turntable

    // radius of a sphere (centered on the origin) which encloses everything, for near/far planes
    float scene_radius{ 1 };

    // how far beyond the scene things (grid, axes) need to be visible (orthographic, perspective goes to infinity)
    float far_extent{ 0 };

    // fit animation
    bool animating{ false };
    double anim_start_time{};
    double anim_duration{};
    gpu::vec3 anim_from_target{};
    gpu::vec3 anim_to_target{};
    float anim_from_distance{};
    float anim_to_distance{};

    struct basis_t
    {
        gpu::vec3 forward;    // from the eye towards the target
        gpu::vec3 right;
        gpu::vec3 up;
    };

    // look from the front (-Y) right (+X) and above
    void set_isometric();

    // look in a direction with up roughly up_hint (turntable can't look straight up/down
    // so it stops just short, turned so up_hint is still up on the screen)
    void look_along(gpu::vec3 const &direction, gpu::vec3 const &up_hint);

    // take out any roll so Z points up (for turntable mode)
    void level();
    bool is_level() const;

    // look at the middle of a box from the current direction, close enough that it
    // fills the view apart from a border (fraction of the viewport size on each side)
    void fit_box(gpu::vec3 const &box_min, gpu::vec3 const &box_max, float aspect, float border);

    // same but where it would end up rather than going there
    void fit_box_target(gpu::vec3 const &box_min, gpu::vec3 const &box_max, float aspect, float border, gpu::vec3 &new_target, float &new_distance) const;

    // move the target/distance smoothly to new values over duration seconds
    void animate_to(gpu::vec3 const &new_target, float new_distance, double now, double duration);

    // advance the animation, returns true while it's still going
    bool update(double now);

    void stop_animation()
    {
        animating = false;
    }

    bool is_animating() const
    {
        return animating;
    }

    // mouse interaction, dx,dy in pixels
    // orbit rotates around pivot if there is one, otherwise around the target
    void orbit(float dx, float dy, gpu::vec3 const *pivot);

    // depth is how far in front of the eye the dragged point is, so it moves exactly with the mouse
    // (orthographic doesn't need it)
    void pan(float dx, float dy, float viewport_height, float depth);

    // perspective zoom: move the camera (eye and target together, so the view direction doesn't change)
    // moving away is limited so the scene is never smaller than ~10 pixels (needs the viewport height)
    void move(gpu::vec3 const &delta, float viewport_height);

    // orthographic zoom: scale the view around a point (which stays on the same pixel)
    void scale_view(float wheel_clicks, gpu::vec3 const &point, float viewport_height);

    // ray through a point on the screen, ndc_x,ndc_y are -1..1 (y up)
    void ray(float ndc_x, float ndc_y, float aspect, gpu::vec3 &origin, gpu::vec3 &direction) const;

    // half the height of the view at the target's depth
    float half_height() const;

    basis_t basis() const;
    gpu::vec3 eye() const;
    gpu::mat4 view_matrix() const;

    // reversed Z (near = 1, far = 0), infinitely far away for perspective
    gpu::mat4 projection_matrix(float aspect) const;
};
