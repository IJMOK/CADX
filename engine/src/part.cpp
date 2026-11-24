#include "part.hpp"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <map>
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

struct FeatureError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

bool contains(const TopTools_ListOfShape& list, const TopoDS_Shape& s) {
  for (const TopoDS_Shape& x : list) {
    if (x.IsSame(s)) return true;
  }
  return false;
}

// ---------------------------------------------------------------- extrude

NamedShape extrude(const json& feature, const json& document) {
  const std::string id = feature.at("id");
  const std::string sketchId = feature.at("sketch");
  const double distance = feature.at("distance");
  if (std::abs(distance) < 1e-6) throw FeatureError("distance must not be zero");

  const json* sketch = nullptr;
  if (document.contains("sketches")) {
    for (const auto& s : document.at("sketches")) {
      if (s.at("id") == sketchId) sketch = &s;
    }
  }
  if (!sketch) throw FeatureError("sketch not found: " + sketchId);

  // Shared vertices, so the wire keeps our edges unchanged and each edge can
  // be traced to the segment that made it.
  std::map<std::pair<long long, long long>, TopoDS_Vertex> vertices;
  auto vertexAt = [&](const json& p) {
    const double x = p.at(0), y = p.at(1);
    const auto key = std::make_pair(std::llround(x * 1e6), std::llround(y * 1e6));
    auto it = vertices.find(key);
    if (it == vertices.end()) {
      it = vertices.emplace(key, BRepBuilderAPI_MakeVertex(gp_Pnt(x, y, 0)).Vertex()).first;
    }
    return it->second;
  };

  BRepBuilderAPI_MakeWire wireMaker;
  std::vector<std::pair<TopoDS_Edge, std::string>> segmentEdges;
  for (const auto& seg : sketch->at("segments")) {
    const std::string segId = seg.at("id");
    TopoDS_Vertex a = vertexAt(seg.at("a")), b = vertexAt(seg.at("b"));
    if (a.IsSame(b)) throw FeatureError("segment " + segId + " has zero length");
    TopoDS_Edge e = BRepBuilderAPI_MakeEdge(a, b).Edge();
    wireMaker.Add(e);
    if (!wireMaker.IsDone()) throw FeatureError("segment " + segId + " does not connect to the loop");
    segmentEdges.emplace_back(e, sketchId + "/" + segId);
  }
  const TopoDS_Wire wire = wireMaker.Wire();
  if (!wire.Closed()) throw FeatureError("sketch " + sketchId + " is not a closed loop");

  BRepBuilderAPI_MakeFace faceMaker(wire, true);
  if (!faceMaker.IsDone()) throw FeatureError("could not make a face from " + sketchId);
  const TopoDS_Face base = faceMaker.Face();

  BRepPrimAPI_MakePrism prism(base, gp_Vec(0, 0, distance));
  prism.Build();
  if (!prism.IsDone()) throw FeatureError("extrude failed");

  std::vector<NamedFace> faces;
  faces.push_back({TopoDS::Face(prism.FirstShape()), id + "/start"});
  faces.push_back({TopoDS::Face(prism.LastShape()), id + "/end"});

  // Side faces come from the sketch edges. The wire may hold the edges with a
  // different orientation, so match each wire edge back to its segment.
  for (TopExp_Explorer ex(base, TopAbs_EDGE); ex.More(); ex.Next()) {
    const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
    std::string segName;
    for (const auto& [se, name] : segmentEdges) {
      if (se.IsSame(e)) segName = name;
    }
    if (segName.empty()) throw FeatureError("internal: lost track of a sketch edge");
    const TopTools_ListOfShape& gen = prism.Generated(e);
    if (gen.IsEmpty()) throw FeatureError("internal: no side face for " + segName);
    for (const TopoDS_Shape& f : gen) {
      if (f.ShapeType() == TopAbs_FACE) faces.push_back({TopoDS::Face(f), id + "/side(" + segName + ")"});
    }
  }
  return NamedShape(prism.Shape(), std::move(faces));
}

// ----------------------------------------------------------------- fillet

