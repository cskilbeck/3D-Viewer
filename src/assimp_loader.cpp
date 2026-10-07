//////////////////////////////////////////////////////////////////////
// Loading mesh formats (glTF, FBX, OBJ, DAE, 3MF, PLY...) with Assimp
//
// The node tree becomes the model tree, each node's meshes become a part (with
// the node's transform baked into the vertices) and the materials are
// converted to PBR ones. Everything ends up in millimeters with Z up, like
// the CAD formats from OpenCascade

#include <assimp/GltfMaterial.h>
#include <assimp/Importer.hpp>
#include <assimp/ProgressHandler.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <Bnd_Box.hxx>
#include <Poly_Triangulation.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

#include "log.h"
#include "util.h"
#include "model_builder.h"

LOG_CONTEXT("assimp", info);

namespace
{
    //////////////////////////////////////////////////////////////////////
    // Report progress and check for cancellation (reading is the first 70%)

    struct progress_handler : Assimp::ProgressHandler
    {
        progress_handler(std::stop_token stop_token, std::atomic<float> &progress_value) : stop(std::move(stop_token)), progress(progress_value)
        {
        }

        bool Update(float percentage) override
        {
            if(percentage >= 0) {
                progress = std::clamp(percentage, 0.0f, 1.0f) * 0.7f;
            }
            return !stop.stop_requested();
        }

        std::stop_token stop;
        std::atomic<float> &progress;
    };

    //////////////////////////////////////////////////////////////////////

    std::string lower_extension(std::filesystem::path const &path)
    {
        std::string extension = path.extension().string();
        if(!extension.empty() && extension[0] == '.') {
            extension.erase(0, 1);
        }
        std::transform(extension.begin(), extension.end(), extension.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
        return extension;
    }

    //////////////////////////////////////////////////////////////////////
    // Assimp leaves units and the up direction as the file has them (apart from
    // FBX and Collada, which it turns Y up). Formats have conventions...

    struct format_convention
    {
        double to_millimeters;
        bool y_up;
    };

    format_convention convention_for(std::string const &extension, aiScene const *scene)
    {
        auto is_one_of = [&](std::initializer_list<char const *> list) {
            return std::any_of(list.begin(), list.end(), [&](char const *e) { return extension == e; });
        };

        // FBX: units are in the metadata as centimeters per unit
        if(extension == "fbx") {
            double centimeters = 1;
            if(scene->mMetaData != nullptr) {
                float f;
                double d;
                if(scene->mMetaData->Get("UnitScaleFactor", f)) {
                    centimeters = f;
                } else if(scene->mMetaData->Get("UnitScaleFactor", d)) {
                    centimeters = d;
                }
            }
            return { centimeters * 10.0, true };
        }

        // Collada is converted to meters
        bool meters = is_one_of({ "gltf", "glb", "dae", "zae", "x3d", "x3db", "ifc", "ifczip" });

        bool z_up = is_one_of({ "3mf", "off", "amf", "3ds", "ifc", "ifczip", "dxf", "md2", "md3", "md5mesh", "mdl", "mdc", "bsp", "pk3", "smd",
                                "vta", "iqm", "stl" });

        return { meters ? 1000.0 : 1.0, !z_up };
    }

    //////////////////////////////////////////////////////////////////////

    float linear_to_srgb(float c)
    {
        c = std::clamp(c, 0.0f, 1.0f);
        return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    }

    uint32_t pack_color(float r, float g, float b, float a)
    {
        auto byte = [](float f) { return (uint32_t)std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f); };
        return byte(r) | (byte(g) << 8) | (byte(b) << 16) | (byte(a) << 24);
    }

    //////////////////////////////////////////////////////////////////////
    // lights, cameras, bones etc are nodes too, but there's nothing to see

    bool has_meshes(aiNode const *node)
    {
        if(node->mNumMeshes != 0) {
            return true;
        }
        for(unsigned i = 0; i < node->mNumChildren; ++i) {
            if(has_meshes(node->mChildren[i])) {
                return true;
            }
        }
        return false;
    }

    //////////////////////////////////////////////////////////////////////

