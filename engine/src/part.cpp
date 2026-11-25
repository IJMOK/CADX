#include "part.hpp"

#include "expr.hpp"
#include "history.hpp"
#include "profile.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <BRepFilletAPI_LocalOperation.hxx>

#include <algorithm>
#include <memory>
#include <cmath>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

using nlohmann::json;

namespace cadx {

const char* toString(FeatureState s) {
  switch (s) {
    case FeatureState::Ok: return "ok";
    case FeatureState::Failed: return "failed";
    case FeatureState::Suppressed: return "suppressed";
  }
  return "?";
}

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

struct FeatureError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// A sketch known to the rebuild: solved once, placed when first needed.
struct SketchSlot {
  sketch::SolveResult solved;
  json planeSpec;                 // null = XY
  std::optional<gp_Ax3> placement;
};

// What a boolean-type feature contributed, so patterns and mirrors can repeat it.
struct ToolRecord {
  NamedShape tool;
  std::string op;
};

struct Context {
  NamedShape body;
  std::map<std::string, SketchSlot> sketches;
  std::map<std::string, gp_Ax3> datums;
  std::vector<DatumInfo> datumList;
  std::map<std::string, ToolRecord> tools;
};

// ------------------------------------------------------------ small helpers

double num(const json& f, const char* key) {
  if (!f.contains(key)) throw FeatureError(std::string("missing ") + key);
  const json& v = f.at(key);
  if (v.is_string()) throw FeatureError(std::string(key) + ": " + v.get<std::string>() + " is not a number");
  if (!v.is_number()) throw FeatureError(std::string(key) + " must be a number");
  return v.get<double>();
}

double num(const json& f, const char* key, double fallback) {
  return f.contains(key) && !f.at(key).is_null() ? num(f, key) : fallback;
}

std::string str(const json& f, const char* key, const std::string& fallback = "") {
  if (!f.contains(key) || f.at(key).is_null()) return fallback;
  if (!f.at(key).is_string()) throw FeatureError(std::string(key) + " must be a string");
  return f.at(key).get<std::string>();
}

gp_Pnt pointOf(const json& f, const char* key) {
  if (!f.contains(key)) return gp_Pnt(0, 0, 0);
  const json& p = f.at(key);
  if (!p.is_array() || p.size() != 3) throw FeatureError(std::string(key) + " must be [x, y, z]");
  for (const auto& v : p) {
    if (!v.is_number()) throw FeatureError(std::string(key) + " has a value that is not a number: " + v.dump());
  }
  return gp_Pnt(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
}

std::vector<std::string> stringList(const json& f, const char* key) {
  std::vector<std::string> out;
  if (!f.contains(key)) return out;
  for (const auto& v : f.at(key)) out.push_back(v.get<std::string>());
  return out;
}

TopoDS_Face findFace(const NamedShape& body, const std::string& name) {
  std::vector<TopoDS_Face> found;
  for (const auto& nf : body.faces()) {
    if (nf.name == name) found.push_back(nf.face);
  }
  if (found.empty()) throw FeatureError("face not found: " + name);
  if (found.size() > 1) throw FeatureError("face name is ambiguous: " + name);
  return found.front();
}

bool hasFace(const NamedShape& body, const std::string& name) {
  for (const auto& nf : body.faces()) {
    if (nf.name == name) return true;
  }
  return false;
}

TopoDS_Edge findEdge(const NamedShape& body, const std::string& name) {
  auto found = body.findEdges(name);
  if (found.empty()) throw FeatureError("edge not found: " + name);
  if (found.size() > 1) throw FeatureError("edge name is ambiguous: " + name);
  return found.front();
}

int solidCount(const TopoDS_Shape& s) {
  int n = 0;
  for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next()) ++n;
  return n;
}

double sizeOf(const TopoDS_Shape& s) {
  if (s.IsNull()) return 0;
  Bnd_Box box;
  BRepBndLib::Add(s, box);
  if (box.IsVoid()) return 0;
  return std::sqrt(box.SquareExtent());
}

// ---------------------------------------------------------------- placement

gp_Ax3 basePlane(const std::string& name) {
  if (name == "XY") return gp_Ax3(gp::Origin(), gp::DZ(), gp::DX());
  if (name == "XZ") return gp_Ax3(gp::Origin(), -gp::DY(), gp::DX());  // sketch y = world Z
  if (name == "YZ") return gp_Ax3(gp::Origin(), gp::DX(), gp::DY());   // sketch y = world Z
  throw FeatureError("unknown plane: " + name);
}

gp_Ax3 planeOfFace(const TopoDS_Face& face, const std::string& name) {
  BRepAdaptor_Surface surf(face);
  if (surf.GetType() != GeomAbs_Plane) throw FeatureError("face is not planar: " + name);
  gp_Ax3 ax = surf.Plane().Position();
  gp_Dir n = ax.Direction();
  if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
  // Origin: the world origin projected onto the plane, so it is stable as the face grows.
  const double signedDist = gp_Vec(ax.Location(), gp::Origin()).Dot(gp_Vec(n));
  const gp_Pnt o = gp::Origin().Translated(gp_Vec(n) * -signedDist);
  gp_Dir x = ax.XDirection();
  // Prefer a world axis as the sketch X when the plane contains one (tidier sketches).
  for (const gp_Dir& w : {gp::DX(), gp::DY(), gp::DZ()}) {
    if (std::abs(w.Dot(n)) < 1e-9) { x = w; break; }
  }
  return gp_Ax3(o, n, x);
}

gp_Ax3 resolvePlane(const json& spec, const Context& ctx) {
  if (spec.is_null()) return basePlane("XY");
  if (spec.is_string()) {
    const std::string s = spec.get<std::string>();
    if (s == "XY" || s == "XZ" || s == "YZ") return basePlane(s);
    if (auto it = ctx.datums.find(s); it != ctx.datums.end()) return it->second;
    if (hasFace(ctx.body, s)) return planeOfFace(findFace(ctx.body, s), s);
    throw FeatureError("plane not found: " + s);
  }
  if (spec.is_object()) {
    json base = spec.contains("base") ? spec.at("base") : json("XY");
    if (spec.contains("face")) base = spec.at("face");
    if (spec.contains("datum")) base = spec.at("datum");
    if (spec.contains("type") && !spec.contains("base")) base = spec.at("type");
    gp_Ax3 ax = resolvePlane(base, ctx);
    const double offset = num(spec, "offset", 0.0);
    if (offset != 0) ax.Translate(gp_Vec(ax.Direction()) * offset);
    if (spec.value("flip", false)) {
      ax.ZReverse();
      ax.XReverse();  // keep it right-handed with the same Y
    }
    return ax;
  }
  throw FeatureError("bad plane: " + spec.dump());
}

std::optional<gp_Dir> worldDir(const std::string& s) {
  if (s == "X") return gp::DX();
  if (s == "Y") return gp::DY();
  if (s == "Z") return gp::DZ();
  if (s == "-X") return -gp::DX();
  if (s == "-Y") return -gp::DY();
  if (s == "-Z") return -gp::DZ();
  return std::nullopt;
}

gp_Dir resolveDirection(const std::string& spec, const Context& ctx) {
  if (auto d = worldDir(spec)) return *d;
  if (auto it = ctx.datums.find(spec); it != ctx.datums.end()) return it->second.Direction();
  if (!ctx.body.findEdges(spec).empty()) {
    BRepAdaptor_Curve c(findEdge(ctx.body, spec));
    if (c.GetType() != GeomAbs_Line) throw FeatureError("edge is not straight: " + spec);
    return c.Line().Direction();
  }
  if (hasFace(ctx.body, spec)) return planeOfFace(findFace(ctx.body, spec), spec).Direction();
  throw FeatureError("direction not found: " + spec);
}

gp_Ax1 resolveAxis(const std::string& spec, const Context& ctx) {
  if (spec == "X" || spec == "Y" || spec == "Z") return gp_Ax1(gp::Origin(), *worldDir(spec));
  if (!ctx.body.findEdges(spec).empty()) {
    BRepAdaptor_Curve c(findEdge(ctx.body, spec));
    if (c.GetType() == GeomAbs_Line) return c.Line().Position();
    if (c.GetType() == GeomAbs_Circle) return c.Circle().Axis();
    throw FeatureError("edge is neither straight nor circular: " + spec);
  }
  if (hasFace(ctx.body, spec)) {
    BRepAdaptor_Surface s(findFace(ctx.body, spec));
    if (s.GetType() == GeomAbs_Cylinder) return s.Cylinder().Axis();
    if (s.GetType() == GeomAbs_Cone) return s.Cone().Axis();
    throw FeatureError("face has no axis: " + spec);
  }
  throw FeatureError("axis not found: " + spec);
}

gp_Trsf toPlane(const gp_Ax3& plane) {
  gp_Trsf t;
  t.SetDisplacement(gp_Ax3(), plane);
  return t;
}

gp_Pnt planePoint(const gp_Ax3& ax, double x, double y) {
  return ax.Location().Translated(gp_Vec(ax.XDirection()) * x + gp_Vec(ax.YDirection()) * y);
}

// --------------------------------------------------------------- sketches

const SketchSlot& placedSketch(const std::string& id, Context& ctx) {
  auto it = ctx.sketches.find(id);
  if (it == ctx.sketches.end()) throw FeatureError("sketch not found: " + id);
  SketchSlot& slot = it->second;
  switch (slot.solved.state) {
    case sketch::SolveState::Solved: break;
    case sketch::SolveState::Conflicting: {
      std::string ids;
      for (const auto& c : slot.solved.conflicting) ids += (ids.empty() ? "" : ", ") + c;
      throw FeatureError(id + " has conflicting constraints: " + ids);
    }
    case sketch::SolveState::Failed: throw FeatureError(id + " could not be solved");
    case sketch::SolveState::Invalid: throw FeatureError(id + " is invalid: " + slot.solved.message);
  }
  if (!slot.placement) slot.placement = resolvePlane(slot.planeSpec, ctx);
  return slot;
}

// The sketch's profile moved onto its plane, with its edges' entity names.
Profile placedProfile(const SketchSlot& slot) {
  Profile p;
  try {
    p = buildProfile(slot.solved.sketch);
  } catch (const ProfileError& e) {
    throw FeatureError(e.what());
  }
  const TopLoc_Location loc(toPlane(*slot.placement));
  p.shape = p.shape.Moved(loc);
  for (auto& f : p.faces) f = TopoDS::Face(f.Moved(loc));
  for (auto& [e, name] : p.edges) e = TopoDS::Edge(e.Moved(loc));
  return p;
}

std::string entityOf(const Profile& p, const TopoDS_Edge& e) {
  for (const auto& [pe, name] : p.edges) {
    if (pe.IsSame(e)) return name;
  }
  return {};
}

// Name for a cap face of a multi-region profile: the smallest entity name on it.
std::string capSuffix(const Profile& p, const TopoDS_Shape& capFace, const TopoDS_Shape& profileFace) {
  if (p.faces.size() <= 1) return "";
  std::string best;
  for (TopExp_Explorer ex(profileFace, TopAbs_EDGE); ex.More(); ex.Next()) {
    const std::string n = entityOf(p, TopoDS::Edge(ex.Current()));
    if (!n.empty() && (best.empty() || n < best)) best = n;
  }
  (void)capFace;
  return best.empty() ? "" : "(" + best + ")";
}

// Faces of a sweep (prism or revolution) of a profile: caps and sides.
template <class Sweep>
NamedShape nameSweep(Sweep& sweep, const Profile& profile, const std::string& id, bool caps) {
  std::vector<NamedFace> faces;
  if (caps) {
    for (const auto& pf : profile.faces) {
      const std::string suffix = capSuffix(profile, pf, pf);
      for (const TopoDS_Shape& s : {sweep.FirstShape(pf), sweep.LastShape(pf)}) {
        const bool first = s.IsSame(sweep.FirstShape(pf));
        for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
          faces.push_back({TopoDS::Face(ex.Current()), id + (first ? "/start" : "/end") + suffix});
        }
      }
    }
  }
  for (TopExp_Explorer ex(profile.shape, TopAbs_EDGE); ex.More(); ex.Next()) {
    const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
    const std::string entity = entityOf(profile, e);
    if (entity.empty()) throw FeatureError("internal: lost track of a sketch edge");
    for (const TopoDS_Shape& f : sweep.Generated(e)) {
      if (f.ShapeType() != TopAbs_FACE) continue;
      bool seen = false;
      for (const auto& nf : faces) seen = seen || nf.face.IsSame(f);
      if (!seen) faces.push_back({TopoDS::Face(f), id + "/side(" + entity + ")"});
    }
  }
  // OCCT's history misses some faces (a revolve does not report the planar faces
  // swept by edges square to the axis). A profile edge lies on the face it sweeps,
  // so its midpoint names any face still left; anything else gets a numbered name.
  int leftover = 0;
  for (TopExp_Explorer fx(sweep.Shape(), TopAbs_FACE); fx.More(); fx.Next()) {
    const TopoDS_Face face = TopoDS::Face(fx.Current());
    bool named = false;
    for (const auto& nf : faces) named = named || nf.face.IsSame(face);
    if (named) continue;
    std::string name;
    for (TopExp_Explorer ex(profile.shape, TopAbs_EDGE); ex.More() && name.empty(); ex.Next()) {
      const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
      BRepAdaptor_Curve c(e);
      const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
      BRepExtrema_DistShapeShape d(BRepBuilderAPI_MakeVertex(mid).Vertex(), face);
      if (d.IsDone() && d.Value() < 1e-6) name = id + "/side(" + entityOf(profile, e) + ")";
    }
    if (name.empty()) name = id + "/face" + std::to_string(++leftover);
    faces.push_back({face, name});
  }
  return NamedShape(sweep.Shape(), std::move(faces));
}

