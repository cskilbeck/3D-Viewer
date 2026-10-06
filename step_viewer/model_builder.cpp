//////////////////////////////////////////////////////////////////////

#include <Bnd_Box.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <stb_image.h>
#include <webp/decode.h>

#include "log.h"
#include "model_builder.h"

LOG_CONTEXT("model_builder", info);

//////////////////////////////////////////////////////////////////////

std::optional<step_texture> decode_texture(uint8_t const *data, size_t size, bool srgb, std::string const &name)
{
    step_texture result;
    result.srgb = srgb;

    if(size >= 12 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBP", 4) == 0) {
        int width, height;
        uint8_t *pixels = WebPDecodeRGBA(data, size, &width, &height);
        if(pixels == nullptr) {
            LOG_WARNING("Can't decode WebP texture {}", name);
            return std::nullopt;
        }
        result.width = (uint32_t)width;
        result.height = (uint32_t)height;
        result.pixels.assign(pixels, pixels + (size_t)width * height * 4);
        WebPFree(pixels);
    } else {
        int width, height, channels;
        uint8_t *pixels = stbi_load_from_memory(data, (int)size, &width, &height, &channels, 4);
        if(pixels == nullptr) {
            LOG_WARNING("Can't decode texture {}: {}", name, stbi_failure_reason());
            return std::nullopt;
        }
        result.width = (uint32_t)width;
        result.height = (uint32_t)height;
        result.pixels.assign(pixels, pixels + (size_t)width * height * 4);
        stbi_image_free(pixels);
    }
    return result;
}

//////////////////////////////////////////////////////////////////////

void model_builder::begin_part()
{
    part_first_vertex = model.vertices.size();
    part_first_edge_vertex = (uint32_t)model.edges.size();
    opaque_indices.clear();
    transparent_indices.clear();
}

//////////////////////////////////////////////////////////////////////

int model_builder::end_part(int node_index)
{
    step_part part;
    part.node = node_index;
    part.first_edge_vertex = part_first_edge_vertex;
    part.num_edge_vertices = (uint32_t)model.edges.size() - part_first_edge_vertex;
    part.first_index = (uint32_t)model.indices.size();
    part.first_batch = (uint32_t)model.batches.size();
    append_batches(opaque_indices, (int)model.parts.size());
    part.num_indices = (uint32_t)model.indices.size() - part.first_index;
    part.num_batches = (uint32_t)model.batches.size() - part.first_batch;

    bool has_transparent = std::any_of(transparent_indices.begin(), transparent_indices.end(), [](auto const &t) { return !t.second.empty(); });
    if(part.num_indices == 0 && !has_transparent) {
        return -1;
    }

    float constexpr big = 3.4e38f;
    part.bounds_min = { big, big, big };
    part.bounds_max = { -big, -big, -big };
    for(size_t i = part_first_vertex; i < model.vertices.size(); ++i) {
        float const *p = model.vertices[i].position;
        part.bounds_min = { std::min(part.bounds_min.x, p[0]), std::min(part.bounds_min.y, p[1]), std::min(part.bounds_min.z, p[2]) };
        part.bounds_max = { std::max(part.bounds_max.x, p[0]), std::max(part.bounds_max.y, p[1]), std::max(part.bounds_max.z, p[2]) };
    }

    int part_index = (int)model.parts.size();
    model.nodes[node_index].part = part_index;
    model.parts.push_back(part);
    part_transparent_indices.push_back(transparent_indices);
    return part_index;
}

//////////////////////////////////////////////////////////////////////
// put the transparent triangles after all the opaque ones

void model_builder::finish()
{
    model.num_opaque_indices = (uint32_t)model.indices.size();
    model.num_opaque_batches = (uint32_t)model.batches.size();
    for(size_t i = 0; i < model.parts.size(); ++i) {
        step_part &part = model.parts[i];
        part.first_transparent_index = (uint32_t)model.indices.size();
        part.first_transparent_batch = (uint32_t)model.batches.size();
        append_batches(part_transparent_indices[i], (int)i);
        part.num_transparent_indices = (uint32_t)model.indices.size() - part.first_transparent_index;
        part.num_transparent_batches = (uint32_t)model.batches.size() - part.first_transparent_batch;
    }
    part_transparent_indices.clear();
}

//////////////////////////////////////////////////////////////////////

void model_builder::append_batches(material_indices const &by_material, int part_index)
{
    for(auto const &[material, indices] : by_material) {
        if(!indices.empty()) {
            model.batches.push_back({ (uint32_t)model.indices.size(), (uint32_t)indices.size(), material, part_index });
            model.indices.insert(model.indices.end(), indices.begin(), indices.end());
        }
    }
}

//////////////////////////////////////////////////////////////////////
// where a triangle with this color and material goes

std::vector<uint32_t> &model_builder::triangle_list(uint32_t color, int material)
{
    step_material const &m = model.materials[material];
    bool transparent = m.blend || (!m.opaque && m.alpha_cutoff == 0 && (color >> 24) != 0xff);
    return transparent ? transparent_indices[material] : opaque_indices[material];
}

//////////////////////////////////////////////////////////////////////

