//////////////////////////////////////////////////////////////////////
// Minimal 3D math for the renderer
//
// Matrices are column major (m[column * 4 + row]) which is what the
// shaders expect (HLSL cbuffer default and Metal float4x4) and they
// transform column vectors: v' = M * v
//
// View space is right handed (camera looks down -Z), clip space depth
// is 0..1 (SDL_GPU uses D3D conventions on every backend)

#pragma once

#include <cmath>

namespace gpu
{
    //////////////////////////////////////////////////////////////////////

    struct vec3
    {
        float x{}, y{}, z{};

        vec3 operator+(vec3 const &o) const
        {
            return { x + o.x, y + o.y, z + o.z };
        }

        vec3 operator-(vec3 const &o) const
        {
            return { x - o.x, y - o.y, z - o.z };
        }

        vec3 operator*(float s) const
        {
            return { x * s, y * s, z * s };
        }

        vec3 operator-() const
        {
            return { -x, -y, -z };
        }

        float length() const
        {
            return std::sqrt(x * x + y * y + z * z);
        }
    };

    inline float dot(vec3 const &a, vec3 const &b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline vec3 cross(vec3 const &a, vec3 const &b)
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    inline vec3 normalize(vec3 const &v)
    {
        float l = v.length();
        return l > 0 ? v * (1.0f / l) : v;
    }

    //////////////////////////////////////////////////////////////////////

    struct mat4
    {
        float m[16]{};

        float &at(int row, int column)
        {
            return m[column * 4 + row];
        }

        static mat4 identity()
        {
            mat4 r;
            r.at(0, 0) = r.at(1, 1) = r.at(2, 2) = r.at(3, 3) = 1;
            return r;
        }

        // right handed view matrix, camera at eye looking at target
        static mat4 look_at(vec3 const &eye, vec3 const &target, vec3 const &up)
        {
            vec3 f = normalize(target - eye);
            vec3 s = normalize(cross(f, up));
            vec3 u = cross(s, f);
            mat4 r = identity();
            r.at(0, 0) = s.x;
            r.at(0, 1) = s.y;
            r.at(0, 2) = s.z;
            r.at(1, 0) = u.x;
            r.at(1, 1) = u.y;
            r.at(1, 2) = u.z;
            r.at(2, 0) = -f.x;
            r.at(2, 1) = -f.y;
            r.at(2, 2) = -f.z;
            r.at(0, 3) = -dot(s, eye);
            r.at(1, 3) = -dot(u, eye);
            r.at(2, 3) = dot(f, eye);
            return r;
        }

        // right handed perspective projection, reversed depth (near = 1, infinitely far away = 0)
        static mat4 perspective_reversed_infinite(float fov_y, float aspect, float near_z)
        {
            float y = 1.0f / std::tan(fov_y * 0.5f);
            mat4 r;
            r.at(0, 0) = y / aspect;
            r.at(1, 1) = y;
            r.at(2, 3) = near_z;
            r.at(3, 2) = -1;
            return r;
        }

        // right handed orthographic projection, reversed depth (near = 1, far = 0)
        static mat4 orthographic_reversed(float half_width, float half_height, float near_z, float far_z)
        {
            mat4 r;
            r.at(0, 0) = 1.0f / half_width;
            r.at(1, 1) = 1.0f / half_height;
            r.at(2, 2) = 1.0f / (far_z - near_z);
            r.at(2, 3) = far_z / (far_z - near_z);
            r.at(3, 3) = 1;
            return r;
        }
    };

}    // namespace gpu
