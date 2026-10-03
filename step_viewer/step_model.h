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

// a part instance's geometry within the model's vertices/indices
// opaque triangles are in one range, transparent ones (if any) in another

struct step_part
{
    int node{ -1 };
    uint32_t first_index{};
    uint32_t num_indices{};
    uint32_t first_transparent_index{};
    uint32_t num_transparent_indices{};
    gpu::vec3 bounds_min{};
    gpu::vec3 bounds_max{};
};

//////////////////////////////////////////////////////////////////////
// GPU ready geometry. Positions are relative to step_model::center so they
// stay precise as floats even if the model is a long way from the origin

struct mesh_vertex
{
    float position[3];
    float normal[3];
    uint32_t color;    // RGBA8 (R in the low byte)
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

    // parts hit by a ray (in vertex space - relative to center), nearest first
    std::vector<pick_hit> pick(gpu::vec3 const &origin, gpu::vec3 const &direction) const;

    // all the parts in (and under) a node
    void get_parts(int node, std::vector<int> &parts_out) const;

    // is ancestor an ancestor of node (or node itself)
    bool is_ancestor(int ancestor, int node) const;
};

//////////////////////////////////////////////////////////////////////
// Load a STEP file and mesh it. Blocking, call it from a worker thread.
// progress is updated (0..1) as it goes, stop_token cancels it.

std::expected<std::unique_ptr<step_model>, std::string> load_step_model(std::filesystem::path const &path, std::stop_token stop, std::atomic<float> &progress);
