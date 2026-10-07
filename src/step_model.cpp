//////////////////////////////////////////////////////////////////////
// Loading CAD formats with OpenCascade, picking the loader for a file, and
// the step_model functions (picking, visibility)

#include <BRepBndLib.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <DEBREP_ConfigurationNode.hxx>
#include <DEIGES_ConfigurationNode.hxx>
#include <DESTEP_ConfigurationNode.hxx>
#include <DESTL_ConfigurationNode.hxx>
#include <DEVRML_ConfigurationNode.hxx>
#include <DEXCAF_ConfigurationNode.hxx>
#include <DE_Wrapper.hxx>
#include <IMeshTools_Parameters.hxx>
#include <Image_Texture.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <NCollection_Buffer.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <Quantity_Color.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_TShape.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_VisMaterial.hxx>
#include <XCAFDoc_VisMaterialTool.hxx>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "log.h"
#include "util.h"
#include "model_builder.h"
#include "step_model.h"

LOG_CONTEXT("step_model", info);

//////////////////////////////////////////////////////////////////////

struct step_document
{
    occ::handle<TDocStd_Document> doc;
    TopoDS_Shape shape;    // compound of all the free shapes

    ~step_document()
    {
        if(!doc.IsNull()) {
            XCAFApp_Application::GetApplication()->Close(doc);
        }
    }
};

namespace
{
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
    // Report progress and check for cancellation

    struct progress_indicator : Message_ProgressIndicator
    {
        progress_indicator(std::stop_token stop_token, std::atomic<float> &progress_value, float progress_start, float progress_scale)
            : stop(std::move(stop_token)), progress(progress_value), start(progress_start), scale(progress_scale)
        {
        }

        bool UserBreak() override
        {
            return stop.stop_requested();
        }

        void Show(const Message_ProgressScope &, const bool) override
        {
            progress = start + (float)GetPosition() * scale;
        }

        std::stop_token stop;
        std::atomic<float> &progress;
        float start;
        float scale;
    };

    //////////////////////////////////////////////////////////////////////

    std::string label_name(TDF_Label const &label)
    {
        occ::handle<TDataStd_Name> name;
        if(label.FindAttribute(TDataStd_Name::GetID(), name)) {
            // AsciiString from ExtendedString converts to UTF-8
            return TCollection_AsciiString(name->Get()).ToCString();
        }
        return {};
    }

    //////////////////////////////////////////////////////////////////////
    // An image from a file, or a buffer in the file

    std::optional<step_texture> load_texture(Image_Texture const &texture, bool srgb)
    {
        std::vector<uint8_t> data;

        occ::handle<NCollection_Buffer> const &buffer = texture.DataBuffer();
        if(!buffer.IsNull() && buffer->Size() != 0) {
            data.assign(buffer->Data(), buffer->Data() + buffer->Size());
        } else if(!texture.FilePath().IsEmpty()) {
            std::string const path_utf8 = texture.FilePath().ToCString();
            std::ifstream file(std::filesystem::path(std::u8string(path_utf8.begin(), path_utf8.end())), std::ios::binary);
            if(!file) {
                LOG_WARNING("Can't open texture {}", path_utf8);
                return std::nullopt;
            }
            file.seekg(0, std::ios::end);
            int64_t file_size = (int64_t)file.tellg();
            int64_t offset = std::max<int64_t>(texture.FileOffset(), 0);
            int64_t length = texture.FileLength() > 0 ? texture.FileLength() : file_size - offset;
            if(offset + length > file_size || length <= 0) {
                LOG_WARNING("Bad texture range in {}", path_utf8);
                return std::nullopt;
            }
            data.resize((size_t)length);
            file.seekg(offset);
            file.read((char *)data.data(), length);
        }
        if(data.empty()) {
            return std::nullopt;
        }
        return decode_texture(data.data(), data.size(), srgb, texture.TextureId().ToCString());
    }

    //////////////////////////////////////////////////////////////////////
    // Build the tree from the XCAF document and flatten the assembly into triangles + edges