// ------------------------------------------------------------- booleans

std::string opOf(const json& f, const Context& ctx) {
  std::string op = str(f, "op", "add");
  if (op == "join" || op == "unite" || op == "union") op = "add";
  if (op == "subtract") op = "cut";
  if (op != "add" && op != "cut" && op != "intersect" && op != "new") throw FeatureError("unknown op: " + op);
  if (op == "new" && !ctx.body.empty()) throw FeatureError("the part already has a body; use add, cut or intersect");
  return op;
}

NamedShape applyBoolean(const NamedShape& body, const NamedShape& tool, const std::string& op,
                        const std::string& id) {
  if (tool.empty()) throw FeatureError("the feature made no geometry");
  if (body.empty()) {
    if (op == "cut" || op == "intersect") throw FeatureError("there is no body to " + op);
    return tool;
  }
  if (op == "new") throw FeatureError("the part already has a body");
  std::unique_ptr<BRepAlgoAPI_BooleanOperation> b;
  if (op == "add") b = std::make_unique<BRepAlgoAPI_Fuse>(body.shape(), tool.shape());
  else if (op == "cut") b = std::make_unique<BRepAlgoAPI_Cut>(body.shape(), tool.shape());
  else b = std::make_unique<BRepAlgoAPI_Common>(body.shape(), tool.shape());
  if (!b->IsDone() || b->HasErrors()) throw FeatureError("the " + op + " operation failed");
  const TopoDS_Shape result = b->Shape();
  if (solidCount(result) == 0) throw FeatureError("the " + op + " left nothing behind");
  return nameFromHistory(result, *b, {{&body, {}}, {&tool, {}}}, id);
}

