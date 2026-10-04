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

    // orthographic zoom, each wheel click scales the view by this
    float constexpr ortho_zoom_step = 0.85f;

    gpu::vec3 const world_up{ 0, 0, 1 };

    //////////////////////////////////////////////////////////////////////
    // rotate v around a unit axis

    gpu::vec3 rotate(gpu::vec3 const &v, gpu::vec3 const &axis, float angle)
    {
        float c = std::cos(angle);
        float s = std::sin(angle);
        return v * c + gpu::cross(axis, v) * s + axis * (gpu::dot(axis, v) * (1.0f - c));
    }

    //////////////////////////////////////////////////////////////////////
    // forward from turntable angles (yaw around Z, pitch above the XY plane)

    gpu::vec3 forward_from(float yaw, float pitch)
    {
        return { -std::cos(pitch) * std::cos(yaw), -std::cos(pitch) * std::sin(yaw), -std::sin(pitch) };
    }

    //////////////////////////////////////////////////////////////////////
    // an up vector at right angles to forward, as close to Z as possible

    gpu::vec3 level_up(gpu::vec3 const &forward, gpu::vec3 const &fallback)
    {
        gpu::vec3 right = gpu::cross(forward, world_up);
        if(right.length() < 1e-6f) {
            // looking straight up/down, keep whatever up we had
            right = gpu::cross(forward, fallback);
        }
        return gpu::normalize(gpu::cross(gpu::normalize(right), forward));
    }

}    // namespace

//////////////////////////////////////////////////////////////////////

void camera::set_isometric()
{
    forward = forward_from(-pi * 0.25f, std::atan(1.0f / std::sqrt(2.0f)));
    up = level_up(forward, up);
}

//////////////////////////////////////////////////////////////////////

void camera::level()
{
    // turntable can't look straight up/down
    float pitch = std::asin(std::clamp(-forward.z, -1.0f, 1.0f));
    if(std::abs(pitch) > max_pitch) {
        float yaw = std::atan2(-forward.y, -forward.x);
        forward = forward_from(yaw, std::clamp(pitch, -max_pitch, max_pitch));
    }
    up = level_up(forward, up);
}

//////////////////////////////////////////////////////////////////////

bool camera::is_level() const
{
    return gpu::dot(up, level_up(forward, up)) > 0.99999f && std::abs(forward.z) < std::sin(max_pitch);
}

//////////////////////////////////////////////////////////////////////

void camera::fit_box(gpu::vec3 const &box_min, gpu::vec3 const &box_max, float aspect, float border)
{
    animating = false;
    fit_box_target(box_min, box_max, aspect, border, target, distance);
}

//////////////////////////////////////////////////////////////////////
// Perspective: a corner at view depth (distance + z) is inside the (shrunk)
// frustum if |x| <= (distance + z) * tan(half fov), so the distance has to be
// at least |x| / tan(half fov) - z for every corner, horizontally and vertically
// Orthographic: the half height of the view has to be at least |y| and |x| / aspect