    struct xcaf_reader
    {
        // what a face (or solid/shell) of a part says about how it looks, overriding the part
        struct face_style
        {
            std::optional<uint32_t> color;
            int material{ -1 };
        };

        using face_style_map = std::unordered_map<TopoDS_Shape, face_style, TopTools_ShapeMapHasher, TopTools_ShapeMapHasher>;
        using shape_set = std::unordered_set<TopoDS_Shape, TopTools_ShapeMapHasher, TopTools_ShapeMapHasher>;

        step_model &model;
        model_builder &builder;
        std::stop_token stop;

        // materials and textures already converted
        std::unordered_map<XCAFDoc_VisMaterial const *, int> material_ids;
        std::map<std::pair<std::string, bool>, int> texture_ids;

        //////////////////////////////////////////////////////////////////////

        int add_texture(occ::handle<Image_Texture> const &texture, bool srgb)
        {
            if(texture.IsNull()) {
                return -1;
            }
            auto key = std::make_pair(std::string(texture->TextureId().ToCString()), srgb);
            if(auto found = texture_ids.find(key); found != texture_ids.end()) {
                return found->second;
            }
            int id = -1;
            if(std::optional<step_texture> loaded = load_texture(*texture, srgb)) {
                id = (int)model.textures.size();
                model.textures.push_back(std::move(*loaded));
            }
            texture_ids[key] = id;
            return id;
        }

        //////////////////////////////////////////////////////////////////////
        // convert a material (once), returns -1 if there isn't one

        int label_material(TDF_Label const &label)
        {
            occ::handle<XCAFDoc_VisMaterial> vis = XCAFDoc_VisMaterialTool::GetShapeMaterial(label);
            if(vis.IsNull() || vis->IsEmpty()) {
                return -1;
            }
            if(auto found = material_ids.find(vis.get()); found != material_ids.end()) {
                return found->second;
            }

            step_material m;
            if(vis->HasPbrMaterial()) {
                XCAFDoc_VisMaterialPBR const &pbr = vis->PbrMaterial();
                m.metallic = pbr.Metallic;
                m.roughness = pbr.Roughness;
                m.emissive[0] = pbr.EmissiveFactor.r();
                m.emissive[1] = pbr.EmissiveFactor.g();
                m.emissive[2] = pbr.EmissiveFactor.b();
                m.base_color_texture = add_texture(pbr.BaseColorTexture, true);
                m.metallic_roughness_texture = add_texture(pbr.MetallicRoughnessTexture, false);
                m.normal_texture = add_texture(pbr.NormalTexture, false);
                m.occlusion_texture = add_texture(pbr.OcclusionTexture, false);
                m.emissive_texture = add_texture(pbr.EmissiveTexture, true);
                model.has_pbr_materials = true;
            } else {
                // Phong, shininess 0..1 is the exponent / 128
                XCAFDoc_VisMaterialCommon const &common = vis->CommonMaterial();
                float exponent = std::max(common.Shininess * 128.0f, 1.0f);
                m.roughness = std::clamp(std::sqrt(2.0f / (exponent + 2.0f)), 0.05f, 1.0f);
                m.emissive[0] = (float)common.EmissiveColor.Red();
                m.emissive[1] = (float)common.EmissiveColor.Green();
                m.emissive[2] = (float)common.EmissiveColor.Blue();
                m.base_color_texture = add_texture(common.DiffuseTexture, true);
                if(m.base_color_texture >= 0) {
                    model.has_pbr_materials = true;
                }
            }

            switch(vis->AlphaMode()) {
            case Graphic3d_AlphaMode_Opaque:
                m.opaque = true;
                break;
            case Graphic3d_AlphaMode_Mask:
                m.alpha_cutoff = std::max(vis->AlphaCutOff(), 1e-4f);
                break;
            case Graphic3d_AlphaMode_Blend:
            case Graphic3d_AlphaMode_MaskBlend:
                m.blend = true;
                break;
            default:
                break;    // BlendAuto: transparent if the color is
            }

            int id = (int)model.materials.size();
            model.materials.push_back(m);
            material_ids[vis.get()] = id;
            return id;
        }