void checkValid(const TopoDS_Shape& s, const char* what) {
  if (!BRepCheck_Analyzer(s).IsValid()) throw FeatureError(std::string(what) + " made an invalid solid");
}

// Combine several named tools into one compound (they keep their names).
NamedShape compoundOf(const std::vector<NamedShape>& parts) {
  BRep_Builder bb;
  TopoDS_Compound c;
  bb.MakeCompound(c);
  std::vector<NamedFace> faces;
  for (const auto& p : parts) {
    bb.Add(c, p.shape());
    for (const auto& f : p.faces()) faces.push_back(f);
  }
  return NamedShape(c, std::move(faces));
}

// ---------------------------------------------------------------- features

NamedShape extrudeTool(const json& f, Context& ctx, const std::string& op) {
  const std::string id = f.at("id");
  const SketchSlot& slot = placedSketch(str(f, "sketch"), ctx);
  const Profile profile = placedProfile(slot);
  const gp_Dir n = slot.placement->Direction();
  const std::string direction = str(f, "direction", "normal");
  const bool through = f.value("through", false);

  // How far the extrude reaches along +n (fwd) and -n (back) from the sketch plane.
  double fwd = 0, back = 0;
  if (through) {
    const double big = 2 * (sizeOf(ctx.body.shape()) + sizeOf(profile.shape)) + 10;
    if (direction == "symmetric" || direction == "twoSided") fwd = back = big;
    else if (direction == "reverse") back = big;
    else if (op == "cut") back = big;  // cutting through: into the material, behind the sketch face
    else fwd = big;
    if (f.value("flip", false)) std::swap(fwd, back);
  } else {
    const double d = num(f, "distance");
    if (std::abs(d) < 1e-6) throw FeatureError("distance must not be zero");
    if (direction == "normal") (d > 0 ? fwd : back) = std::abs(d);
    else if (direction == "reverse") (d > 0 ? back : fwd) = std::abs(d);
    else if (direction == "symmetric") fwd = back = std::abs(d) / 2;
    else if (direction == "twoSided") {
      const double d2 = num(f, "distance2", 0.0);
      if (d < 0 || d2 < 0) throw FeatureError("two-sided distances must not be negative");
      fwd = d;
      back = d2;
    } else throw FeatureError("unknown direction: " + direction);
  }
  const double start = -back, length = fwd + back;
  if (length < 1e-6) throw FeatureError("the extrude has no length");

  Profile moved = profile;
  if (std::abs(start) > 1e-12) {
    gp_Trsf t;
    t.SetTranslation(gp_Vec(n) * start);
    const TopLoc_Location loc(t);
    moved.shape = profile.shape.Moved(loc);
    for (auto& face : moved.faces) face = TopoDS::Face(face.Moved(loc));
    for (auto& [e, name] : moved.edges) e = TopoDS::Edge(e.Moved(loc));
  }
  BRepPrimAPI_MakePrism prism(moved.shape, gp_Vec(n) * length);
  prism.Build();
  if (!prism.IsDone()) throw FeatureError("extrude failed");
  NamedShape tool = nameSweep(prism, moved, id, true);
  // "start" is the cap on the sketch plane; an extrude that only goes backwards starts at its far end.
  if (fwd == 0 && back > 0) {
    std::vector<NamedFace> swapped;
    for (auto nf : tool.faces()) {
      if (nf.name.rfind(id + "/start", 0) == 0) nf.name = id + "/end" + nf.name.substr(id.size() + 6);
      else if (nf.name.rfind(id + "/end", 0) == 0) nf.name = id + "/start" + nf.name.substr(id.size() + 4);
      swapped.push_back(nf);
    }
    tool = NamedShape(tool.shape(), std::move(swapped));
  }
  return tool;
}

