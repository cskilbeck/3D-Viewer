//////////////////////////////////////////////////////////////////////

#include <BRepBndLib.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <IMeshTools_Parameters.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <Quantity_Color.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <DEBREP_ConfigurationNode.hxx>
#include <DEGLTF_ConfigurationNode.hxx>
#include <DEIGES_ConfigurationNode.hxx>
#include <DEOBJ_ConfigurationNode.hxx>
#include <DESTEP_ConfigurationNode.hxx>
#include <DESTL_ConfigurationNode.hxx>
#include <DEVRML_ConfigurationNode.hxx>
#include <DEXCAF_ConfigurationNode.hxx>
#include <DE_Wrapper.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_TShape.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <BRep_Builder.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_VisMaterial.hxx>
#include <XCAFDoc_VisMaterialTool.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "log.h"
#include "util.h"
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
    // Build the tree and flatten the assembly into triangles + edges for the GPU

    struct mesh_builder
    {
        using shape_color_map = std::unordered_map<TopoDS_Shape, uint32_t, TopTools_ShapeMapHasher, TopTools_ShapeMapHasher>;
        using shape_set = std::unordered_set<TopoDS_Shape, TopTools_ShapeMapHasher, TopTools_ShapeMapHasher>;

        static constexpr uint32_t default_color = 0xffbfbfbf;

        step_model &model;
        gp_XYZ center;
        std::stop_token stop;

        // transparent triangles of the part being added, and of all the parts so far (by part index)
        std::vector<uint32_t> transparent_indices;
        std::vector<std::vector<uint32_t>> part_transparent_indices;

        //////////////////////////////////////////////////////////////////////
        // put the transparent triangles after all the opaque ones

        void finish()
        {
            model.num_opaque_indices = (uint32_t)model.indices.size();
            for(size_t i = 0; i < model.parts.size(); ++i) {
                step_part &part = model.parts[i];
                part.first_transparent_index = (uint32_t)model.indices.size();
                part.num_transparent_indices = (uint32_t)part_transparent_indices[i].size();
                model.indices.insert(model.indices.end(), part_transparent_indices[i].begin(), part_transparent_indices[i].end());
            }
            part_transparent_indices.clear();
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
            // mesh formats (OBJ, glTF) have materials rather than colors
            occ::handle<XCAFDoc_VisMaterial> material = XCAFDoc_VisMaterialTool::GetShapeMaterial(label);
            if(!material.IsNull()) {
                return pack_color(material->BaseColor());
            }
            return std::nullopt;
        }

        //////////////////////////////////////////////////////////////////////

        void add_vertex(gp_Pnt const &p, gp_Dir const &n, uint32_t color)
        {
            gp_XYZ v = p.XYZ() - center;
            model.vertices.push_back({ { (float)v.X(), (float)v.Y(), (float)v.Z() }, { (float)n.X(), (float)n.Y(), (float)n.Z() }, color });
        }

        //////////////////////////////////////////////////////////////////////

        void add_edge_vertex(gp_Pnt const &p)
        {
            gp_XYZ v = p.XYZ() - center;
            model.edges.push_back({ { (float)v.X(), (float)v.Y(), (float)v.Z() } });
        }

        //////////////////////////////////////////////////////////////////////

        static void set_node_color(step_node &node, uint32_t color)
        {
            node.has_color = true;
            node.color[0] = (float)(color & 0xff) / 255.0f;
            node.color[1] = (float)((color >> 8) & 0xff) / 255.0f;
            node.color[2] = (float)((color >> 16) & 0xff) / 255.0f;
        }

        //////////////////////////////////////////////////////////////////////
        // label is a free shape or a component (instance) of an assembly
        // colors: face (subshape) > instance (nearest) > part > default
        // returns the index of the new node

        int add_label(TDF_Label const &label, int parent, TopLoc_Location const &parent_location, std::optional<uint32_t> inherited_color)
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

            if(XCAFDoc_ShapeTool::IsReference(label)) {
                location = parent_location * XCAFDoc_ShapeTool::GetLocation(label);
                if(auto instance_color = label_color(label)) {
                    color = instance_color;
                    set_node_color(model.nodes[node_index], *instance_color);
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
                set_node_color(model.nodes[node_index], *own_color);
            }

            if(XCAFDoc_ShapeTool::IsAssembly(shape_label)) {
                model.nodes[node_index].is_assembly = true;
                NCollection_Sequence<TDF_Label> components;
                XCAFDoc_ShapeTool::GetComponents(shape_label, components);
                for(TDF_Label const &component : components) {
                    // careful, add_label() can reallocate model.nodes
                    int child = add_label(component, node_index, location, color);
                    model.nodes[node_index].children.push_back(child);
                }
                return node_index;
            }

            if(!color.has_value()) {
                color = own_color;
            }

            TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(shape_label);
            if(shape.IsNull()) {
                return node_index;
            }

            // colors attached to faces (or solids/shells) of the part
            shape_color_map face_colors;
            NCollection_Sequence<TDF_Label> sub_shapes;
            if(XCAFDoc_ShapeTool::GetSubShapes(shape_label, sub_shapes)) {
                for(TDF_Label const &sub_label : sub_shapes) {
                    if(auto sub_color = label_color(sub_label)) {
                        TopoDS_Shape sub_shape = XCAFDoc_ShapeTool::GetShape(sub_label);
                        for(TopExp_Explorer exp(sub_shape, TopAbs_FACE); exp.More(); exp.Next()) {
                            face_colors[exp.Current()] = *sub_color;
                        }
                    }
                }
            }

            // record where this part's triangles are
            step_part part;
            part.node = node_index;
            part.first_index = (uint32_t)model.indices.size();
            part.first_edge_vertex = (uint32_t)model.edges.size();
            size_t first_vertex = model.vertices.size();
            transparent_indices.clear();

            add_shape(shape, location, color.value_or(default_color), face_colors);

            part.num_indices = (uint32_t)model.indices.size() - part.first_index;
            part.num_edge_vertices = (uint32_t)model.edges.size() - part.first_edge_vertex;
            if(part.num_indices != 0 || !transparent_indices.empty()) {
                float constexpr big = 3.4e38f;
                part.bounds_min = { big, big, big };
                part.bounds_max = { -big, -big, -big };
                for(size_t i = first_vertex; i < model.vertices.size(); ++i) {
                    float const *p = model.vertices[i].position;
                    part.bounds_min = { std::min(part.bounds_min.x, p[0]), std::min(part.bounds_min.y, p[1]), std::min(part.bounds_min.z, p[2]) };
                    part.bounds_max = { std::max(part.bounds_max.x, p[0]), std::max(part.bounds_max.y, p[1]), std::max(part.bounds_max.z, p[2]) };
                }
                model.nodes[node_index].part = (int)model.parts.size();
                model.parts.push_back(part);
                part_transparent_indices.push_back(transparent_indices);
            }
            return node_index;
        }

        //////////////////////////////////////////////////////////////////////
        // Edges for a mesh: where the triangles either side meet at more than the
        // crease angle, or there's only one triangle (an open boundary). Vertices
        // are welded by position first because a lot of files duplicate them along
        // normal/texture seams, which would otherwise look like boundaries

        static constexpr double crease_angle_degrees = 35.0;

        void add_feature_edges(occ::handle<Poly_Triangulation> const &triangulation, gp_Trsf const &transform)
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
                int a, b;               // original node indices, for positions
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

        void add_creased_triangles(occ::handle<Poly_Triangulation> const &triangulation, gp_Trsf const &transform, bool reversed, bool flip_winding,
                                   uint32_t face_color)
        {
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
                    add_vertex(triangulation->Node(v).Transformed(transform), normal, face_color);
                }
                uint32_t a = base, b = base + 1, c = base + 2;
                if(flip_winding) {
                    std::swap(b, c);
                }
                auto &dest = (face_color >> 24) == 0xff ? model.indices : transparent_indices;
                dest.push_back(a);
                dest.push_back(b);
                dest.push_back(c);
            }
        }

        //////////////////////////////////////////////////////////////////////

        void add_shape(TopoDS_Shape const &shape, TopLoc_Location const &location, uint32_t color, shape_color_map const &face_colors)
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
                if(auto found = face_colors.find(face); found != face_colors.end()) {
                    face_color = found->second;
                }

                // faces from mesh formats (no surface) have no CAD edges, find the sharp ones
                TopLoc_Location surface_location;
                bool mesh_only = BRep_Tool::Surface(face, surface_location).IsNull();
                if(mesh_only) {
                    add_feature_edges(triangulation, transform);
                }

                // and if the file didn't have normals, make some which keep the sharp edges sharp
                if(mesh_only && !triangulation->HasNormals()) {
                    add_creased_triangles(triangulation, transform, reversed, flip_winding, face_color);
                    model.num_faces += 1;
                    model.num_triangles += triangulation->NbTriangles();
                    continue;
                }

                BRepLib_ToolTriangulatedShape::ComputeNormals(face, triangulation);

                uint32_t base = (uint32_t)model.vertices.size();

                for(int i = 1; i <= triangulation->NbNodes(); ++i) {
                    gp_Dir normal = triangulation->Normal(i).Transformed(transform);
                    if(reversed) {
                        normal.Reverse();
                    }
                    add_vertex(triangulation->Node(i).Transformed(transform), normal, face_color);
                }

                for(int i = 1; i <= triangulation->NbTriangles(); ++i) {
                    int a, b, c;
                    triangulation->Triangle(i).Get(a, b, c);
                    if(flip_winding) {
                        std::swap(b, c);
                    }
                    auto &dest = (face_color >> 24) == 0xff ? model.indices : transparent_indices;
                    dest.push_back(base + a - 1);
                    dest.push_back(base + b - 1);
                    dest.push_back(base + c - 1);
                }

                model.num_faces += 1;
                model.num_triangles += triangulation->NbTriangles();
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
                    add_edge_vertex(triangulation->Node(polygon->Node(i)).Transformed(transform));
                    add_edge_vertex(triangulation->Node(polygon->Node(i + 1)).Transformed(transform));
                }
            }
        }
    };

}    // namespace