        //////////////////////////////////////////////////////////////////////

        static uint32_t pack_color(Quantity_ColorRGBA const &color)
        {
            double r, g, b;
            color.GetRGB().Values(r, g, b, Quantity_TOC_sRGB);
            auto byte = [](double f) { return (uint32_t)std::lround(std::clamp(f, 0.0, 1.0) * 255.0); };
            return byte(r) | (byte(g) << 8) | (byte(b) << 16) | (byte(color.Alpha()) << 24);
        }

        //////////////////////////////////////////////////////////////////////

        static std::optional<uint32_t> label_color(TDF_Label const &label)
        {
            Quantity_ColorRGBA color;
            if(XCAFDoc_ColorTool::GetColor(label, XCAFDoc_ColorSurf, color) || XCAFDoc_ColorTool::GetColor(label, XCAFDoc_ColorGen, color)) {
                return pack_color(color);
            }
            // some files have materials rather than colors
            occ::handle<XCAFDoc_VisMaterial> material = XCAFDoc_VisMaterialTool::GetShapeMaterial(label);
            if(!material.IsNull()) {
                return pack_color(material->BaseColor());
            }
            return std::nullopt;
        }

        //////////////////////////////////////////////////////////////////////
        // label is a free shape or a component (instance) of an assembly
        // colors: face (subshape) > instance (nearest) > part > default
        // returns the index of the new node

        int add_label(TDF_Label const &label, int parent, TopLoc_Location const &parent_location, std::optional<uint32_t> inherited_color, int inherited_material)
        {
            int node_index = (int)model.nodes.size();
            model.nodes.emplace_back();
            model.nodes[node_index].parent = parent;
            model.nodes[node_index].name = label_name(label);

            if(stop.stop_requested()) {
                return node_index;
            }

            TopLoc_Location location = parent_location;
            TDF_Label shape_label = label;
            std::optional<uint32_t> color = inherited_color;
            int material = inherited_material;

            if(XCAFDoc_ShapeTool::IsReference(label)) {
                location = parent_location * XCAFDoc_ShapeTool::GetLocation(label);
                if(auto instance_color = label_color(label)) {
                    color = instance_color;
                    model_builder::set_node_color(model.nodes[node_index], *instance_color);
                }
                if(int instance_material = label_material(label); instance_material >= 0) {
                    material = instance_material;
                }
                XCAFDoc_ShapeTool::GetReferredShape(label, shape_label);
                if(model.nodes[node_index].name.empty()) {
                    model.nodes[node_index].name = label_name(shape_label);
                }
            }

            if(model.nodes[node_index].name.empty()) {
                model.nodes[node_index].name = "<unnamed>";
            }

            std::optional<uint32_t> own_color = label_color(shape_label);
            if(own_color.has_value() && !model.nodes[node_index].has_color) {
                model_builder::set_node_color(model.nodes[node_index], *own_color);
            }

            if(XCAFDoc_ShapeTool::IsAssembly(shape_label)) {
                model.nodes[node_index].is_assembly = true;
                NCollection_Sequence<TDF_Label> components;
                XCAFDoc_ShapeTool::GetComponents(shape_label, components);
                for(TDF_Label const &component : components) {
                    // careful, add_label() can reallocate model.nodes
                    int child = add_label(component, node_index, location, color, material);
                    model.nodes[node_index].children.push_back(child);
                }
                return node_index;
            }

            if(!color.has_value()) {
                color = own_color;
            }
            if(material < 0) {
                material = label_material(shape_label);
            }

            TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(shape_label);
            if(shape.IsNull()) {
                return node_index;
            }

            // colors/materials attached to faces (or solids/shells) of the part
            face_style_map face_styles;
            NCollection_Sequence<TDF_Label> sub_shapes;
            if(XCAFDoc_ShapeTool::GetSubShapes(shape_label, sub_shapes)) {
                for(TDF_Label const &sub_label : sub_shapes) {
                    face_style style{ label_color(sub_label), label_material(sub_label) };
                    if(style.color.has_value() || style.material >= 0) {
                        TopoDS_Shape sub_shape = XCAFDoc_ShapeTool::GetShape(sub_label);
                        for(TopExp_Explorer exp(sub_shape, TopAbs_FACE); exp.More(); exp.Next()) {
                            face_styles[exp.Current()] = style;
                        }
                    }
                }
            }

            builder.begin_part();
            add_shape(shape, location, color.value_or(model_builder::default_color), std::max(material, 0), face_styles);
            builder.end_part(node_index);
            return node_index;
        }