gp_Ax1 sketchAxis(const std::string& spec, const SketchSlot& slot, const Context& ctx) {
  const gp_Ax3& ax = *slot.placement;
  if (spec == "H" || spec == "xaxis") return gp_Ax1(ax.Location(), ax.XDirection());
  if (spec == "V" || spec == "yaxis") return gp_Ax1(ax.Location(), ax.YDirection());
  if (const sketch::Entity* e = slot.solved.sketch.find(spec)) {
    if (e->type != sketch::EntityType::Line) throw FeatureError("axis " + spec + " is not a line");
    const gp_Pnt a = planePoint(ax, e->a.x, e->a.y), b = planePoint(ax, e->b.x, e->b.y);
    if (a.Distance(b) < 1e-9) throw FeatureError("axis " + spec + " has zero length");
    return gp_Ax1(a, gp_Dir(gp_Vec(a, b)));
  }
  return resolveAxis(spec, ctx);
}

NamedShape revolveTool(const json& f, Context& ctx) {
  const std::string id = f.at("id");
  const SketchSlot& slot = placedSketch(str(f, "sketch"), ctx);
  const gp_Ax1 axis = sketchAxis(str(f, "axis", "V"), slot, ctx);
  double angle = num(f, "angle", 360.0);
  if (std::abs(angle) < 1e-6) throw FeatureError("angle must not be zero");
  if (std::abs(angle) > 360) throw FeatureError("angle must be at most 360 degrees");
  // A revolve uses only the non-construction loops; a construction axis line is ignored by the profile.
  const Profile profile = placedProfile(slot);
  const bool full = std::abs(std::abs(angle) - 360) < 1e-9;
  BRepPrimAPI_MakeRevol revol(profile.shape, axis, angle * kDeg);
  revol.Build();
  if (!revol.IsDone()) throw FeatureError("revolve failed (does the profile cross the axis?)");
  NamedShape tool = nameSweep(revol, profile, id, !full);
  checkValid(tool.shape(), "the revolve");
  return tool;
}

