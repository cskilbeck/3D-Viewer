//////////////////////////////////////////////////////////////////////
// STEP model - loaded with OpenCascade
//
// OCCT headers stay out of here (they're big), the document itself is
// hidden behind step_document

#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include "gpu_math.h"

//////////////////////////////////////////////////////////////////////

struct step_document;

// a node in the assembly tree: an assembly or a part (instance)

struct step_node
{
    std::string name;
    bool is_assembly{ false };
    bool has_color{ false };
    bool visible{ true };    // its own setting, it's only shown if its ancestors are visible too
    float color[3]{};
    int parent{ -1 };
    std::vector<int> children;    // indices into step_model::nodes
    int part{ -1 };               // index into step_model::parts if it's a part
};

// a part hit by a ray, distance is along the ray (which is normalized)

struct pick_hit
{
    int part;
    float distance;
};

// a run of triangles (within a part) which all use the same material

struct step_batch
{
    uint32_t first_index{};
    uint32_t num_indices{};
    int material{};
    int part{};
};

// a part instance's geometry within the model's vertices/indices/edges
// opaque triangles are in one range, transparent ones (if any) in another
// parts are in order, so each kind of range goes up from one part to the next
// each range is split into batches by material (consecutive in step_model::batches)

struct step_part
{
    int node{ -1 };
    uint32_t first_index{};
    uint32_t num_indices{};
    uint32_t first_transparent_index{};
    uint32_t num_transparent_indices{};
    uint32_t first_batch{};
    uint32_t num_batches{};
    uint32_t first_transparent_batch{};
    uint32_t num_transparent_batches{};
    uint32_t first_edge_vertex{};
    uint32_t num_edge_vertices{};
    gpu::vec3 bounds_min{};
    gpu::vec3 bounds_max{};
    bool visible{ true };    // its node and all of the node's ancestors are visible
};

//////////////////////////////////////////////////////////////////////
// GPU ready geometry. Positions are relative to step_model::center so they
// stay precise as floats even if the model is a long way from the origin

struct mesh_vertex
{
    float position[3];
    float normal[3];
    float uv[2];       // texture coordinates (0,0 = top left of the image)
    uint32_t color;    // RGBA8 sRGB (R in the low byte), the base color (factor) for realistic shading
};

//////////////////////////////////////////////////////////////////////
// Materials for realistic (PBR, metallic/roughness) shading. CAD colors
// end up in the vertex colors with the default material (index 0)

struct step_texture
{
    uint32_t width{};
    uint32_t height{};
    bool srgb{};                    // color (base color, emissive) rather than data
    std::vector<uint8_t> pixels;    // RGBA8, top row first (freed once it's on the GPU)
};

struct step_material
{
    // the base color factor is in the vertex colors
    float emissive[3]{};
    float metallic{ 0 };
    float roughness{ 0.5f };
    float alpha_cutoff{ 0 };    // alpha mask: discard below this (0 = not masked)
    bool blend{ false };        // alpha blended (transparent)
    bool opaque{ false };       // ignore alpha completely

    // indices into step_model::textures, -1 for none
    int base_color_texture{ -1 };
    int metallic_roughness_texture{ -1 };    // G = roughness, B = metallic
    int normal_texture{ -1 };
    int occlusion_texture{ -1 };             // R
    int emissive_texture{ -1 };
};

struct edge_vertex
{
    float position[3];
};

//////////////////////////////////////////////////////////////////////

struct step_model
{
    std::filesystem::path path;

    std::shared_ptr<step_document> document;

    // assembly structure, roots are the free shapes
    std::vector<step_node> nodes;
    std::vector<int> roots;

    // the part instances, in the order they appear in the vertices/indices
    std::vector<step_part> parts;

    // triangles (indexed triangle list)
    // all the opaque triangles come first, then each part's transparent ones
    std::vector<mesh_vertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t num_opaque_indices{};

    // all the opaque batches (in index order), then the transparent ones
    std::vector<step_batch> batches;
    uint32_t num_opaque_batches{};

    // materials[0] is the default (for CAD colors)
    std::vector<step_material> materials;
    std::vector<step_texture> textures;

    // the file has real materials (glTF etc), always shade it realistically
    bool has_pbr_materials{ false };

    // edges (line list)
    std::vector<edge_vertex> edges;

    // the model space position of (0,0,0) in vertices/edges and a radius which encloses everything
    double center[3]{};
    double radius{ 1 };

    // stats
    size_t num_solids{};
    size_t num_faces{};
    size_t num_triangles{};
    double extent_min[3]{};
    double extent_max[3]{};
    double read_time{};
    double mesh_time{};

    // visible parts hit by a ray (in vertex space - relative to center), nearest first
    std::vector<pick_hit> pick(gpu::vec3 const &origin, gpu::vec3 const &direction) const;

    // show/hide a node (and so everything under it)
    void set_visible(int node, bool visible);

    // hide everything except a node (and what's under it)
    void isolate(int node);

    // make everything visible
    void show_all();

    // all the parts in (and under) a node
    void get_parts(int node, std::vector<int> &parts_out) const;

    // is ancestor an ancestor of node (or node itself)
    bool is_ancestor(int ancestor, int node) const;

private:
    void update_visibility(int node, bool parent_visible);
};

//////////////////////////////////////////////////////////////////////
// Load a model (STEP, glTF, FBX, OBJ...) and mesh it. Blocking, call it from a worker thread.
// progress is updated (0..1) as it goes, stop_token cancels it.
// CAD formats are loaded with OpenCascade, mesh formats with Assimp

std::expected<std::unique_ptr<step_model>, std::string> load_step_model(std::filesystem::path const &path, std::stop_token stop, std::atomic<float> &progress);

// every file extension which can be loaded (lower case, no dot)
std::vector<std::string> supported_file_extensions();