        //////////////////////////////////////////////////////////////////////

        void add_shape(TopoDS_Shape const &shape, TopLoc_Location const &location, uint32_t color, int material, face_style_map const &face_styles)
        {
            for(TopExp_Explorer exp(shape, TopAbs_SOLID); exp.More(); exp.Next()) {
                model.num_solids += 1;
            }

            // triangles

            for(TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {

                TopoDS_Face const &face = TopoDS::Face(exp.Current());

                TopLoc_Location face_location;
                occ::handle<Poly_Triangulation> triangulation = BRep_Tool::Triangulation(face, face_location);
                if(triangulation.IsNull() || triangulation->NbTriangles() == 0) {
                    continue;
                }

                gp_Trsf transform = (location * face_location).Transformation();
                bool reversed = face.Orientation() == TopAbs_REVERSED;

                // mirrored instances flip the winding but not the normals
                bool flip_winding = reversed != transform.IsNegative();

                uint32_t face_color = color;
                int face_material = material;
                if(auto found = face_styles.find(face); found != face_styles.end()) {
                    face_color = found->second.color.value_or(face_color);
                    if(found->second.material >= 0) {
                        face_material = found->second.material;
                    }
                }

                // faces from mesh formats (STL, VRML) have no surface
                TopLoc_Location surface_location;
                bool mesh_only = BRep_Tool::Surface(face, surface_location).IsNull();
                if(!mesh_only) {
                    BRepLib_ToolTriangulatedShape::ComputeNormals(face, triangulation);
                }

                builder.add_triangles(triangulation, transform, reversed, flip_winding, mesh_only, face_color, face_material);
            }

            // edges, each one once (they're shared by 2 faces)

            shape_set done_edges;

            for(TopExp_Explorer exp(shape, TopAbs_EDGE); exp.More(); exp.Next()) {

                TopoDS_Edge const &edge = TopoDS::Edge(exp.Current());

                if(BRep_Tool::Degenerated(edge) || !done_edges.insert(edge).second) {
                    continue;
                }

                occ::handle<Poly_PolygonOnTriangulation> polygon;
                occ::handle<Poly_Triangulation> triangulation;
                TopLoc_Location edge_location;
                BRep_Tool::PolygonOnTriangulation(edge, polygon, triangulation, edge_location);
                if(polygon.IsNull() || triangulation.IsNull()) {
                    continue;
                }

                gp_Trsf transform = (location * edge_location).Transformation();

                for(int i = 1; i < polygon->NbNodes(); ++i) {
                    builder.add_edge_vertex(triangulation->Node(polygon->Node(i)).Transformed(transform));
                    builder.add_edge_vertex(triangulation->Node(polygon->Node(i + 1)).Transformed(transform));
                }
            }
        }
    };

}    // namespace

//////////////////////////////////////////////////////////////////////
// OpenCascade does the CAD formats, and STL and VRML (which Assimp is no better at)

std::vector<std::string> occt_extensions()
{
    return { "step", "stp", "stpz", "iges", "igs", "stl", "wrl", "vrml", "brep", "xbf" };
}

//////////////////////////////////////////////////////////////////////

std::vector<std::string> supported_file_extensions()
{
    std::set<std::string> all;
    for(std::string const &extension : occt_extensions()) {
        all.insert(extension);
    }
    for(std::string const &extension : assimp_extensions()) {
        all.insert(extension);
    }
    return { all.begin(), all.end() };
}

//////////////////////////////////////////////////////////////////////

load_result load_step_model(std::filesystem::path const &path, std::stop_token stop, std::atomic<float> &progress)
{
    std::string extension = lower_extension(path);
    std::vector<std::string> const occt = occt_extensions();
    bool use_occt = std::find(occt.begin(), occt.end(), extension) != occt.end();
    load_result result = use_occt ? load_occt_model(path, stop, progress) : load_assimp_model(path, stop, progress);

    // an empty (or not really the right kind of) file can "load" with nothing in it
    if(result.has_value() && result.value()->indices.empty() && result.value()->edges.empty()) {
        std::u8string name = path.u8string();
        return std::unexpected(std::format("There's nothing to show in {}", std::string(name.begin(), name.end())));
    }
    return result;
}

//////////////////////////////////////////////////////////////////////

load_result load_occt_model(std::filesystem::path const &path, std::stop_token stop, std::atomic<float> &progress)
{
    auto model = std::make_unique<step_model>();
    model->path = path;
    model->document = std::make_shared<step_document>();

    progress = 0;

    // read

    double t = get_time();

    occ::handle<TDocStd_Document> doc;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
    model->document->doc = doc;

    // the DE wrapper picks a reader based on the file extension (default settings
    // read names and colors, and everything is converted to millimeters)
    occ::handle<DE_Wrapper> wrapper = new DE_Wrapper();

    occ::handle<DE_ConfigurationNode> const readers[] = {
        new DESTEP_ConfigurationNode(), new DEIGES_ConfigurationNode(), new DESTL_ConfigurationNode(),
        new DEVRML_ConfigurationNode(), new DEBREP_ConfigurationNode(), new DEXCAF_ConfigurationNode(),
    };
    for(auto const &reader : readers) {
        wrapper->Bind(reader);
    }

    // OCCT wants file names in UTF-8
    std::u8string path_utf8 = path.u8string();
    std::string filename(path_utf8.begin(), path_utf8.end());
    {
        occ::handle<progress_indicator> read_progress = new progress_indicator(stop, progress, 0.0f, 0.7f);
        if(!wrapper->Read(TCollection_AsciiString(filename.c_str()), doc, read_progress->Start())) {
            if(stop.stop_requested()) {
                return std::unexpected("Cancelled");
            }
            return std::unexpected(std::format("Can't read {}", filename));
        }
    }

    if(stop.stop_requested()) {
        return std::unexpected("Cancelled");
    }

    model->read_time = get_time() - t;

    // walk the assembly structure

    occ::handle<XCAFDoc_ShapeTool> shape_tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());