NamedShape holeTool(const json& f, Context& ctx) {
  const std::string id = f.at("id");
  const SketchSlot& slot = placedSketch(str(f, "sketch"), ctx);
  const gp_Ax3& ax = *slot.placement;
  const double dia = num(f, "diameter");
  if (dia <= 0) throw FeatureError("diameter must be positive");
  const bool through = f.value("through", false);
  const double depth = through ? 2 * (sizeOf(ctx.body.shape()) + 10) : num(f, "depth");
  if (depth <= 0) throw FeatureError("depth must be positive");
  const std::string form = str(f, "form", "simple");
  const double tipAngle = through ? 0.0 : num(f, "tipAngle", 0.0);
  const double r = dia / 2;

  // Half-section in local (radius, z) with z going into the material (down).
  struct P { double r, z; std::string part; };  // part names the edge from this point to the next
  std::vector<P> pts;
  if (form == "counterbore") {
    const double cbr = num(f, "cbDiameter") / 2, cbd = num(f, "cbDepth");
    if (cbr <= r) throw FeatureError("counterbore diameter must be larger than the hole");
    if (cbd <= 0 || cbd >= depth) throw FeatureError("counterbore depth must be between 0 and the hole depth");
    pts = {{0, 0, "top"}, {cbr, 0, "cbWall"}, {cbr, cbd, "cbFloor"}, {r, cbd, "wall"}};
  } else if (form == "countersink") {
    const double csr = num(f, "csDiameter") / 2, csa = num(f, "csAngle", 90.0);
    if (csr <= r) throw FeatureError("countersink diameter must be larger than the hole");
    if (csa <= 0 || csa >= 180) throw FeatureError("countersink angle must be between 0 and 180");
    const double csd = (csr - r) / std::tan(csa / 2 * kDeg);
    if (csd >= depth) throw FeatureError("the countersink is deeper than the hole");
    pts = {{0, 0, "top"}, {csr, 0, "countersink"}, {r, csd, "wall"}};
  } else if (form == "simple") {
    pts = {{0, 0, "top"}, {r, 0, "wall"}};
  } else {
    throw FeatureError("unknown hole form: " + form);
  }
  if (tipAngle > 0) {
    if (tipAngle >= 180) throw FeatureError("tip angle must be below 180");
    const double tip = r / std::tan(tipAngle / 2 * kDeg);
    pts.push_back({r, depth, "tip"});
    pts.push_back({0, depth + tip, "axis"});
  } else {
    pts.push_back({r, depth, "bottom"});
    pts.push_back({0, depth, "axis"});
  }

  // Build the section in the XZ plane (x = radius, z = -depth) and revolve about Z.
  // Edges share their vertices, so the wire keeps them as they are and each
  // edge can be traced to its part name.
  std::vector<TopoDS_Vertex> verts;
  for (const P& p : pts) verts.push_back(BRepBuilderAPI_MakeVertex(gp_Pnt(p.r, 0, -p.z)).Vertex());
  std::vector<std::pair<TopoDS_Edge, std::string>> edges;
  BRepBuilderAPI_MakeWire wire;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const std::size_t j = (i + 1) % pts.size();
    if (BRep_Tool::Pnt(verts[i]).Distance(BRep_Tool::Pnt(verts[j])) < 1e-9) continue;
    TopoDS_Edge e = BRepBuilderAPI_MakeEdge(verts[i], verts[j]).Edge();
    wire.Add(e);
    edges.emplace_back(e, pts[i].part);
  }
  if (!wire.IsDone()) throw FeatureError("could not build the hole section");
  const TopoDS_Face section = BRepBuilderAPI_MakeFace(wire.Wire(), true).Face();

  std::vector<NamedShape> tools;
  std::vector<std::string> pointIds;
  for (const auto& e : slot.solved.sketch.entities) {
    if (e.type != sketch::EntityType::Point) continue;
    pointIds.push_back(e.id);
    BRepPrimAPI_MakeRevol revol(section, gp_Ax1(gp::Origin(), gp::DZ()), 2 * std::numbers::pi);
    revol.Build();
    if (!revol.IsDone()) throw FeatureError("could not build the hole");
    // Name the faces from the section geometry: a cylinder by its radius, a
    // plane by its depth, a cone by where it sits. Every hole face is one of these.
    std::vector<NamedFace> faces;
    for (TopExp_Explorer fx(revol.Shape(), TopAbs_FACE); fx.More(); fx.Next()) {
      const TopoDS_Face face = TopoDS::Face(fx.Current());
      BRepAdaptor_Surface surf(face);
      GProp_GProps g;
      BRepGProp::SurfaceProperties(face, g);
      const double zc = -g.CentreOfMass().Z();  // depth of the face's centre
      std::string part;
      double best = 1e100;
      for (std::size_t i = 0; i < pts.size(); ++i) {
        const P& p0 = pts[i];
        const P& p1 = pts[(i + 1) % pts.size()];
        if (p0.part == "axis") continue;
        const bool vertical = std::abs(p0.r - p1.r) < 1e-9, horizontal = std::abs(p0.z - p1.z) < 1e-9;
        double score = 1e100;
        if (surf.GetType() == GeomAbs_Cylinder && vertical) score = std::abs(surf.Cylinder().Radius() - p0.r);
        else if (surf.GetType() == GeomAbs_Plane && horizontal) score = std::abs(zc - p0.z);
        else if (surf.GetType() == GeomAbs_Cone && !vertical && !horizontal) score = std::abs(zc - (p0.z + p1.z) / 2);
        if (score < best) { best = score; part = p0.part; }
      }
      if (part.empty()) part = "face";
      faces.push_back({face, id + "/" + part + "(" + e.id + ")"});
    }
    // Place: local Z up = sketch normal, drilled along -Z into the material.
    const gp_Pnt c = planePoint(ax, e.a.x, e.a.y);
    gp_Trsf t;
    t.SetDisplacement(gp_Ax3(), gp_Ax3(c, ax.Direction(), ax.XDirection()));
    NamedShape local(revol.Shape(), std::move(faces));
    tools.push_back(renameTransformed(local, revol.Shape().Moved(TopLoc_Location(t)), ""));
  }
  if (tools.empty()) throw FeatureError("sketch " + slot.solved.sketch.id + " has no points to place holes at");
  return compoundOf(tools);
}

NamedShape filletOrChamfer(const json& f, const NamedShape& body, bool chamfer) {
  const std::string id = f.at("id");
  const double size = chamfer ? num(f, "distance") : num(f, "radius");
  if (size <= 0) throw FeatureError(chamfer ? "distance must be positive" : "radius must be positive");
  if (body.empty()) throw FeatureError(std::string("there is no body to ") + (chamfer ? "chamfer" : "fillet"));
  const auto refs = stringList(f, "edges");
  if (refs.empty()) throw FeatureError("pick at least one edge");

  std::vector<std::pair<TopoDS_Edge, std::string>> picked;
  for (const auto& name : refs) picked.emplace_back(findEdge(body, name), name);

  std::unique_ptr<BRepFilletAPI_LocalOperation> maker;
  if (chamfer) {
    auto m = std::make_unique<BRepFilletAPI_MakeChamfer>(body.shape());
    for (const auto& [e, n] : picked) m->Add(size, e);
    maker = std::move(m);
  } else {
    auto m = std::make_unique<BRepFilletAPI_MakeFillet>(body.shape());
    for (const auto& [e, n] : picked) m->Add(size, e);
    maker = std::move(m);
  }
  maker->Build();
  if (!maker->IsDone()) {
    throw FeatureError(chamfer ? "chamfer failed (distance too large?)" : "fillet failed (radius too large?)");
  }
  return nameFromHistory(maker->Shape(), *maker, {{&body, picked}}, id);
}