void model_builder::add_vertex(gp_Pnt const &p, gp_Dir const &n, gp_Pnt2d const &uv, uint32_t color)
{
    gp_XYZ v = p.XYZ() - center;
    // texture coordinates come with 0 at the bottom of the image, flip them so 0 is the top
    model.vertices.push_back(
        { { (float)v.X(), (float)v.Y(), (float)v.Z() }, { (float)n.X(), (float)n.Y(), (float)n.Z() }, { (float)uv.X(), 1.0f - (float)uv.Y() }, color });
}

//////////////////////////////////////////////////////////////////////

void model_builder::add_edge_vertex(gp_Pnt const &p)
{
    gp_XYZ v = p.XYZ() - center;
    model.edges.push_back({ { (float)v.X(), (float)v.Y(), (float)v.Z() } });
}

//////////////////////////////////////////////////////////////////////

void model_builder::set_node_color(step_node &node, uint32_t color)
{
    node.has_color = true;
    node.color[0] = (float)(color & 0xff) / 255.0f;
    node.color[1] = (float)((color >> 8) & 0xff) / 255.0f;
    node.color[2] = (float)((color >> 16) & 0xff) / 255.0f;
}

//////////////////////////////////////////////////////////////////////

void model_builder::add_triangles(occ::handle<Poly_Triangulation> const &triangulation,
                                  gp_Trsf const &transform,
                                  bool reversed,
                                  bool flip_winding,
                                  bool mesh_only,
                                  uint32_t color,
                                  int material)
{
    if(triangulation.IsNull() || triangulation->NbTriangles() == 0) {
        return;
    }

    model.num_faces += 1;
    model.num_triangles += triangulation->NbTriangles();

    // meshes have no CAD edges, find the sharp ones
    if(mesh_only) {
        add_feature_edges(triangulation, transform);
    }

    // and if there are no normals, make some which keep the sharp edges sharp
    if(!triangulation->HasNormals()) {
        add_creased_triangles(triangulation, transform, reversed, flip_winding, color, material);
        return;
    }

    uint32_t base = (uint32_t)model.vertices.size();

    // CAD faces have surface parameters, which are no use as texture coordinates
    bool has_uvs = mesh_only && triangulation->HasUVNodes();

    for(int i = 1; i <= triangulation->NbNodes(); ++i) {
        gp_Dir normal = triangulation->Normal(i).Transformed(transform);
        if(reversed) {
            normal.Reverse();
        }
        add_vertex(triangulation->Node(i).Transformed(transform), normal, has_uvs ? triangulation->UVNode(i) : gp_Pnt2d(0, 1), color);
    }

    auto &dest = triangle_list(color, material);
    for(int i = 1; i <= triangulation->NbTriangles(); ++i) {
        int a, b, c;
        triangulation->Triangle(i).Get(a, b, c);
        if(flip_winding) {
            std::swap(b, c);
        }
        dest.push_back(base + a - 1);
        dest.push_back(base + b - 1);
        dest.push_back(base + c - 1);
    }
}

//////////////////////////////////////////////////////////////////////
// Edges for a mesh: where the triangles either side meet at more than the
// crease angle, or there's only one triangle (an open boundary). Vertices
// are welded by position first because a lot of files duplicate them along
// normal/texture seams, which would otherwise look like boundaries

namespace
{
    constexpr double crease_angle_degrees = 35.0;
}

void model_builder::add_feature_edges(occ::handle<Poly_Triangulation> const &triangulation, gp_Trsf const &transform)
{
    double const crease_cos = std::cos(crease_angle_degrees * 3.14159265358979 / 180.0);

    int num_nodes = triangulation->NbNodes();
    int num_triangles = triangulation->NbTriangles();

    // weld: quantize positions to a tiny fraction of the mesh size
    Bnd_Box box;
    for(int i = 1; i <= num_nodes; ++i) {
        box.Add(triangulation->Node(i));
    }
    double cell = std::max(std::sqrt(box.SquareExtent()) * 1e-7, 1e-12);

    struct key_hash
    {
        size_t operator()(std::array<int64_t, 3> const &k) const noexcept
        {
            return std::hash<int64_t>{}(k[0] * 73856093LL ^ k[1] * 19349663LL ^ k[2] * 83492791LL);
        }
    };
    std::unordered_map<std::array<int64_t, 3>, int, key_hash> position_ids;
    std::vector<int> welded(num_nodes + 1);
    for(int i = 1; i <= num_nodes; ++i) {
        gp_XYZ p = triangulation->Node(i).XYZ();
        std::array<int64_t, 3> key{ std::llround(p.X() / cell), std::llround(p.Y() / cell), std::llround(p.Z() / cell) };
        welded[i] = position_ids.try_emplace(key, i).first->second;
    }

    // triangles around each (welded) edge
    struct edge_info
    {
        int a, b;    // original node indices, for positions
        int triangles[2];
        int count;
    };
    std::unordered_map<uint64_t, edge_info> edges;
    std::vector<gp_XYZ> normals(num_triangles);

    for(int t = 0; t < num_triangles; ++t) {
        int n[3];
        triangulation->Triangle(t + 1).Get(n[0], n[1], n[2]);
        gp_XYZ p0 = triangulation->Node(n[0]).XYZ();
        gp_XYZ normal = (triangulation->Node(n[1]).XYZ() - p0).Crossed(triangulation->Node(n[2]).XYZ() - p0);
        double length = normal.Modulus();
        normals[t] = length > 0 ? normal / length : gp_XYZ(0, 0, 0);
        for(int k = 0; k < 3; ++k) {
            int a = n[k];
            int b = n[(k + 1) % 3];
            int wa = welded[a];
            int wb = welded[b];
            if(wa == wb) {
                continue;    // degenerate
            }
            uint64_t key = ((uint64_t)std::min(wa, wb) << 32) | (uint32_t)std::max(wa, wb);
            auto [it, inserted] = edges.try_emplace(key, edge_info{ a, b, { t, -1 }, 0 });
            if(it->second.count < 2) {
                it->second.triangles[it->second.count] = t;
            }
            it->second.count += 1;
        }
    }

    for(auto const &[key, edge] : edges) {
        bool sharp = edge.count != 2;    // boundary or non-manifold
        if(!sharp) {
            // winding may differ either side (welded seams), so compare the angle either way round
            double cos_angle = std::abs(normals[edge.triangles[0]].Dot(normals[edge.triangles[1]]));
            sharp = cos_angle < crease_cos;
        }
        if(sharp) {
            add_edge_vertex(triangulation->Node(edge.a).Transformed(transform));
            add_edge_vertex(triangulation->Node(edge.b).Transformed(transform));
        }
    }
}