    NCollection_Sequence<TDF_Label> free_shapes;
    shape_tool->GetFreeShapes(free_shapes);

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);

    for(TDF_Label const &label : free_shapes) {
        TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(label);
        if(!shape.IsNull()) {
            builder.Add(compound, shape);
        }
    }
    model->document->shape = compound;

    if(stop.stop_requested()) {
        return std::unexpected("Cancelled");
    }

    // mesh it

    t = get_time();

    Bnd_Box bounds;
    BRepBndLib::Add(compound, bounds);
    if(!bounds.IsVoid()) {
        bounds.Get(model->extent_min[0], model->extent_min[1], model->extent_min[2], model->extent_max[0], model->extent_max[1], model->extent_max[2]);
    }

    {
        // deflection is 0.1% of the bounding box diagonal
        double diagonal = bounds.IsVoid() ? 1.0 : std::sqrt(bounds.SquareExtent());

        IMeshTools_Parameters params;
        params.Deflection = diagonal * 0.001;
        params.Relative = false;
        params.Angle = 0.35;
        params.InParallel = true;

        // Faces from mesh formats (STL, VRML) have a triangulation but no surface. The mesher
        // would throw their triangulation away if it didn't like it and have nothing to replace
        // it with, so it only gets faces with surfaces (each shared face once)
        TopoDS_Compound to_mesh;
        builder.MakeCompound(to_mesh);
        std::unordered_set<TopoDS_TShape const *> seen_faces;
        for(TopExp_Explorer exp(compound, TopAbs_FACE); exp.More(); exp.Next()) {
            TopoDS_Face const &face = TopoDS::Face(exp.Current());
            TopLoc_Location surface_location;
            if(!BRep_Tool::Surface(face, surface_location).IsNull() && seen_faces.insert(face.TShape().get()).second) {
                builder.Add(to_mesh, face);
            }
        }

        occ::handle<progress_indicator> mesh_progress = new progress_indicator(stop, progress, 0.7f, 0.3f);
        BRepMesh_IncrementalMesh mesher(to_mesh, params, mesh_progress->Start());
        if(stop.stop_requested()) {
            return std::unexpected("Cancelled");
        }
    }

    // flatten it into vertices/indices/edges, centered around the middle of the bounding box

    if(!bounds.IsVoid()) {
        for(int i = 0; i < 3; ++i) {
            model->center[i] = (model->extent_min[i] + model->extent_max[i]) * 0.5;
        }
        model->radius = std::max(std::sqrt(bounds.SquareExtent()) * 0.5, 1e-6);
    }

    model->materials.emplace_back();    // the default

    model_builder flattener(*model, gp_XYZ(model->center[0], model->center[1], model->center[2]));
    xcaf_reader reader{ *model, flattener, stop };

    for(TDF_Label const &label : free_shapes) {
        model->roots.push_back(reader.add_label(label, -1, TopLoc_Location(), std::nullopt, -1));
    }
    flattener.finish();

    if(stop.stop_requested()) {
        return std::unexpected("Cancelled");
    }

    model->mesh_time = get_time() - t;

    progress = 1;

    LOG_INFO("Loaded {} with OpenCascade: {} solids, {} faces, {} triangles ({} transparent), {} edge segments, {} materials, {} textures (read {:.2f}s, mesh "
             "{:.2f}s)",
             filename,
             model->num_solids,
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

//////////////////////////////////////////////////////////////////////

namespace
{
    // slab test, does the ray hit the box at all (in front of the origin)
    bool ray_hits_box(gpu::vec3 const &origin, gpu::vec3 const &direction, gpu::vec3 const &box_min, gpu::vec3 const &box_max)
    {
        float t_near = 0;
        float t_far = 3.4e38f;
        float const o[3] = { origin.x, origin.y, origin.z };
        float const d[3] = { direction.x, direction.y, direction.z };
        float const lo[3] = { box_min.x, box_min.y, box_min.z };
        float const hi[3] = { box_max.x, box_max.y, box_max.z };
        for(int i = 0; i < 3; ++i) {
            if(std::abs(d[i]) < 1e-12f) {
                if(o[i] < lo[i] || o[i] > hi[i]) {
                    return false;
                }
                continue;
            }
            float t0 = (lo[i] - o[i]) / d[i];
            float t1 = (hi[i] - o[i]) / d[i];
            if(t0 > t1) {
                std::swap(t0, t1);
            }
            t_near = std::max(t_near, t0);
            t_far = std::min(t_far, t1);
            if(t_near > t_far) {
                return false;
            }
        }
        return true;
    }

    //////////////////////////////////////////////////////////////////////
    // Moller-Trumbore, either side, returns distance along the ray or < 0 for a miss

    float ray_hits_triangle(gpu::vec3 const &origin, gpu::vec3 const &direction, gpu::vec3 const &a, gpu::vec3 const &b, gpu::vec3 const &c)
    {
        gpu::vec3 e1 = b - a;
        gpu::vec3 e2 = c - a;
        gpu::vec3 p = gpu::cross(direction, e2);
        float det = gpu::dot(e1, p);
        if(std::abs(det) < 1e-20f) {
            return -1;
        }
        float inv_det = 1.0f / det;
        gpu::vec3 s = origin - a;
        float u = gpu::dot(s, p) * inv_det;
        if(u < 0 || u > 1) {
            return -1;
        }
        gpu::vec3 q = gpu::cross(s, e1);
        float v = gpu::dot(direction, q) * inv_det;
        if(v < 0 || u + v > 1) {
            return -1;
        }
        return gpu::dot(e2, q) * inv_det;
    }

}    // namespace

//////////////////////////////////////////////////////////////////////

std::vector<pick_hit> step_model::pick(gpu::vec3 const &origin, gpu::vec3 const &direction) const
{
    std::vector<pick_hit> hits;

    for(int part_index = 0; part_index < (int)parts.size(); ++part_index) {

        step_part const &part = parts[part_index];

        if(!part.visible || !ray_hits_box(origin, direction, part.bounds_min, part.bounds_max)) {
            continue;
        }

        auto vertex = [this](uint32_t index) {
            float const *p = vertices[index].position;
            return gpu::vec3{ p[0], p[1], p[2] };
        };

        float nearest = -1;
        auto check_range = [&](uint32_t first, uint32_t count) {
            for(uint32_t i = first; i < first + count; i += 3) {
                float t = ray_hits_triangle(origin, direction, vertex(indices[i]), vertex(indices[i + 1]), vertex(indices[i + 2]));
                if(t > 0 && (nearest < 0 || t < nearest)) {
                    nearest = t;
                }
            }
        };
        check_range(part.first_index, part.num_indices);
        check_range(part.first_transparent_index, part.num_transparent_indices);

        if(nearest > 0) {
            hits.push_back({ part_index, nearest });
        }
    }

    std::sort(hits.begin(), hits.end(), [](pick_hit const &a, pick_hit const &b) { return a.distance < b.distance; });
    return hits;
}

//////////////////////////////////////////////////////////////////////

void step_model::get_parts(int node, std::vector<int> &parts_out) const
{
    if(node < 0 || node >= (int)nodes.size()) {
        return;
    }
    if(nodes[node].part >= 0) {
        parts_out.push_back(nodes[node].part);
    }
    for(int child : nodes[node].children) {
        get_parts(child, parts_out);
    }
}

//////////////////////////////////////////////////////////////////////

bool step_model::is_ancestor(int ancestor, int node) const
{
    while(node >= 0) {
        if(node == ancestor) {
            return true;
        }
        node = nodes[node].parent;
    }
    return false;
}

//////////////////////////////////////////////////////////////////////

void step_model::set_visible(int node, bool visible)
{
    if(node < 0 || node >= (int)nodes.size()) {
        return;
    }
    nodes[node].visible = visible;

    bool parent_visible = true;
    for(int parent = nodes[node].parent; parent >= 0; parent = nodes[parent].parent) {
        parent_visible = parent_visible && nodes[parent].visible;
    }
    update_visibility(node, parent_visible);
}

//////////////////////////////////////////////////////////////////////
// the node, everything under it and the nodes above it (so it's not hidden by them) stay visible

void step_model::isolate(int node)
{
    if(node < 0 || node >= (int)nodes.size()) {
        return;
    }
    for(int i = 0; i < (int)nodes.size(); ++i) {
        nodes[i].visible = is_ancestor(i, node) || is_ancestor(node, i);
    }
    for(int root : roots) {
        update_visibility(root, true);
    }
}

//////////////////////////////////////////////////////////////////////

void step_model::show_all()
{
    for(step_node &node : nodes) {
        node.visible = true;
    }
    for(int root : roots) {
        update_visibility(root, true);
    }
}

//////////////////////////////////////////////////////////////////////

void step_model::update_visibility(int node, bool parent_visible)
{
    bool visible = parent_visible && nodes[node].visible;
    if(nodes[node].part >= 0) {
        parts[nodes[node].part].visible = visible;
    }
    for(int child : nodes[node].children) {
        update_visibility(child, visible);
    }
}