//////////////////////////////////////////////////////////////////////

std::expected<std::unique_ptr<step_model>, std::string> load_step_model(std::filesystem::path const &path, std::stop_token stop, std::atomic<float> &progress)
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

    // OBJ has no units, the reader assumes meters but CAD exports are almost always millimeters
    occ::handle<DEOBJ_ConfigurationNode> obj_reader = new DEOBJ_ConfigurationNode();
    obj_reader->InternalParameters.FileLengthUnit = 0.001;

    occ::handle<DE_ConfigurationNode> const readers[] = {
        new DESTEP_ConfigurationNode(), new DEIGES_ConfigurationNode(), new DESTL_ConfigurationNode(), obj_reader,
        new DEGLTF_ConfigurationNode(), new DEVRML_ConfigurationNode(), new DEBREP_ConfigurationNode(), new DEXCAF_ConfigurationNode(),
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

        // Faces from mesh formats (STL, OBJ etc) have a triangulation but no surface. The mesher
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

    mesh_builder flattener{ *model, gp_XYZ(model->center[0], model->center[1], model->center[2]), stop };

    for(TDF_Label const &label : free_shapes) {
        model->roots.push_back(flattener.add_label(label, -1, TopLoc_Location(), std::nullopt));
    }
    flattener.finish();

    if(stop.stop_requested()) {
        return std::unexpected("Cancelled");
    }

    model->mesh_time = get_time() - t;

    progress = 1;

    LOG_INFO("Loaded {}: {} solids, {} faces, {} triangles ({} transparent), {} edge segments (read {:.2f}s, mesh {:.2f}s)",
             filename,
             model->num_solids,
             model->num_faces,
             model->num_triangles,
             (model->indices.size() - model->num_opaque_indices) / 3,
             model->edges.size() / 2,
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