//////////////////////////////////////////////////////////////////////
// Mesh formats (STL, OBJ without normals etc) share vertices across sharp
// edges, so averaging the normals at each vertex makes everything look
// blobby. Instead each corner of each triangle averages only the triangles
// around that vertex which are within the crease angle of its own

void model_builder::add_creased_triangles(occ::handle<Poly_Triangulation> const &triangulation,
                                          gp_Trsf const &transform,
                                          bool reversed,
                                          bool flip_winding,
                                          uint32_t color,
                                          int material)
{
    bool has_uvs = triangulation->HasUVNodes();
    double const crease_cos = std::cos(crease_angle_degrees * 3.14159265358979 / 180.0);

    int num_nodes = triangulation->NbNodes();
    int num_triangles = triangulation->NbTriangles();

    // area weighted (cross product) and unit normal of each triangle
    std::vector<gp_XYZ> weighted(num_triangles);
    std::vector<gp_XYZ> unit(num_triangles);
    std::vector<int> corners(num_triangles * 3);

    // which triangles use each vertex (compressed: first[v]..first[v+1])
    std::vector<int> first(num_nodes + 2, 0);

    for(int t = 0; t < num_triangles; ++t) {
        int n[3];
        triangulation->Triangle(t + 1).Get(n[0], n[1], n[2]);
        gp_XYZ p0 = triangulation->Node(n[0]).XYZ();
        gp_XYZ e1 = triangulation->Node(n[1]).XYZ() - p0;
        gp_XYZ e2 = triangulation->Node(n[2]).XYZ() - p0;
        weighted[t] = e1.Crossed(e2);
        double length = weighted[t].Modulus();
        unit[t] = length > 0 ? weighted[t] / length : gp_XYZ(0, 0, 0);
        for(int k = 0; k < 3; ++k) {
            corners[t * 3 + k] = n[k];
            first[n[k] + 1] += 1;
        }
    }
    for(int v = 1; v <= num_nodes + 1; ++v) {
        first[v] += first[v - 1];
    }
    std::vector<int> fill(first.begin(), first.end());
    std::vector<int> adjacent(num_triangles * 3);
    for(int t = 0; t < num_triangles; ++t) {
        for(int k = 0; k < 3; ++k) {
            adjacent[fill[corners[t * 3 + k]]++] = t;
        }
    }

    // every corner gets its own vertex
    auto &dest = triangle_list(color, material);
    for(int t = 0; t < num_triangles; ++t) {
        uint32_t base = (uint32_t)model.vertices.size();
        for(int k = 0; k < 3; ++k) {
            int v = corners[t * 3 + k];
            gp_XYZ sum(0, 0, 0);
            for(int i = first[v]; i < first[v + 1]; ++i) {
                int other = adjacent[i];
                if(unit[t].Dot(unit[other]) >= crease_cos) {
                    sum += weighted[other];
                }
            }
            if(sum.Modulus() <= 0) {
                sum = unit[t].Modulus() > 0 ? unit[t] : gp_XYZ(0, 0, 1);
            }
            gp_Dir normal = gp_Dir(sum).Transformed(transform);
            if(reversed) {
                normal.Reverse();
            }
            add_vertex(triangulation->Node(v).Transformed(transform), normal, has_uvs ? triangulation->UVNode(v) : gp_Pnt2d(0, 1), color);
        }
        uint32_t a = base, b = base + 1, c = base + 2;
        if(flip_winding) {
            std::swap(b, c);
        }
        dest.push_back(a);
        dest.push_back(b);
        dest.push_back(c);
    }
}
