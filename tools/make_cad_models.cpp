//////////////////////////////////////////////////////////////////////
// Writes the CAD test models (models/cad) with OpenCascade
//
//   make_cad_models <output directory>
//
// assembly.*: an assembly with
//   red_box         10 x 20 x 30, red with its top face yellow
//   blue_cylinder   r5 x 20, blue, used twice (the second one lying down)
//   sub_assembly    a green sphere (r6) and a transparent cyan plate (15 x 15 x 3, alpha 0.4) above it
// written as STEP, IGES, XBF (everything), BREP (geometry only) and VRML (meshed)
//
// inches.step: a box 1 x 2 x 3 inches, written in inches (it should load as 25.4 x 50.8 x 76.2 mm)
// far_from_origin.step: a 10mm cube a kilometer away in X and Y

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BinXCAFDrivers.hxx>
#include <IGESCAFControl_Writer.hxx>
#include <IGESControl_Controller.hxx>
#include <Interface_Static.hxx>
#include <Quantity_Color.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_Controller.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <VrmlAPI_Writer.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{
    std::filesystem::path out_dir;

    std::string out(char const *name)
    {
        return (out_dir / name).string();
    }

    void check(bool ok, char const *what)
    {
        printf("%s %s\n", ok ? "wrote" : "FAILED", what);
        if(!ok) {
            fflush(stdout);
            std::quick_exit(1);
        }
    }

    TopLoc_Location translation(double x, double y, double z)
    {
        gp_Trsf t;
        t.SetTranslation(gp_Vec(x, y, z));
        return TopLoc_Location(t);
    }

    Quantity_Color srgb(double r, double g, double b)
    {
        return Quantity_Color(r, g, b, Quantity_TOC_sRGB);
    }

    TDF_Label add_part(occ::handle<XCAFDoc_ShapeTool> const &shapes,
                       occ::handle<XCAFDoc_ColorTool> const &colors,
                       TopoDS_Shape const &shape,
                       char const *name,
                       Quantity_ColorRGBA const &color)
    {
        TDF_Label label = shapes->AddShape(shape, false);
        TDataStd_Name::Set(label, name);
        colors->SetColor(label, color, XCAFDoc_ColorSurf);
        return label;
    }

    occ::handle<TDocStd_Document> new_document()
    {
        occ::handle<TDocStd_Document> doc;
        XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", doc);
        return doc;
    }

    void write_step(occ::handle<TDocStd_Document> const &doc, char const *name)
    {
        STEPCAFControl_Writer writer;
        bool ok = writer.Transfer(doc, STEPControl_AsIs) && writer.Write(out(name).c_str()) == IFSelect_RetDone;
        check(ok, name);
    }
}    // namespace