    struct assimp_reader
    {
        step_model &model;
        model_builder &builder;
        aiScene const *scene;
        std::filesystem::path directory;    // for textures in other files
        bool gltf;                          // colors are linear, alpha mode is explicit
        std::stop_token stop;

        // by Assimp material index: ours and the color for its vertices
        std::vector<int> material_ids;
        std::vector<uint32_t> material_colors;

        std::map<std::pair<std::string, bool>, int> texture_ids;

        //////////////////////////////////////////////////////////////////////
        // a texture from inside the file, or another file

        int add_texture(aiMaterial const *material, aiTextureType type, bool srgb)
        {
            aiString path;
            if(material->GetTexture(type, 0, &path) != AI_SUCCESS || path.length == 0) {
                return -1;
            }
            auto key = std::make_pair(std::string(path.C_Str()), srgb);
            if(auto found = texture_ids.find(key); found != texture_ids.end()) {
                return found->second;
            }

            std::optional<step_texture> texture;

            if(aiTexture const *embedded = scene->GetEmbeddedTexture(path.C_Str())) {
                if(embedded->mHeight == 0) {
                    // a compressed image (PNG etc), mWidth is its size in bytes
                    texture = decode_texture((uint8_t const *)embedded->pcData, embedded->mWidth, srgb, path.C_Str());
                } else {
                    texture.emplace();
                    texture->width = embedded->mWidth;
                    texture->height = embedded->mHeight;
                    texture->srgb = srgb;
                    texture->pixels.reserve((size_t)embedded->mWidth * embedded->mHeight * 4);
                    for(size_t i = 0; i < (size_t)embedded->mWidth * embedded->mHeight; ++i) {
                        aiTexel const &t = embedded->pcData[i];
                        texture->pixels.insert(texture->pixels.end(), { t.r, t.g, t.b, t.a });
                    }
                }
            } else {
                std::string name = path.C_Str();
                std::replace(name.begin(), name.end(), '\\', '/');
                std::filesystem::path file = directory / std::filesystem::path(std::u8string(name.begin(), name.end()));
                std::ifstream stream(file, std::ios::binary);
                if(!stream) {
                    // some exporters put absolute paths from the machine they ran on, try next to the model
                    file = directory / std::filesystem::path(std::u8string(name.begin(), name.end())).filename();
                    stream.open(file, std::ios::binary);
                }
                if(stream) {
                    std::vector<uint8_t> data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                    texture = decode_texture(data.data(), data.size(), srgb, name);
                } else {
                    LOG_WARNING("Can't find texture {}", name);
                }
            }

            int id = -1;
            if(texture.has_value()) {
                id = (int)model.textures.size();
                model.textures.push_back(std::move(*texture));
            }
            texture_ids[key] = id;
            return id;
        }

        //////////////////////////////////////////////////////////////////////

        void add_materials()
        {
            for(unsigned i = 0; i < scene->mNumMaterials; ++i) {
                aiMaterial const *material = scene->mMaterials[i];
                step_material m;

                // base color: PBR formats have one, others have a diffuse color (and opacity)
                aiColor4D base(0.75f, 0.75f, 0.75f, 1.0f);
                bool linear = gltf;
                if(material->Get(AI_MATKEY_BASE_COLOR, base) != AI_SUCCESS) {
                    aiColor3D diffuse;
                    if(material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS) {
                        base = aiColor4D(diffuse.r, diffuse.g, diffuse.b, 1.0f);
                    }
                    float opacity;
                    if(material->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
                        base.a = opacity;
                    }
                    linear = false;
                }
                uint32_t color = linear ? pack_color(linear_to_srgb(base.r), linear_to_srgb(base.g), linear_to_srgb(base.b), base.a)
                                        : pack_color(base.r, base.g, base.b, base.a);

                float metallic;
                if(material->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS) {
                    m.metallic = metallic;
                    model.has_pbr_materials = true;
                }
                float roughness;
                if(material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) {
                    m.roughness = roughness;
                } else {
                    // Phong exponent
                    float shininess;
                    if(material->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 0) {
                        m.roughness = std::clamp(std::sqrt(2.0f / (shininess + 2.0f)), 0.05f, 1.0f);
                    }
                }

                aiColor3D emissive(0, 0, 0);
                material->Get(AI_MATKEY_COLOR_EMISSIVE, emissive);
                float emissive_strength = 1;
                material->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissive_strength);
                m.emissive[0] = emissive.r * emissive_strength;
                m.emissive[1] = emissive.g * emissive_strength;
                m.emissive[2] = emissive.b * emissive_strength;

                // glTF says how to use alpha, otherwise it's transparent if it's not opaque
                if(gltf) {
                    aiString mode;
                    material->Get(AI_MATKEY_GLTF_ALPHAMODE, mode);
                    if(strcmp(mode.C_Str(), "BLEND") == 0) {
                        m.blend = true;
                    } else if(strcmp(mode.C_Str(), "MASK") == 0) {
                        float cutoff = 0.5f;
                        material->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff);
                        m.alpha_cutoff = std::max(cutoff, 1e-4f);
                    } else {
                        m.opaque = true;
                    }
                }

