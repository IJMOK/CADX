#include "history.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Surface.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt2d.hxx>

#include <optional>

namespace cadx {

namespace {

bool contains(const TopTools_ListOfShape& list, const TopoDS_Shape& s) {
  for (const TopoDS_Shape& x : list) {
    if (x.IsSame(s)) return true;
  }
  return false;
}

// Some point strictly inside the face (not just on its untrimmed surface).
std::optional<gp_Pnt> interiorPoint(const TopoDS_Face& face) {
  double u0, u1, v0, v1;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
  if (surf.IsNull()) return std::nullopt;
  BRepTopAdaptor_FClass2d cls(face, 1e-7);
  constexpr int n = 7;
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      // Centre first, then spread out.
      const double fu = 0.5 + ((i % 2 ? 1 : -1) * ((i + 1) / 2)) / double(n + 1);
      const double fv = 0.5 + ((j % 2 ? 1 : -1) * ((j + 1) / 2)) / double(n + 1);
      const gp_Pnt2d uv(u0 + (u1 - u0) * fu, v0 + (v1 - v0) * fv);
      if (cls.Perform(uv) == TopAbs_IN) return surf->Value(uv.X(), uv.Y());
    }
  }
  return std::nullopt;
}

}  // namespace

bool faceLiesOn(const TopoDS_Face& face, const TopoDS_Face& onto, double tol) {
  try {
    if (BRepAdaptor_Surface(face).GetType() != BRepAdaptor_Surface(onto).GetType()) return false;
    const auto p = interiorPoint(face);
    if (!p) return false;
    BRepExtrema_DistShapeShape d(BRepBuilderAPI_MakeVertex(*p).Vertex(), onto);
    return d.IsDone() && d.Value() < tol;
  } catch (const Standard_Failure&) {
    return false;
  }
}

NamedShape nameFromHistory(const TopoDS_Shape& result, BRepBuilderAPI_MakeShape& op,
                           const std::vector<HistoryInput>& inputs, const std::string& featureId) {
  // Sub-shapes of every input, mapped once.
  struct Mapped {
    const HistoryInput* in;
    TopTools_IndexedMapOfShape edges, vertices;
  };
  std::vector<Mapped> mapped;
  for (const auto& in : inputs) {
    if (!in.shape || in.shape->empty()) continue;
    Mapped m{&in, {}, {}};
    TopExp::MapShapes(in.shape->shape(), TopAbs_EDGE, m.edges);
    TopExp::MapShapes(in.shape->shape(), TopAbs_VERTEX, m.vertices);
    mapped.push_back(std::move(m));
  }

  std::vector<NamedFace> faces;
  std::vector<TopoDS_Face> leftovers;
  TopTools_IndexedMapOfShape resultFaces;
  TopExp::MapShapes(result, TopAbs_FACE, resultFaces);

  for (int i = 1; i <= resultFaces.Extent(); ++i) {
    const TopoDS_Face rf = TopoDS::Face(resultFaces(i));
    std::optional<std::string> name;

    // 1. Unchanged.
    for (const auto& m : mapped) {
      if (auto n = m.in->shape->nameOf(rf)) { name = n; break; }
    }
    // 2. Modified.
    if (!name) {
      std::optional<std::string> offsetName;
      for (const auto& m : mapped) {
        for (const auto& nf : m.in->shape->faces()) {
          if (!contains(op.Modified(nf.face), rf)) continue;
          if (faceLiesOn(rf, nf.face)) { name = nf.name; break; }
          if (!offsetName) offsetName = featureId + "/offset(" + nf.name + ")";
        }
        if (name) break;
      }
      if (!name) name = offsetName;
    }
    // 3. Generated from an edge: priority edges, then all.
    if (!name) {
      for (const auto& m : mapped) {
        for (const auto& [edge, edgeName] : m.in->priorityEdges) {
          if (contains(op.Generated(edge), rf)) { name = featureId + "/blend(" + edgeName + ")"; break; }
        }
        if (name) break;
      }
    }
    if (!name) {
      for (const auto& m : mapped) {
        for (int k = 1; k <= m.edges.Extent() && !name; ++k) {
          const TopoDS_Edge e = TopoDS::Edge(m.edges(k));
          if (contains(op.Generated(e), rf)) {
            if (auto en = m.in->shape->edgeName(e)) name = featureId + "/blend(" + *en + ")";
          }
        }
        if (name) break;
      }
    }
    // 4. Generated from a vertex.
    if (!name) {
      for (const auto& m : mapped) {
        for (int k = 1; k <= m.vertices.Extent() && !name; ++k) {
          const TopoDS_Vertex v = TopoDS::Vertex(m.vertices(k));
          if (contains(op.Generated(v), rf)) {
            if (auto vn = m.in->shape->vertexName(v)) name = featureId + "/corner(" + *vn + ")";
          }
        }
        if (name) break;
      }
    }
    // 5. Generated from a face.
    if (!name) {
      for (const auto& m : mapped) {
        for (const auto& nf : m.in->shape->faces()) {
          if (contains(op.Generated(nf.face), rf)) { name = featureId + "/from(" + nf.name + ")"; break; }
        }
        if (name) break;
      }
    }
    // 6. Geometric fallback.
    if (!name) {
      for (const auto& m : mapped) {
        for (const auto& nf : m.in->shape->faces()) {
          if (faceLiesOn(rf, nf.face)) { name = nf.name; break; }
        }
        if (name) break;
      }
    }
    if (name) faces.push_back({rf, *name});
    else leftovers.push_back(rf);
  }
  // 7. Leftovers.
  for (std::size_t k = 0; k < leftovers.size(); ++k) {
    faces.push_back({leftovers[k], featureId + "/face" + std::to_string(k + 1)});
  }
  return NamedShape(result, std::move(faces));
}

NamedShape renameTransformed(const NamedShape& source, const TopoDS_Shape& moved, const std::string& prefix) {
  TopTools_IndexedMapOfShape before, after;
  TopExp::MapShapes(source.shape(), TopAbs_FACE, before);
  TopExp::MapShapes(moved, TopAbs_FACE, after);
  std::vector<NamedFace> faces;
  for (int i = 1; i <= after.Extent() && i <= before.Extent(); ++i) {
    auto n = source.nameOf(TopoDS::Face(before(i)));
    faces.push_back({TopoDS::Face(after(i)), prefix + n.value_or("face" + std::to_string(i))});
  }
  return NamedShape(moved, std::move(faces));
}

}  // namespace cadx