NamedShape shell(const json& f, const NamedShape& body) {
  const std::string id = f.at("id");
  const double t = num(f, "thickness");
  if (t <= 0) throw FeatureError("thickness must be positive");
  if (body.empty()) throw FeatureError("there is no body to shell");
  TopTools_ListOfShape removed;
  for (const auto& name : stringList(f, "faces")) removed.Append(findFace(body, name));
  if (removed.IsEmpty()) throw FeatureError("pick at least one face to remove");
  BRepOffsetAPI_MakeThickSolid maker;
  maker.MakeThickSolidByJoin(body.shape(), removed, -t, 1e-3);
  maker.Build();
  if (!maker.IsDone()) throw FeatureError("shell failed (thickness too large for the part?)");
  return nameFromHistory(maker.Shape(), maker, {{&body, {}}}, id);
}

// A copy of a named tool moved by trsf, names prefixed.
NamedShape transformed(const NamedShape& src, const gp_Trsf& t, const std::string& prefix) {
  BRepBuilderAPI_Transform tr(src.shape(), t, true);
  if (!tr.IsDone()) throw FeatureError("could not transform the geometry");
  std::vector<NamedFace> faces;
  for (const auto& nf : src.faces()) {
    const TopoDS_Shape m = tr.ModifiedShape(nf.face);
    if (!m.IsNull()) faces.push_back({TopoDS::Face(m), prefix + nf.name});
  }
  return NamedShape(tr.Shape(), std::move(faces));
}

// Repeat features (or the whole body) once per transform.
NamedShape repeat(const json& f, Context& ctx, const std::vector<std::pair<gp_Trsf, std::string>>& copies,
                  bool keepOriginalBody = true) {
  const std::string id = f.at("id");
  if (ctx.body.empty()) throw FeatureError("there is no body");
  if (f.value("body", false)) {
    std::vector<NamedShape> parts;
    for (const auto& [t, tag] : copies) parts.push_back(transformed(ctx.body, t, id + tag + "/"));
    (void)keepOriginalBody;
    return applyBoolean(ctx.body, compoundOf(parts), "add", id);
  }
  const auto ids = stringList(f, "features");
  if (ids.empty()) throw FeatureError("pick at least one feature, or the body");
  NamedShape body = ctx.body;
  for (const auto& fid : ids) {
    auto it = ctx.tools.find(fid);
    if (it == ctx.tools.end()) {
      throw FeatureError("cannot repeat " + fid + ": only extrudes, revolves, holes and primitives can be patterned");
    }
    std::vector<NamedShape> parts;
    for (const auto& [t, tag] : copies) parts.push_back(transformed(it->second.tool, t, id + tag + "/"));
    std::string op = it->second.op == "new" ? "add" : it->second.op;
    body = applyBoolean(body, compoundOf(parts), op, id);
  }
  return body;
}

NamedShape linearPattern(const json& f, Context& ctx) {
  const gp_Dir d1 = resolveDirection(str(f, "direction", "X"), ctx);
  const int n1 = static_cast<int>(std::lround(num(f, "count")));
  const double s1 = num(f, "spacing");
  int n2 = 1;
  double s2 = 0;
  gp_Dir d2 = gp::DY();
  if (f.contains("direction2") && !f.at("direction2").is_null() && str(f, "direction2") != "") {
    d2 = resolveDirection(str(f, "direction2"), ctx);
    n2 = static_cast<int>(std::lround(num(f, "count2", 1.0)));
    s2 = num(f, "spacing2", 0.0);
  }
  if (n1 < 1 || n2 < 1) throw FeatureError("count must be at least 1");
  if (n1 * n2 > 1000) throw FeatureError("too many instances (limit 1000)");
  if (n1 * n2 == 1) throw FeatureError("a pattern needs at least two instances");
  std::vector<std::pair<gp_Trsf, std::string>> copies;
  for (int i = 0; i < n1; ++i) {
    for (int j = 0; j < n2; ++j) {
      if (i == 0 && j == 0) continue;
      gp_Trsf t;
      t.SetTranslation(gp_Vec(d1) * (s1 * i) + gp_Vec(d2) * (s2 * j));
      copies.emplace_back(t, n2 > 1 ? "[" + std::to_string(i) + "," + std::to_string(j) + "]"
                                     : "[" + std::to_string(i) + "]");
    }
  }
  return repeat(f, ctx, copies);
}

NamedShape circularPattern(const json& f, Context& ctx) {
  const gp_Ax1 axis = resolveAxis(str(f, "axis", "Z"), ctx);
  const int n = static_cast<int>(std::lround(num(f, "count")));
  const double total = num(f, "angle", 360.0);
  if (n < 2) throw FeatureError("a pattern needs at least two instances");
  if (n > 1000) throw FeatureError("too many instances (limit 1000)");
  const bool full = std::abs(std::abs(total) - 360) < 1e-9;
  const double step = full ? total / n : total / (n - 1);
  std::vector<std::pair<gp_Trsf, std::string>> copies;
  for (int i = 1; i < n; ++i) {
    gp_Trsf t;
    t.SetRotation(axis, step * i * kDeg);
    copies.emplace_back(t, "[" + std::to_string(i) + "]");
  }
  return repeat(f, ctx, copies);
}