int main(int argc, char **argv)
{
    if(argc != 2) {
        printf("usage: make_cad_models <output directory>\n");
        return 1;
    }
    out_dir = argv[1];
    std::filesystem::create_directories(out_dir);

    occ::handle<XCAFApp_Application> app = XCAFApp_Application::GetApplication();
    BinXCAFDrivers::DefineFormat(app);
    STEPControl_Controller::Init();
    IGESControl_Controller::Init();

    //////////////////////////////////////////////////////////////////////
    // the assembly

    occ::handle<TDocStd_Document> doc = new_document();
    occ::handle<XCAFDoc_ShapeTool> shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    occ::handle<XCAFDoc_ColorTool> colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());

    TopoDS_Shape box = BRepPrimAPI_MakeBox(10, 20, 30).Shape();
    TopoDS_Shape cylinder = BRepPrimAPI_MakeCylinder(5, 20).Shape();
    TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(6).Shape();
    TopoDS_Shape plate = BRepPrimAPI_MakeBox(15, 15, 3).Shape();

    TDF_Label box_label = add_part(shapes, colors, box, "red_box", Quantity_ColorRGBA(srgb(0.85, 0.1, 0.1), 1.0f));
    TDF_Label cylinder_label = add_part(shapes, colors, cylinder, "blue_cylinder", Quantity_ColorRGBA(srgb(0.15, 0.3, 0.9), 1.0f));
    TDF_Label sphere_label = add_part(shapes, colors, sphere, "green_sphere", Quantity_ColorRGBA(srgb(0.1, 0.7, 0.2), 1.0f));
    TDF_Label plate_label = add_part(shapes, colors, plate, "glass_plate", Quantity_ColorRGBA(srgb(0.3, 0.8, 0.9), 0.4f));

    // the box's top face (the one at z = 30) is yellow
    for(TopExp_Explorer exp(box, TopAbs_FACE); exp.More(); exp.Next()) {
        TopoDS_Face const &face = TopoDS::Face(exp.Current());
        BRepTools::UpdateFaceUVPoints(face);
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        TopLoc_Location location;
        gp_Pnt middle = BRep_Tool::Surface(face, location)->Value((u0 + u1) / 2, (v0 + v1) / 2);
        if(std::abs(middle.Z() - 30) < 1e-6) {
            TDF_Label face_label = shapes->AddSubShape(box_label, face);
            colors->SetColor(face_label, srgb(1.0, 0.85, 0.0), XCAFDoc_ColorSurf);
        }
    }

    TDF_Label sub_assembly = shapes->NewShape();
    TDataStd_Name::Set(sub_assembly, "sub_assembly");
    shapes->AddComponent(sub_assembly, sphere_label, TopLoc_Location());
    shapes->AddComponent(sub_assembly, plate_label, translation(-7.5, -7.5, 8));

    TDF_Label assembly = shapes->NewShape();
    TDataStd_Name::Set(assembly, "assembly");
    shapes->AddComponent(assembly, box_label, TopLoc_Location());
    shapes->AddComponent(assembly, cylinder_label, translation(25, 10, 0));
    gp_Trsf lying_down;
    lying_down.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), 1.5707963267948966);
    lying_down.SetTranslationPart(gp_Vec(40, 30, 5));
    shapes->AddComponent(assembly, cylinder_label, TopLoc_Location(lying_down));
    shapes->AddComponent(assembly, sub_assembly, translation(5, 45, 6));
    shapes->UpdateAssemblies();

    write_step(doc, "assembly.step");

    {
        IGESCAFControl_Writer writer;
        check(writer.Transfer(doc) && writer.Write(out("assembly.iges").c_str()), "assembly.iges");
    }

    check(app->SaveAs(doc, TCollection_ExtendedString(out("assembly.xbf").c_str())) == PCDM_SS_OK, "assembly.xbf");

    {
        // geometry only: all the shapes where they are in the assembly
        TopoDS_Shape all = shapes->GetShape(assembly);
        check(BRepTools::Write(all, out("assembly.brep").c_str()), "assembly.brep");

        BRepMesh_IncrementalMesh mesher(all, 0.05);
        VrmlAPI_Writer writer;
        writer.SetRepresentation(VrmlAPI_ShadedRepresentation);
        check(writer.WriteDoc(doc, out("assembly.wrl").c_str(), 1.0), "assembly.wrl");
    }

    //////////////////////////////////////////////////////////////////////
    // a box in inches

    {
        occ::handle<TDocStd_Document> inches = new_document();
        occ::handle<XCAFDoc_ShapeTool> inch_shapes = XCAFDoc_DocumentTool::ShapeTool(inches->Main());
        occ::handle<XCAFDoc_ColorTool> inch_colors = XCAFDoc_DocumentTool::ColorTool(inches->Main());
        add_part(inch_shapes, inch_colors, BRepPrimAPI_MakeBox(25.4, 50.8, 76.2).Shape(), "inch_box", Quantity_ColorRGBA(srgb(0.8, 0.5, 0.1), 1.0f));
        Interface_Static::SetCVal("write.step.unit", "INCH");
        write_step(inches, "inches.step");
        Interface_Static::SetCVal("write.step.unit", "MM");
    }

    //////////////////////////////////////////////////////////////////////
    // a long way from the origin

    {
        occ::handle<TDocStd_Document> far = new_document();
        occ::handle<XCAFDoc_ShapeTool> far_shapes = XCAFDoc_DocumentTool::ShapeTool(far->Main());
        occ::handle<XCAFDoc_ColorTool> far_colors = XCAFDoc_DocumentTool::ColorTool(far->Main());
        add_part(far_shapes,
                 far_colors,
                 BRepPrimAPI_MakeBox(gp_Pnt(1e6, 1e6, 0), 10, 10, 10).Shape(),
                 "far_box",
                 Quantity_ColorRGBA(srgb(0.6, 0.2, 0.8), 1.0f));
        write_step(far, "far_from_origin.step");
    }

    fflush(stdout);
    std::quick_exit(0);    // OCCT's static destructors assert in Debug
}