NamedShape fillet(const json& feature, const NamedShape& input) {
  const std::string id = feature.at("id");
  const double radius = feature.at("radius");
  if (radius <= 0) throw FeatureError("radius must be positive");
  if (input.empty()) throw FeatureError("there is no body to fillet");

  BRepFilletAPI_MakeFillet maker(input.shape());
  std::vector<std::pair<TopoDS_Edge, std::string>> picked;
  for (const auto& ref : feature.at("edges")) {
    const std::string name = ref;
    auto found = input.findEdges(name);
    if (found.empty()) throw FeatureError("edge not found: " + name);
    if (found.size() > 1) throw FeatureError("edge name is ambiguous: " + name);
    maker.Add(radius, found.front());
    picked.emplace_back(found.front(), name);
  }

  maker.Build();
  if (!maker.IsDone()) throw FeatureError("fillet failed (radius too large?)");
  const TopoDS_Shape result = maker.Shape();

  // Old faces, and the vertices that may grow corner patches.
  TopTools_IndexedMapOfShape oldVertices, oldEdges;
  TopExp::MapShapes(input.shape(), TopAbs_VERTEX, oldVertices);
  TopExp::MapShapes(input.shape(), TopAbs_EDGE, oldEdges);

  std::vector<NamedFace> faces;
  TopTools_IndexedMapOfShape resultFaces;
  TopExp::MapShapes(result, TopAbs_FACE, resultFaces);
  for (int i = 1; i <= resultFaces.Extent(); ++i) {
    const TopoDS_Face rf = TopoDS::Face(resultFaces(i));
    std::optional<std::string> name;

    // 1. Unchanged or modified input face keeps its name.
    for (const auto& nf : input.faces()) {
      if (nf.face.IsSame(rf) || contains(maker.Modified(nf.face), rf)) {
        name = nf.name;
        break;
      }
    }
    // 2. Blend face generated from a filleted edge. The fillet follows
    //    tangent chains, so the edge may be one the user did not pick.
    if (!name) {
      for (const auto& [edge, edgeName] : picked) {
        if (contains(maker.Generated(edge), rf)) {
          name = id + "/blend(" + edgeName + ")";
          break;
        }
      }
    }
    if (!name) {
      for (int k = 1; k <= oldEdges.Extent() && !name; ++k) {
        const TopoDS_Edge e = TopoDS::Edge(oldEdges(k));
        if (contains(maker.Generated(e), rf)) {
          if (auto en = input.edgeName(e)) name = id + "/blend(" + *en + ")";
        }
      }
    }
    // 3. Corner patch generated from a vertex.
    if (!name) {
      for (int v = 1; v <= oldVertices.Extent() && !name; ++v) {
        const TopoDS_Vertex vx = TopoDS::Vertex(oldVertices(v));
        if (contains(maker.Generated(vx), rf)) {
          if (auto vn = input.vertexName(vx)) name = id + "/corner(" + *vn + ")";
        }
      }
    }
    if (!name) throw FeatureError("internal: could not name a face produced by the fillet");
    faces.push_back({rf, *name});
  }
  return NamedShape(result, std::move(faces));
}

}  // namespace

RebuildResult rebuild(const json& document) {
  RebuildResult out;
  bool failedUpstream = false;

  const json noFeatures = json::array();
  const json& featureList = document.contains("features") ? document.at("features") : noFeatures;
  for (const auto& feature : featureList) {
    FeatureStatus status;
    status.id = feature.value("id", std::string("?"));

    if (failedUpstream) {
      status.state = FeatureState::Suppressed;
      status.message = "suppressed: an earlier feature failed";
      out.features.push_back(status);
      continue;
    }

    try {
      const std::string type = feature.at("type");
      NamedShape next;
      if (type == "extrude") {
        if (!out.body.empty()) throw FeatureError("only one extrude (the base body) is supported so far");
        next = extrude(feature, document);
      } else if (type == "fillet") {
        next = fillet(feature, out.body);
      } else {
        throw FeatureError("unknown feature type: " + type);
      }
      out.body = std::move(next);
    } catch (const FeatureError& e) {
      status.state = FeatureState::Failed;
      status.message = e.what();
    } catch (const Standard_Failure& e) {
      status.state = FeatureState::Failed;
      status.message = std::string("geometry error: ") + e.GetMessageString();
    } catch (const json::exception& e) {
      status.state = FeatureState::Failed;
      status.message = std::string("bad feature definition: ") + e.what();
    }
    if (status.state == FeatureState::Failed) failedUpstream = true;
    out.features.push_back(status);
  }
  return out;
}

}  // namespace cadx