NamedShape mirror(const json& f, Context& ctx) {
  const json spec = f.contains("plane") ? f.at("plane") : json("YZ");
  const gp_Ax3 pl = resolvePlane(spec, ctx);
  gp_Trsf t;
  t.SetMirror(gp_Ax2(pl.Location(), pl.Direction(), pl.XDirection()));
  return repeat(f, ctx, {{t, ""}});
}

gp_Ax3 primitiveFrame(const json& f) {
  const gp_Pnt p = pointOf(f, "position");
  const std::string axis = str(f, "axis", "Z");
  if (axis == "Z") return gp_Ax3(p, gp::DZ(), gp::DX());
  if (axis == "X") return gp_Ax3(p, gp::DX(), gp::DY());
  if (axis == "Y") return gp_Ax3(p, gp::DY(), gp::DZ());
  throw FeatureError("axis must be X, Y or Z");
}

// Names faces of a primitive by geometry relative to its frame.
NamedShape namePrimitive(const TopoDS_Shape& s, const gp_Ax3& frame, const std::string& id, bool box) {
  std::vector<NamedFace> faces;
  int other = 0;
  for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
    const TopoDS_Face face = TopoDS::Face(ex.Current());
    BRepAdaptor_Surface surf(face);
    std::string name;
    if (surf.GetType() == GeomAbs_Plane) {
      gp_Dir n = surf.Plane().Axis().Direction();
      if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
      if (box) {
        const gp_Dir X = frame.XDirection(), Y = frame.YDirection(), Z = frame.Direction();
        if (n.Dot(Z) < -0.5) name = "bottom";
        else if (n.Dot(Z) > 0.5) name = "top";
        else if (n.Dot(Y) < -0.5) name = "front";
        else if (n.Dot(Y) > 0.5) name = "back";
        else if (n.Dot(X) < -0.5) name = "left";
        else name = "right";
      } else {
        name = n.Dot(frame.Direction()) > 0 ? "top" : "bottom";
      }
    } else if (surf.GetType() == GeomAbs_Sphere) {
      name = "surface";
    } else if (surf.GetType() == GeomAbs_Cylinder || surf.GetType() == GeomAbs_Cone) {
      name = "side";
    } else {
      name = "face" + std::to_string(++other);
    }
    faces.push_back({face, id + "/" + name});
  }
  return NamedShape(s, std::move(faces));
}

NamedShape primitiveTool(const json& f, const std::string& type) {
  const std::string id = f.at("id");
  const gp_Ax3 frame = primitiveFrame(f);
  const gp_Ax2 ax2(frame.Location(), frame.Direction(), frame.XDirection());
  if (type == "box") {
    const double l = num(f, "length"), w = num(f, "width"), h = num(f, "height");
    if (l <= 0 || w <= 0 || h <= 0) throw FeatureError("box sizes must be positive");
    return namePrimitive(BRepPrimAPI_MakeBox(ax2, l, w, h).Shape(), frame, id, true);
  }
  if (type == "cylinder") {
    const double r = num(f, "radius"), h = num(f, "height");
    if (r <= 0 || h <= 0) throw FeatureError("radius and height must be positive");
    return namePrimitive(BRepPrimAPI_MakeCylinder(ax2, r, h).Shape(), frame, id, false);
  }
  if (type == "sphere") {
    const double r = num(f, "radius");
    if (r <= 0) throw FeatureError("radius must be positive");
    return namePrimitive(BRepPrimAPI_MakeSphere(ax2, r).Shape(), frame, id, false);
  }
  if (type == "cone") {
    const double r1 = num(f, "r1"), r2 = num(f, "r2"), h = num(f, "height");
    if (r1 < 0 || r2 < 0 || (r1 == 0 && r2 == 0) || h <= 0) throw FeatureError("bad cone sizes");
    if (std::abs(r1 - r2) < 1e-9) throw FeatureError("r1 and r2 are equal: use a cylinder");
    return namePrimitive(BRepPrimAPI_MakeCone(ax2, r1, r2, h).Shape(), frame, id, false);
  }
  throw FeatureError("unknown primitive " + type);
}

NamedShape importTool(const json& f) {
  const std::string id = f.at("id");
  const std::string text = str(f, "brep");
  if (text.empty()) throw FeatureError("the import holds no geometry");
  std::istringstream in(text);
  TopoDS_Shape shape;
  BRep_Builder bb;
  BRepTools::Read(shape, in, bb);
  if (shape.IsNull()) throw FeatureError("could not read the imported geometry");
  std::vector<NamedFace> faces;
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(shape, TopAbs_FACE, map);
  for (int i = 1; i <= map.Extent(); ++i) faces.push_back({TopoDS::Face(map(i)), id + "/face" + std::to_string(i)});
  return NamedShape(shape, std::move(faces));
}

void addSketch(const json& js, Context& ctx, RebuildResult& out, bool inTree) {
  SketchSlot slot;
  try {
    slot.solved = sketch::solve(sketch::fromJson(js));
  } catch (const std::exception& e) {
    slot.solved.state = sketch::SolveState::Invalid;
    slot.solved.sketch.id = js.value("id", std::string("?"));
    slot.solved.message = e.what();
  }
  slot.planeSpec = js.contains("plane") ? js.at("plane") : json(nullptr);
  if (inTree) {
    // A sketch in the tree is placed where it stands, so a face it sits on is the face as it is now.
    out.sketches.push_back(slot.solved);
    slot.placement = resolvePlane(slot.planeSpec, ctx);
    out.placements[slot.solved.sketch.id] = *slot.placement;
    ctx.sketches[slot.solved.sketch.id] = slot;
    switch (slot.solved.state) {
      case sketch::SolveState::Solved: return;
      case sketch::SolveState::Conflicting: {
        std::string ids;
        for (const auto& c : slot.solved.conflicting) ids += (ids.empty() ? "" : ", ") + c;
        throw FeatureError("conflicting constraints: " + ids);
      }
      case sketch::SolveState::Failed: throw FeatureError("the sketch could not be solved");
      case sketch::SolveState::Invalid: throw FeatureError(slot.solved.message);
    }
  } else {
    out.sketches.push_back(slot.solved);
    ctx.sketches[slot.solved.sketch.id] = slot;
  }
}

}  // namespace