                m.base_color_texture = add_texture(material, aiTextureType_BASE_COLOR, true);
                if(m.base_color_texture < 0) {
                    m.base_color_texture = add_texture(material, aiTextureType_DIFFUSE, true);
                }
                m.metallic_roughness_texture = add_texture(material, aiTextureType_GLTF_METALLIC_ROUGHNESS, false);
                m.normal_texture = add_texture(material, aiTextureType_NORMALS, false);
                m.occlusion_texture = add_texture(material, aiTextureType_AMBIENT_OCCLUSION, false);
                if(m.occlusion_texture < 0) {
                    m.occlusion_texture = add_texture(material, aiTextureType_LIGHTMAP, false);    // glTF occlusion
                }
                m.emissive_texture = add_texture(material, aiTextureType_EMISSIVE, true);
                if(m.emissive_texture >= 0 && emissive.IsBlack()) {
                    m.emissive[0] = m.emissive[1] = m.emissive[2] = emissive_strength;
                }

                if(m.base_color_texture >= 0 || m.metallic_roughness_texture >= 0 || m.normal_texture >= 0 || m.emissive_texture >= 0) {
                    model.has_pbr_materials = true;
                }

                material_ids.push_back((int)model.materials.size());
                material_colors.push_back(color);
                model.materials.push_back(m);
            }
        }

        //////////////////////////////////////////////////////////////////////
        // the meshes of a node as one part

        void add_meshes(aiNode const *node, int node_index, aiMatrix4x4 const &transform)
        {
            aiMatrix3x3 normal_transform = aiMatrix3x3(transform);
            bool mirrored = normal_transform.Determinant() < 0;
            normal_transform.Inverse().Transpose();

            builder.begin_part();

            std::optional<uint32_t> part_color;
            bool one_color = true;

            for(unsigned m = 0; m < node->mNumMeshes; ++m) {
                aiMesh const *mesh = scene->mMeshes[node->mMeshes[m]];

                int num_triangles = 0;
                for(unsigned f = 0; f < mesh->mNumFaces; ++f) {
                    num_triangles += mesh->mFaces[f].mNumIndices == 3 ? 1 : 0;
                }
                if(num_triangles == 0 || mesh->mNumVertices == 0) {
                    continue;
                }

                bool has_uvs = mesh->HasTextureCoords(0);
                bool has_normals = mesh->HasNormals();
                occ::handle<Poly_Triangulation> triangulation = new Poly_Triangulation((int)mesh->mNumVertices, num_triangles, has_uvs, has_normals);

                for(unsigned v = 0; v < mesh->mNumVertices; ++v) {
                    aiVector3D p = transform * mesh->mVertices[v];
                    triangulation->SetNode((int)v + 1, gp_Pnt(p.x, p.y, p.z));
                    if(has_normals) {
                        aiVector3D n = normal_transform * mesh->mNormals[v];
                        if(n.SquareLength() > 1e-30f) {
                            triangulation->SetNormal((int)v + 1, gp_Dir(n.x, n.y, n.z));
                        } else {
                            triangulation->SetNormal((int)v + 1, gp_Dir(0, 0, 1));
                        }
                    }
                    if(has_uvs) {
                        aiVector3D const &uv = mesh->mTextureCoords[0][v];
                        triangulation->SetUVNode((int)v + 1, gp_Pnt2d(uv.x, uv.y));
                    }
                }

                int t = 1;
                for(unsigned f = 0; f < mesh->mNumFaces; ++f) {
                    aiFace const &face = mesh->mFaces[f];
                    if(face.mNumIndices == 3) {
                        triangulation->SetTriangle(t++, Poly_Triangle((int)face.mIndices[0] + 1, (int)face.mIndices[1] + 1, (int)face.mIndices[2] + 1));
                    }
                }

                unsigned material_index = std::min(mesh->mMaterialIndex, (unsigned)material_ids.size() - 1);
                uint32_t color = material_colors[material_index];
                if(part_color.has_value() && *part_color != color) {
                    one_color = false;
                }
                part_color = color;

                builder.add_triangles(triangulation, gp_Trsf(), false, mirrored, true, color, material_ids[material_index]);
            }

            if(builder.end_part(node_index) >= 0 && one_color && part_color.has_value()) {
                model_builder::set_node_color(model.nodes[node_index], *part_color);
            }
        }

        //////////////////////////////////////////////////////////////////////
        // returns the index of the new node

        int add_node(aiNode const *node, int parent, aiMatrix4x4 const &parent_transform)
        {
            int node_index = (int)model.nodes.size();
            model.nodes.emplace_back();
            model.nodes[node_index].parent = parent;
            model.nodes[node_index].name = node->mName.C_Str();
            if(model.nodes[node_index].name.empty() && node->mNumMeshes == 1) {
                model.nodes[node_index].name = scene->mMeshes[node->mMeshes[0]]->mName.C_Str();
            }
            if(model.nodes[node_index].name.empty()) {
                model.nodes[node_index].name = "<unnamed>";
            }

            if(stop.stop_requested()) {
                return node_index;
            }

            aiMatrix4x4 transform = parent_transform * node->mTransformation;

            if(node->mNumChildren == 0 && node->mNumMeshes != 0) {
                add_meshes(node, node_index, transform);
                return node_index;
            }

            model.nodes[node_index].is_assembly = true;

            // an assembly with meshes of its own: they go in a part under it
            if(node->mNumMeshes != 0) {
                int part_node = (int)model.nodes.size();
                model.nodes.emplace_back();
                model.nodes[part_node].parent = node_index;
                model.nodes[part_node].name = model.nodes[node_index].name;
                model.nodes[node_index].children.push_back(part_node);
                add_meshes(node, part_node, transform);
            }

            for(unsigned i = 0; i < node->mNumChildren; ++i) {
                if(has_meshes(node->mChildren[i])) {
                    // careful, add_node() can reallocate model.nodes
                    int child = add_node(node->mChildren[i], node_index, transform);
                    model.nodes[node_index].children.push_back(child);
                }
            }
            return node_index;
        }
    };

    //////////////////////////////////////////////////////////////////////
    // bounding box of everything, with all the transforms

    void add_bounds(aiScene const *scene, aiNode const *node, aiMatrix4x4 const &parent_transform, Bnd_Box &box)
    {
        aiMatrix4x4 transform = parent_transform * node->mTransformation;
        for(unsigned m = 0; m < node->mNumMeshes; ++m) {
            aiMesh const *mesh = scene->mMeshes[node->mMeshes[m]];
            for(unsigned v = 0; v < mesh->mNumVertices; ++v) {
                aiVector3D p = transform * mesh->mVertices[v];
                box.Add(gp_Pnt(p.x, p.y, p.z));
            }
        }
        for(unsigned i = 0; i < node->mNumChildren; ++i) {
            add_bounds(scene, node->mChildren[i], transform, box);
        }
    }

}    // namespace

