//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cmath>

#include "camera.h"

namespace
{
    float constexpr pi = 3.14159265f;

    // radians per pixel of mouse movement
    float constexpr orbit_speed = 0.008f;

    // keep away from straight up/down where the turntable flips over
    float constexpr max_pitch = pi * 0.5f - 0.001f;


    // zooming out stops when the scene is this many pixels across
    float constexpr min_scene_pixels = 10.0f;

    gpu::vec3 const world_up{ 0, 0, 1 };

}    // namespace

//////////////////////////////////////////////////////////////////////

void camera::set_isometric()
{
    yaw = -pi * 0.25f;
    pitch = std::atan(1.0f / std::sqrt(2.0f));
}

//////////////////////////////////////////////////////////////////////
// A corner at view depth (distance + z) is inside the (shrunk) frustum if
// |x| <= (distance + z) * tan(half fov) so the distance has to be at least
// |x| / tan(half fov) - z for every corner, horizontally and vertically

void camera::fit_box(gpu::vec3 const &box_min, gpu::vec3 const &box_max, float aspect, float border)
{
    animating = false;
    fit_box_target(box_min, box_max, aspect, border, target, distance);
}

//////////////////////////////////////////////////////////////////////

void camera::fit_box_target(gpu::vec3 const &box_min, gpu::vec3 const &box_max, float aspect, float border, gpu::vec3 &new_target, float &new_distance) const
{
    new_target = (box_min + box_max) * 0.5f;

    basis_t b = basis();

    float scale = std::max(1.0f - 2.0f * border, 0.05f);
    float tan_y = std::tan(fov_y * 0.5f) * scale;
    float tan_x = tan_y * aspect;

    float d = 0;
    for(int i = 0; i < 8; ++i) {
        gpu::vec3 corner{ (i & 1) ? box_max.x : box_min.x, (i & 2) ? box_max.y : box_min.y, (i & 4) ? box_max.z : box_min.z };
        gpu::vec3 c = corner - new_target;
        float x = std::abs(gpu::dot(c, b.right));
        float y = std::abs(gpu::dot(c, b.up));
        float z = gpu::dot(c, b.forward);
        d = std::max({ d, x / tan_x - z, y / tan_y - z });
    }

    new_distance = std::max(d, scene_radius * 1e-4f);
}

//////////////////////////////////////////////////////////////////////

void camera::animate_to(gpu::vec3 const &new_target, float new_distance, double now, double duration)
{
    if(duration <= 0) {
        target = new_target;
        distance = new_distance;
        animating = false;
        return;
    }
    anim_from_target = target;
    anim_to_target = new_target;
    anim_from_distance = distance;
    anim_to_distance = new_distance;
    anim_start_time = now;
    anim_duration = duration;
    animating = true;
}

//////////////////////////////////////////////////////////////////////
// Ease in-out (cubic). Distance is interpolated logarithmically so
// zooming a long way in or out moves at a steady rate

bool camera::update(double now)
{
    if(!animating) {
        return false;
    }

    float t = (float)std::clamp((now - anim_start_time) / anim_duration, 0.0, 1.0);
    float eased = t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;

    target = anim_from_target + (anim_to_target - anim_from_target) * eased;
    distance = std::exp(std::log(anim_from_distance) + (std::log(anim_to_distance) - std::log(anim_from_distance)) * eased);

    if(t >= 1.0f) {
        target = anim_to_target;
        distance = anim_to_distance;
        animating = false;
    }
    return animating;
}

//////////////////////////////////////////////////////////////////////
// Turning the camera also swings it around the pivot: express the eye
// relative to the pivot in the old camera basis, rebuild it in the new one

void camera::orbit(float dx, float dy, gpu::vec3 const *pivot)
{
    gpu::vec3 old_eye = eye();
    basis_t old_basis = basis();

    yaw -= dx * orbit_speed;
    pitch = std::clamp(pitch + dy * orbit_speed, -max_pitch, max_pitch);

    if(pivot == nullptr) {
        return;
    }

    basis_t new_basis = basis();
    gpu::vec3 rel = old_eye - *pivot;
    gpu::vec3 new_eye = *pivot + new_basis.right * gpu::dot(rel, old_basis.right) + new_basis.up * gpu::dot(rel, old_basis.up) +
                        new_basis.forward * gpu::dot(rel, old_basis.forward);
    target = new_eye + new_basis.forward * distance;
}

//////////////////////////////////////////////////////////////////////
// move the camera sideways so a point at depth stays under the mouse

void camera::pan(float dx, float dy, float viewport_height, float depth)
{
    float world_per_pixel = 2.0f * depth * std::tan(fov_y * 0.5f) / std::max(viewport_height, 1.0f);
    basis_t b = basis();
    target = target - b.right * (dx * world_per_pixel) + b.up * (dy * world_per_pixel);
}

//////////////////////////////////////////////////////////////////////

// Moving the eye along the line to the point keeps the point on the same
// pixel - scale the eye's (and target's) offset from the point

void camera::move(gpu::vec3 const &delta, float viewport_height)
{
    // the scene's bounding sphere (centered on the origin) shouldn't get smaller than
    // min_scene_pixels, which happens at this distance from the origin
    float max_eye_distance = scene_radius * std::max(viewport_height, 1.0f) / (min_scene_pixels * std::tan(fov_y * 0.5f));

    gpu::vec3 old_eye = eye();
    gpu::vec3 new_eye = old_eye + delta;
    float t = 1.0f;
    if(new_eye.length() > max_eye_distance && new_eye.length() > old_eye.length()) {
        // solve |old_eye + delta * t| = max for t in 0..1
        float a = gpu::dot(delta, delta);
        float b = 2.0f * gpu::dot(old_eye, delta);
        float c = gpu::dot(old_eye, old_eye) - max_eye_distance * max_eye_distance;
        float discriminant = b * b - 4 * a * c;
        t = (a > 0 && discriminant >= 0) ? std::clamp((-b + std::sqrt(discriminant)) / (2 * a), 0.0f, 1.0f) : 0.0f;
    }
    target = target + delta * t;
}

//////////////////////////////////////////////////////////////////////

void camera::ray(float ndc_x, float ndc_y, float aspect, gpu::vec3 &origin, gpu::vec3 &direction) const
{
    origin = eye();
    basis_t b = basis();
    float t = std::tan(fov_y * 0.5f);
    direction = gpu::normalize(b.forward + b.right * (ndc_x * t * aspect) + b.up * (ndc_y * t));
}

//////////////////////////////////////////////////////////////////////

camera::basis_t camera::basis() const
{
    basis_t b;
    b.forward = { -std::cos(pitch) * std::cos(yaw), -std::cos(pitch) * std::sin(yaw), -std::sin(pitch) };
    b.right = gpu::normalize(gpu::cross(b.forward, world_up));
    b.up = gpu::cross(b.right, b.forward);
    return b;
}

//////////////////////////////////////////////////////////////////////

gpu::vec3 camera::eye() const
{
    return target - basis().forward * distance;
}

//////////////////////////////////////////////////////////////////////

gpu::mat4 camera::view_matrix() const
{
    return gpu::mat4::look_at(eye(), target, world_up);
}

//////////////////////////////////////////////////////////////////////
// near/far planes hug the scene's bounding sphere (centered on the origin)

gpu::mat4 camera::projection_matrix(float aspect) const
{
    float eye_distance = eye().length();
    float far_z = eye_distance + scene_radius * 1.1f;
    float near_z = std::max(eye_distance - scene_radius * 1.1f, far_z * 1e-5f);
    return gpu::mat4::perspective(fov_y, aspect, near_z, far_z);
}
