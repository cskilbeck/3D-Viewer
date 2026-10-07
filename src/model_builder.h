//////////////////////////////////////////////////////////////////////
// Turns triangle meshes into a step_model's GPU ready geometry. Shared by the
// loaders: OpenCascade for the CAD formats (STEP etc) and Assimp for the
// mesh formats (glTF, FBX, OBJ...)
//
// Each part is begin_part(), add_triangles() (any number), end_part(), then
// finish() at the end puts all the transparent triangles after the opaque ones

#pragma once

#include <Poly_Triangulation.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_XYZ.hxx>

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "step_model.h"

//////////////////////////////////////////////////////////////////////
// The loaders (load_step_model() picks one by the file extension)

using load_result = std::expected<std::unique_ptr<step_model>, std::string>;

load_result load_occt_model(std::filesystem::path const &path, std::stop_token stop, std::atomic<float> &progress);
load_result load_assimp_model(std::filesystem::path const &path, std::stop_token stop, std::atomic<float> &progress);

// lower case, no dots
std::vector<std::string> occt_extensions();
std::vector<std::string> assimp_extensions();

//////////////////////////////////////////////////////////////////////
// Decode an image file (PNG, JPEG, WebP etc) in memory to RGBA8

std::optional<step_texture> decode_texture(uint8_t const *data, size_t size, bool srgb, std::string const &name);

//////////////////////////////////////////////////////////////////////

struct model_builder
{
    static constexpr uint32_t default_color = 0xffbfbfbf;

    model_builder(step_model &model, gp_XYZ const &center) : model(model), center(center)
    {
    }

    step_model &model;
    gp_XYZ center;    // subtracted from everything so floats stay precise

    void begin_part();

    // returns the index of the part (and sets the node's part) or -1 if nothing was added
    int end_part(int node_index);

    // add a triangle mesh to the current part
    // transform: to model space
    // reversed: the normals point the other way
    // flip_winding: the triangles are the other way round (reversed and/or mirrored)
    // mesh_only: not from a CAD face, so find the sharp edges to draw and any texture coordinates are real
    // if it has no normals, ones which keep the sharp edges sharp are made
    void add_triangles(occ::handle<Poly_Triangulation> const &triangulation,
                       gp_Trsf const &transform,
                       bool reversed,
                       bool flip_winding,
                       bool mesh_only,
                       uint32_t color,
                       int material);

    void add_edge_vertex(gp_Pnt const &p);

    static void set_node_color(step_node &node, uint32_t color);

    void finish();

private:
    // triangles of a part by material
    using material_indices = std::map<int, std::vector<uint32_t>>;

    material_indices opaque_indices;
    material_indices transparent_indices;

    // transparent triangles of all the parts so far (by part index)
    std::vector<material_indices> part_transparent_indices;

    size_t part_first_vertex{};
    uint32_t part_first_edge_vertex{};

    void append_batches(material_indices const &by_material, int part_index);
    std::vector<uint32_t> &triangle_list(uint32_t color, int material);
    void add_vertex(gp_Pnt const &p, gp_Dir const &n, gp_Pnt2d const &uv, uint32_t color);
    void add_feature_edges(occ::handle<Poly_Triangulation> const &triangulation, gp_Trsf const &transform);
    void add_creased_triangles(occ::handle<Poly_Triangulation> const &triangulation,
                               gp_Trsf const &transform,
                               bool reversed,
                               bool flip_winding,
                               uint32_t color,
                               int material);
};
