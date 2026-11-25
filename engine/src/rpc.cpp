#include "rpc.hpp"

#include "expr.hpp"
#include "io.hpp"
#include "mesh.hpp"
#include "part.hpp"
#include "sketch/sketch.hpp"

#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <Bnd_Box.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

using nlohmann::json;

namespace cadx {

namespace {

constexpr const char* kVersion = "0.2.0";

json meshToJson(const Mesh& m) {
  return json{{"positions", m.positions},
              {"indices", m.indices},
              {"faceIds", m.faceIds},
              {"edges", m.edges},
              {"edgeIds", m.edgeIds}};
}

json vec(const gp_XYZ& v) { return json::array({v.X(), v.Y(), v.Z()}); }

json axes(const gp_Ax3& ax) {
  return json{{"origin", vec(ax.Location().XYZ())},
              {"xDir", vec(ax.XDirection().XYZ())},
              {"yDir", vec(ax.YDirection().XYZ())},
              {"normal", vec(ax.Direction().XYZ())}};
}

// Mesh tolerance relative to part size: fine for small parts, light for big ones.
double deflectionFor(const TopoDS_Shape& s) {
  Bnd_Box box;
  BRepBndLib::Add(s, box);
  if (box.IsVoid()) return 0.1;
  return std::clamp(std::sqrt(box.SquareExtent()) * 0.0008, 0.005, 2.0);
}

json resultJson(const RebuildResult& r) {
  json features = json::array();
  for (const auto& f : r.features) {
    features.push_back({{"id", f.id}, {"state", toString(f.state)}, {"message", f.message}});
  }
  json sketches = json::array();
  for (const auto& sk : r.sketches) {
    json js = sketch::toJson(sk);
    if (auto it = r.placements.find(sk.sketch.id); it != r.placements.end()) js["placement"] = axes(it->second);
    sketches.push_back(js);
  }
  json datums = json::array();
  for (const auto& d : r.datums) {
    json jd = axes(d.plane);
    jd["id"] = d.id;
    datums.push_back(jd);
  }
  json out{{"features", features},
           {"sketches", sketches},
           {"datums", datums},
           {"expressionErrors", r.expressionErrors}};
  if (!r.body.empty()) {
    out["mesh"] = meshToJson(tessellate(r.body.shape(), deflectionFor(r.body.shape()), 0.3));
    // faceNames[i] names the face with faceId i (same TopExp::MapShapes order as the mesh).
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(r.body.shape(), TopAbs_FACE, faces);
    json names = json::array();
    for (int i = 1; i <= faces.Extent(); ++i) names.push_back(r.body.nameOf(TopoDS::Face(faces(i))).value_or("?"));
    out["faceNames"] = names;
    out["edgeNames"] = r.body.edgeNamesIndexed();
  }
  return out;
}

// Large imported geometry stays in the engine; the UI gets its size.
json slimDocument(const json& doc) {
  json out = doc;
  for (auto& f : out["features"]) {
    if (f.contains("brep") && f.at("brep").is_string()) {
      f["brepBytes"] = f.at("brep").get<std::string>().size();
      f.erase("brep");
    }
  }
  return out;
}

std::string fileStem(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const auto dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
  return name;
}

RebuildResult rebuildCurrent(const json& doc, const std::string& rollback) {
  RebuildOptions opt;
  opt.stopAfter = rollback;
  return rebuild(doc, opt);
}

}  // namespace

json Session::state(const json* overrideDoc) {
  const json& doc = overrideDoc ? *overrideDoc : doc_.data();
  if (!rollback_.empty() && doc.contains("features")) {
    bool found = false;
    for (const auto& f : doc.at("features")) found = found || f.value("id", std::string()) == rollback_;
    if (!found) rollback_.clear();
  }
  const RebuildResult r = rebuildCurrent(doc, rollback_);
  json out = resultJson(r);
  out["revision"] = doc_.revision();
  out["name"] = doc.value("name", std::string("Part1"));
  out["document"] = slimDocument(doc);
  out["canUndo"] = doc_.canUndo();
  out["undoLabel"] = doc_.undoLabel();
  out["canRedo"] = doc_.canRedo();
  out["redoLabel"] = doc_.redoLabel();
  out["rollback"] = rollback_.empty() ? json(nullptr) : json(rollback_);
  json values = json::object();
  try {
    for (const auto& [k, v] : evaluateParameters(doc.value("parameters", json::array()))) values[k] = v;
  } catch (const ExprError&) {
    // reported through expressionErrors
  }
  out["parameterValues"] = values;
  return out;
}

json Session::handle(const std::string& method, const json& params) {
  // ---- queries
  if (method == "ping") return json{{"engine", "cadx"}, {"version", kVersion}};
  if (method == "getState") return state();
  if (method == "getDocument") return json{{"document", doc_.data()}, {"revision", doc_.revision()}};
  if (method == "solveSketch") {
    // Dimensions may be expressions over the document's parameters.
    ParamValues values;
    try {
      values = evaluateParameters(doc_.data().value("parameters", json::array()));
    } catch (const ExprError&) {
    }
    std::vector<std::string> errors;
    const json resolved = resolveExpressions(json{{"sketches", json::array({params.at("sketch")})}}, values, &errors);
    if (!errors.empty()) {
      sketch::SolveResult bad;
      bad.state = sketch::SolveState::Invalid;
      bad.sketch = sketch::fromJson(json{{"id", params.at("sketch").value("id", std::string("?"))}});
      bad.message = errors.front();
      return sketch::toJson(bad);
    }
    return sketch::toJson(sketch::solve(sketch::fromJson(resolved.at("sketches").at(0))));
  }
  if (method == "preview") {
    json feature = params.at("feature");
    if (feature.contains("id") && feature.at("id").is_null()) feature.erase("id");
    std::string after = params.value("after", std::string());
    const bool isNew = !feature.contains("id") || doc_.indexOf(feature.at("id").get<std::string>()) < 0;
    if (!feature.contains("id")) feature["id"] = doc_.uniqueId(idPrefixFor(feature.value("type", std::string())));
    if (isNew && after.empty() && !rollback_.empty()) after = rollback_;
    const json next = doc_.withFeature(feature, after);
    // While rolled back, show the model up to the new feature.
    const std::string saved = rollback_;
    if (isNew && !rollback_.empty()) rollback_ = feature.at("id").get<std::string>();
    json out = state(&next);
    rollback_ = saved;
    out["id"] = feature.at("id");
    return out;
  }
  if (method == "rebuild") {
    // Stateless: rebuild the given document (tests, tools, the Phase 0 protocol).
    return resultJson(rebuild(params.at("document")));
  }
  if (method == "measure") {
    return measure(rebuildCurrent(doc_.data(), rollback_).body.shape());
  }
  if (method == "exportFile") {
    const std::string path = params.at("path");
    std::string format = params.value("format", std::string());
    if (format.empty()) {
      const auto dot = path.find_last_of('.');
      format = dot == std::string::npos ? "step" : path.substr(dot + 1);
      std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    }
    const RebuildResult r = rebuildCurrent(doc_.data(), rollback_);
    const TopoDS_Shape& s = r.body.shape();
    if (format == "step" || format == "stp") exportStep(s, path, doc_.data().value("name", std::string("Part1")));
    else if (format == "iges" || format == "igs") exportIges(s, path);
    else if (format == "stl") exportStl(s, path, params.value("deflection", deflectionFor(s) * 0.5));
    else if (format == "brep" || format == "brp") exportBrep(s, path);
    else throw std::runtime_error("unknown export format: " + format);
    return json{{"path", path}, {"format", format}};
  }

  // ---- Phase 0 primitives, kept for the smoke test
  if (method == "makeBox") {
    return meshToJson(tessellate(
        BRepPrimAPI_MakeBox(params.value("x", 40.0), params.value("y", 20.0), params.value("z", 10.0)).Shape()));
  }
  if (method == "makeCylinder") {
    return meshToJson(tessellate(BRepPrimAPI_MakeCylinder(params.value("r", 10.0), params.value("h", 30.0)).Shape()));
  }

  // ---- document changes
  if (method == "newDocument") {
    doc_.reset(params.value("name", std::string("Part1")));
    rollback_.clear();
    return state();
  }
  if (method == "openDocument") {
    doc_.load(params.at("document"));
    rollback_.clear();
    return state();
  }
  if (method == "addFeature") {
    json feature = params.at("feature");
    std::string after = params.value("after", std::string());
    // While rolled back, new features go in at the rollback point, like NX.
    if (after.empty() && !rollback_.empty()) after = rollback_;
    const std::string id = doc_.addFeature(feature, after);
    if (!rollback_.empty()) rollback_ = id;
    json out = state();
    out["id"] = id;
    return out;
  }
  if (method == "editFeature") {
    json patch = params.at("feature");
    if (patch.is_object()) patch.erase("id");
    doc_.editFeature(params.at("id"), patch);
    return state();
  }
  if (method == "deleteFeature") {
    const std::string id = params.at("id");
    if (rollback_ == id) {
      const int i = doc_.indexOf(id);
      rollback_ = i > 0 ? doc_.data()["features"][i - 1].value("id", std::string()) : "";
    }
    doc_.deleteFeature(id);
    return state();
  }
  if (method == "moveFeature") {
    doc_.moveFeature(params.at("id"), params.at("index"));
    return state();
  }
  if (method == "setParameters") {
    doc_.setParameters(params.at("parameters"));
    return state();
  }
  if (method == "setName") {
    doc_.setName(params.at("name"));
    return state();
  }
  if (method == "setRollback") {
    const json& id = params.value("id", json(nullptr));
    if (id.is_null()) rollback_.clear();
    else {
      if (doc_.indexOf(id.get<std::string>()) < 0) throw std::runtime_error("no feature " + id.get<std::string>());
      rollback_ = id.get<std::string>();
    }
    return state();
  }
  if (method == "undo") {
    doc_.undo();
    return state();
  }
  if (method == "redo") {
    doc_.redo();
    return state();
  }
  if (method == "importFile") {
    const std::string path = params.at("path");
    const TopoDS_Shape shape = importShape(path);
    json feature{{"type", "import"},
                 {"name", fileStem(path)},
                 {"source", path},
                 {"brep", toBrepText(shape)},
                 {"op", params.value("op", std::string("add"))}};
    const std::string id = doc_.addFeature(feature, rollback_);
    if (!rollback_.empty()) rollback_ = id;
    json out = state();
    out["id"] = id;
    return out;
  }

  throw MethodNotFound("method not found: " + method);
}

}  // namespace cadx