void camera::fit_box_target(gpu::vec3 const &box_min, gpu::vec3 const &box_max, float aspect, float border, gpu::vec3 &new_target, float &new_distance) const
{
    new_target = (box_min + box_max) * 0.5f;

    basis_t b = basis();

    float scale = std::max(1.0f - 2.0f * border, 0.05f);
    float tan_half_fov = std::tan(fov_y * 0.5f);
    float tan_y = tan_half_fov * scale;
    float tan_x = tan_y * aspect;

    float d = 0;
    for(int i = 0; i < 8; ++i) {
        gpu::vec3 corner{ (i & 1) ? box_max.x : box_min.x, (i & 2) ? box_max.y : box_min.y, (i & 4) ? box_max.z : box_min.z };
        gpu::vec3 c = corner - new_target;
        float x = std::abs(gpu::dot(c, b.right));
        float y = std::abs(gpu::dot(c, b.up));
        if(orthographic) {
            d = std::max({ d, x / tan_x, y / tan_y });
        } else {
            float z = gpu::dot(c, b.forward);
            d = std::max({ d, x / tan_x - z, y / tan_y - z });
        }
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
// Turntable: yaw around Z, pitch up/down (clamped), Z stays up
// Trackball: turn around the camera's own up and right axes, no limits
// Either way the target swings around the pivot: express it relative to
// the pivot in the old camera basis, rebuild it in the new one

void camera::orbit(float dx, float dy, gpu::vec3 const *pivot)
{
    basis_t old_basis = basis();

    if(trackball) {
        forward = rotate(forward, old_basis.up, -dx * orbit_speed);
        gpu::vec3 right = gpu::normalize(gpu::cross(forward, old_basis.up));
        forward = gpu::normalize(rotate(forward, right, -dy * orbit_speed));
        up = gpu::normalize(gpu::cross(right, forward));
    } else {
        float yaw = std::atan2(-forward.y, -forward.x) - dx * orbit_speed;
        float pitch = std::asin(std::clamp(-forward.z, -1.0f, 1.0f));
        pitch = std::clamp(pitch + dy * orbit_speed, -max_pitch, max_pitch);
        forward = forward_from(yaw, pitch);
        up = level_up(forward, up);
    }

    if(pivot == nullptr) {
        return;
    }

    basis_t new_basis = basis();
    gpu::vec3 rel = target - *pivot;
    target = *pivot + new_basis.right * gpu::dot(rel, old_basis.right) + new_basis.up * gpu::dot(rel, old_basis.up) +
             new_basis.forward * gpu::dot(rel, old_basis.forward);
}

//////////////////////////////////////////////////////////////////////
// move the camera sideways so a point at depth stays under the mouse

void camera::pan(float dx, float dy, float viewport_height, float depth)
{
    float view_height = orthographic ? half_height() * 2.0f : 2.0f * depth * std::tan(fov_y * 0.5f);
    float world_per_pixel = view_height / std::max(viewport_height, 1.0f);
    basis_t b = basis();
    target = target - b.right * (dx * world_per_pixel) + b.up * (dy * world_per_pixel);
}

//////////////////////////////////////////////////////////////////////

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
// Scaling the target's sideways offset from the point (depth doesn't matter
// in orthographic) keeps the point on the same pixel

void camera::scale_view(float wheel_clicks, gpu::vec3 const &point, float viewport_height)
{
    // scene's bounding sphere no smaller than min_scene_pixels
    float max_distance = scene_radius * std::max(viewport_height, 1.0f) / (min_scene_pixels * std::tan(fov_y * 0.5f));
    float new_distance = std::clamp(distance * std::pow(ortho_zoom_step, wheel_clicks), scene_radius * 1e-5f, std::max(max_distance, distance));
    float scale = new_distance / distance;

    basis_t b = basis();
    gpu::vec3 offset = target - point;
    float along = gpu::dot(offset, b.forward);
    gpu::vec3 sideways = offset - b.forward * along;
    target = point + sideways * scale + b.forward * along;
    distance = new_distance;
}

//////////////////////////////////////////////////////////////////////

void camera::ray(float ndc_x, float ndc_y, float aspect, gpu::vec3 &origin, gpu::vec3 &direction) const
{
    basis_t b = basis();
    if(orthographic) {
        float h = half_height();
        origin = eye() + b.right * (ndc_x * h * aspect) + b.up * (ndc_y * h);
        direction = b.forward;
    } else {
        origin = eye();
        float t = std::tan(fov_y * 0.5f);
        direction = gpu::normalize(b.forward + b.right * (ndc_x * t * aspect) + b.up * (ndc_y * t));
    }
}

//////////////////////////////////////////////////////////////////////

float camera::half_height() const
{
    return distance * std::tan(fov_y * 0.5f);
}

//////////////////////////////////////////////////////////////////////

camera::basis_t camera::basis() const
{
    basis_t b;
    b.forward = forward;
    b.right = gpu::normalize(gpu::cross(forward, up));
    b.up = gpu::cross(b.right, forward);
    return b;
}

//////////////////////////////////////////////////////////////////////
// orthographic pulls the eye back out of the scene (it doesn't change what's
// visible, just keeps the whole scene in front of it)

gpu::vec3 camera::eye() const
{
    float back = distance;
    if(orthographic) {
        back = std::max(distance, target.length() + scene_radius * 2.0f);
    }
    return target - forward * back;
}

//////////////////////////////////////////////////////////////////////

gpu::mat4 camera::view_matrix() const
{
    return gpu::mat4::look_at(eye(), target, basis().up);
}

//////////////////////////////////////////////////////////////////////
// Reversed Z keeps the depth precision good everywhere, so the near plane can
// be very close and perspective can go out to infinity (the grid and axes)

gpu::mat4 camera::projection_matrix(float aspect) const
{
    float eye_distance = eye().length();
    if(orthographic) {
        float h = half_height();
        float far_z = eye_distance + std::max(scene_radius * 1.1f, far_extent);
        return gpu::mat4::orthographic_reversed(h * aspect, h, 0.0f, far_z);
    }
    float near_z = std::max(scene_radius * 1e-5f, eye_distance * 1e-4f);
    return gpu::mat4::perspective_reversed_infinite(fov_y, aspect, near_z);
}
