#include "io.hpp"

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <StlAPI_Writer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <algorithm>
#include <cctype>
#include <sstream>

using nlohmann::json;

namespace cadx {

namespace {

std::string lowerExt(const std::string& path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return "";
  std::string e = path.substr(dot + 1);
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return e;
}

void requireShape(const TopoDS_Shape& s) {
  if (s.IsNull()) throw IoError("the part has no geometry to export");
}

int count(const TopoDS_Shape& s, TopAbs_ShapeEnum t) {
  TopTools_IndexedMapOfShape map;  // unique sub-shapes (a shared edge counts once)
  TopExp::MapShapes(s, t, map);
  return map.Extent();
}

}  // namespace

void exportStep(const TopoDS_Shape& shape, const std::string& path, const std::string& name) {
  requireShape(shape);
  STEPControl_Writer writer;
  Interface_Static::SetCVal("write.step.unit", "MM");
  Interface_Static::SetCVal("write.step.schema", "AP214IS");
  if (!name.empty()) Interface_Static::SetCVal("write.step.product.name", name.c_str());
  if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone) throw IoError("could not convert the part to STEP");
  if (writer.Write(path.c_str()) != IFSelect_RetDone) throw IoError("could not write " + path);
}

void exportIges(const TopoDS_Shape& shape, const std::string& path) {
  requireShape(shape);
  IGESControl_Controller::Init();
  IGESControl_Writer writer("MM", 1);  // 1 = B-rep (faces), not trimmed surfaces only
  if (!writer.AddShape(shape)) throw IoError("could not convert the part to IGES");
  writer.ComputeModel();
  if (!writer.Write(path.c_str())) throw IoError("could not write " + path);
}

void exportStl(const TopoDS_Shape& shape, const std::string& path, double deflection) {
  requireShape(shape);
  BRepMesh_IncrementalMesh mesher(shape, deflection, false, 0.2, true);
  mesher.Perform();
  StlAPI_Writer writer;
  writer.ASCIIMode() = false;
  if (!writer.Write(shape, path.c_str())) throw IoError("could not write " + path);
}

void exportBrep(const TopoDS_Shape& shape, const std::string& path) {
  requireShape(shape);
  if (!BRepTools::Write(shape, path.c_str())) throw IoError("could not write " + path);
}

TopoDS_Shape importShape(const std::string& path) {
  const std::string ext = lowerExt(path);
  TopoDS_Shape shape;
  if (ext == "step" || ext == "stp") {
    STEPControl_Reader reader;
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) throw IoError("could not read STEP file " + path);
    reader.TransferRoots();
    shape = reader.OneShape();
  } else if (ext == "iges" || ext == "igs") {
    IGESControl_Controller::Init();
    IGESControl_Reader reader;
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) throw IoError("could not read IGES file " + path);
    reader.TransferRoots();
    shape = reader.OneShape();
  } else if (ext == "brep" || ext == "brp") {
    BRep_Builder bb;
    if (!BRepTools::Read(shape, path.c_str(), bb)) throw IoError("could not read BREP file " + path);
  } else {
    throw IoError("unsupported file type: ." + ext + " (use STEP, IGES or BREP)");
  }
  if (shape.IsNull() || count(shape, TopAbs_FACE) == 0) throw IoError("the file has no usable geometry");
  return shape;
}

std::string toBrepText(const TopoDS_Shape& shape) {
  std::ostringstream out;
  BRepTools::Write(shape, out);
  return out.str();
}

json measure(const TopoDS_Shape& shape) {
  if (shape.IsNull()) return json{{"empty", true}};
  GProp_GProps vol, area;
  BRepGProp::VolumeProperties(shape, vol);
  BRepGProp::SurfaceProperties(shape, area);
  Bnd_Box box;
  BRepBndLib::AddOptimal(shape, box, false, false);
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  if (!box.IsVoid()) box.Get(x0, y0, z0, x1, y1, z1);
  const gp_Pnt c = vol.CentreOfMass();
  return json{{"volume", vol.Mass()},
              {"area", area.Mass()},
              {"centreOfMass", {c.X(), c.Y(), c.Z()}},
              {"bboxMin", {x0, y0, z0}},
              {"bboxMax", {x1, y1, z1}},
              {"solids", count(shape, TopAbs_SOLID)},
              {"faces", count(shape, TopAbs_FACE)},
              {"edges", count(shape, TopAbs_EDGE)}};
}

}  // namespace cadx