//////////////////////////////////////////////////////////////////////

std::vector<std::string> assimp_extensions()
{
    std::string list;
    Assimp::Importer().GetExtensionList(list);    // "*.3ds;*.obj;..."

    std::vector<std::string> extensions;
    std::stringstream stream(list);
    std::string item;
    while(std::getline(stream, item, ';')) {
        if(item.starts_with("*.")) {
            item.erase(0, 2);
        }
        std::transform(item.begin(), item.end(), item.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
        if(!item.empty()) {
            extensions.push_back(item);
        }
    }
    return extensions;
}

//////////////////////////////////////////////////////////////////////

load_result load_assimp_model(std::filesystem::path const &path, std::stop_token stop, std::atomic<float> &progress)
{
    auto model = std::make_unique<step_model>();
    model->path = path;

    progress = 0;

    double t = get_time();

    std::u8string path_utf8 = path.u8string();
    std::string filename(path_utf8.begin(), path_utf8.end());

    Assimp::Importer importer;
    importer.SetProgressHandler(new progress_handler(stop, progress));    // the importer owns it

    // just triangles, no points or lines
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);

    // no extra nodes for FBX pivots
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    // shared vertices so normals can be made for meshes without them, texture transforms applied to the coordinates
    unsigned const flags = aiProcess_Triangulate | aiProcess_SortByPType | aiProcess_JoinIdenticalVertices | aiProcess_TransformUVCoords;

    aiScene const *scene = importer.ReadFile(filename.c_str(), flags);
    if(stop.stop_requested()) {
        return std::unexpected("Cancelled");
    }
    if(scene == nullptr || scene->mRootNode == nullptr) {
        return std::unexpected(std::format("Can't read {}: {}", filename, importer.GetErrorString()));
    }

    model->read_time = get_time() - t;
    t = get_time();

    std::string extension = lower_extension(path);
    format_convention convention = convention_for(extension, scene);

    // to millimeters, Z up (Y up turns forward (+Z) to -Y, the front of the default view)
    float s = (float)convention.to_millimeters;
    aiMatrix4x4 to_model = convention.y_up ? aiMatrix4x4(s, 0, 0, 0, 0, 0, -s, 0, 0, s, 0, 0, 0, 0, 0, 1) : aiMatrix4x4(s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1);

    Bnd_Box bounds;
    add_bounds(scene, scene->mRootNode, to_model, bounds);
    if(!bounds.IsVoid()) {
        bounds.Get(model->extent_min[0], model->extent_min[1], model->extent_min[2], model->extent_max[0], model->extent_max[1], model->extent_max[2]);
        for(int i = 0; i < 3; ++i) {
            model->center[i] = (model->extent_min[i] + model->extent_max[i]) * 0.5;
        }
        model->radius = std::max(std::sqrt(bounds.SquareExtent()) * 0.5, 1e-6);
    }

    model->materials.emplace_back();    // the default

    model_builder builder(*model, gp_XYZ(model->center[0], model->center[1], model->center[2]));
    assimp_reader reader{ *model, builder, scene, path.parent_path(), extension == "gltf" || extension == "glb", stop };

    reader.add_materials();
    if(reader.material_ids.empty()) {
        reader.material_ids.push_back(0);
        reader.material_colors.push_back(model_builder::default_color);
    }

    // the root node is usually just a container, its children are the top of the tree
    aiNode const *root = scene->mRootNode;
    if(root->mNumMeshes == 0 && root->mNumChildren != 0) {
        aiMatrix4x4 root_transform = to_model * root->mTransformation;
        for(unsigned i = 0; i < root->mNumChildren; ++i) {
            if(has_meshes(root->mChildren[i])) {
                model->roots.push_back(reader.add_node(root->mChildren[i], -1, root_transform));
            }
        }
    } else {
        model->roots.push_back(reader.add_node(root, -1, to_model));
    }
    builder.finish();

    if(stop.stop_requested()) {
        return std::unexpected("Cancelled");
    }

    model->mesh_time = get_time() - t;

    progress = 1;

    LOG_INFO("Loaded {} with Assimp: {} meshes, {} triangles ({} transparent), {} edge segments, {} materials, {} textures (read {:.2f}s, build {:.2f}s)",
             filename,
             model->num_faces,
             model->num_triangles,
             (model->indices.size() - model->num_opaque_indices) / 3,
             model->edges.size() / 2,
             model->materials.size(),
             model->textures.size(),
             model->read_time,
             model->mesh_time);

    return model;
}