RebuildResult rebuild(const json& rawDocument, const RebuildOptions& options) {
  RebuildResult out;
  Context ctx;

  // Expressions first: a copy of the document with every expression evaluated.
  json document = rawDocument;
  try {
    const ParamValues params = evaluateParameters(rawDocument.value("parameters", json::array()));
    document = resolveExpressions(rawDocument, params, &out.expressionErrors);
  } catch (const ExprError& e) {
    out.expressionErrors.push_back(e.what());
    document = resolveExpressions(rawDocument, {}, nullptr);
  }

  // Legacy sketches (outside the tree): solved now, placed when first used.
  if (document.contains("sketches")) {
    for (const auto& js : document.at("sketches")) addSketch(js, ctx, out, false);
  }

  bool failedUpstream = false;
  bool stopped = false;
  const json noFeatures = json::array();
  const json& featureList = document.contains("features") ? document.at("features") : noFeatures;
  for (const auto& feature : featureList) {
    FeatureStatus status;
    status.id = feature.value("id", std::string("?"));

    if (stopped) {
      status.state = FeatureState::Suppressed;
      status.message = "rolled back";
      out.features.push_back(status);
      continue;
    }
    if (feature.value("suppressed", false)) {
      status.state = FeatureState::Suppressed;
      status.message = "suppressed";
      out.features.push_back(status);
      if (status.id == options.stopAfter) stopped = true;
      continue;
    }
    if (failedUpstream) {
      status.state = FeatureState::Suppressed;
      status.message = "suppressed: an earlier feature failed";
      out.features.push_back(status);
      continue;
    }

    try {
      // A value of this feature (or of a sketch dimension in it) that would not evaluate.
      const std::string prefix = "features/" + status.id;
      for (const auto& err : out.expressionErrors) {
        if (err.rfind(prefix + ".", 0) == 0 || err.rfind(prefix + "/", 0) == 0) throw FeatureError(err.substr(prefix.size() + 1));
      }
      const std::string type = feature.at("type");
      if (status.id.empty() || status.id.find_first_of("|()/[] ") != std::string::npos) {
        throw FeatureError("invalid feature id: '" + status.id + "'");
      }
      if (type == "sketch") {
        addSketch(feature, ctx, out, true);
      } else if (type == "datumPlane") {
        const json base = feature.contains("base") ? feature.at("base") : json("XY");
        gp_Ax3 ax = resolvePlane(base, ctx);
        const double offset = num(feature, "offset", 0.0);
        if (offset != 0) ax.Translate(gp_Vec(ax.Direction()) * offset);
        if (feature.value("flip", false)) { ax.ZReverse(); ax.XReverse(); }
        ctx.datums[status.id] = ax;
        ctx.datumList.push_back({status.id, ax});
      } else if (type == "extrude" || type == "revolve" || type == "hole" || type == "box" ||
                 type == "cylinder" || type == "sphere" || type == "cone" || type == "import") {
        std::string op = type == "hole" ? "cut" : opOf(feature, ctx);
        NamedShape tool;
        if (type == "extrude") tool = extrudeTool(feature, ctx, op);
        else if (type == "revolve") tool = revolveTool(feature, ctx);
        else if (type == "hole") tool = holeTool(feature, ctx);
        else if (type == "import") tool = importTool(feature);
        else tool = primitiveTool(feature, type);
        ctx.body = applyBoolean(ctx.body, tool, op, status.id);
        ctx.tools[status.id] = {tool, op};
      } else if (type == "fillet" || type == "chamfer") {
        ctx.body = filletOrChamfer(feature, ctx.body, type == "chamfer");
      } else if (type == "shell") {
        ctx.body = shell(feature, ctx.body);
      } else if (type == "linearPattern") {
        ctx.body = linearPattern(feature, ctx);
      } else if (type == "circularPattern") {
        ctx.body = circularPattern(feature, ctx);
      } else if (type == "mirror") {
        ctx.body = mirror(feature, ctx);
      } else {
        throw FeatureError("unknown feature type: " + type);
      }
    } catch (const FeatureError& e) {
      status.state = FeatureState::Failed;
      status.message = e.what();
    } catch (const Standard_Failure& e) {
      status.state = FeatureState::Failed;
      status.message = std::string("geometry error: ") + e.GetMessageString();
    } catch (const json::exception& e) {
      status.state = FeatureState::Failed;
      status.message = std::string("bad feature definition: ") + e.what();
    } catch (const std::exception& e) {
      status.state = FeatureState::Failed;
      status.message = e.what();
    }
    if (status.state == FeatureState::Failed) failedUpstream = true;
    out.features.push_back(status);
    if (status.id == options.stopAfter) stopped = true;
  }

  // Legacy sketches that were used have placements now; give the rest theirs if possible.
  for (auto& [id, slot] : ctx.sketches) {
    if (!slot.placement) {
      try {
        slot.placement = resolvePlane(slot.planeSpec, ctx);
      } catch (const std::exception&) {
        continue;
      }
    }
    out.placements[id] = *slot.placement;
  }
  out.datums = ctx.datumList;
  out.body = ctx.body;
  return out;
}

}  // namespace cadx
