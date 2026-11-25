#include "naming.hpp"

#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <set>

namespace cadx {

namespace {

std::optional<std::string> joinAdjacent(const NamedShape& ns,
                                        const TopTools_ListOfShape& faces) {
  std::set<std::string> names;  // sorted, unique
  int count = 0;
  for (const TopoDS_Shape& f : faces) {
    auto n = ns.nameOf(TopoDS::Face(f));
    if (!n) return std::nullopt;
    names.insert(*n);
    ++count;
  }
  if (names.empty()) return std::nullopt;
  std::string out;
  for (const auto& n : names) {
    if (!out.empty()) out += '|';
    out += n;
  }
  // An edge with a single adjacent face (a seam, or an open boundary) gets a
  // marker so it cannot collide with a face name or a real two-face edge.
  if (count == 1) out += "|seam";
  return out;
}

}  // namespace

std::optional<std::string> NamedShape::nameOf(const TopoDS_Face& face) const {
  for (const auto& nf : faces_) {
    if (nf.face.IsSame(face)) return nf.name;
  }
  return std::nullopt;
}

std::optional<std::string> NamedShape::edgeName(const TopoDS_Edge& edge) const {
  TopTools_IndexedDataMapOfShapeListOfShape map;
  TopExp::MapShapesAndUniqueAncestors(shape_, TopAbs_EDGE, TopAbs_FACE, map);
  const int i = map.FindIndex(edge);
  if (i == 0) return std::nullopt;
  return joinAdjacent(*this, map(i));
}

std::optional<std::string> NamedShape::vertexName(const TopoDS_Vertex& vertex) const {
  TopTools_IndexedDataMapOfShapeListOfShape map;
  TopExp::MapShapesAndUniqueAncestors(shape_, TopAbs_VERTEX, TopAbs_FACE, map);
  const int i = map.FindIndex(vertex);
  if (i == 0) return std::nullopt;
  return joinAdjacent(*this, map(i));
}

std::vector<TopoDS_Edge> NamedShape::findEdges(const std::string& name) const {
  std::vector<TopoDS_Edge> out;
  TopTools_IndexedDataMapOfShapeListOfShape map;
  TopExp::MapShapesAndUniqueAncestors(shape_, TopAbs_EDGE, TopAbs_FACE, map);
  for (int i = 1; i <= map.Extent(); ++i) {
    auto n = joinAdjacent(*this, map(i));
    if (n && *n == name) out.push_back(TopoDS::Edge(map.FindKey(i)));
  }
  return out;
}

std::vector<std::string> NamedShape::edgeNamesIndexed() const {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape_, TopAbs_EDGE, edges);
  TopTools_IndexedDataMapOfShapeListOfShape map;
  TopExp::MapShapesAndUniqueAncestors(shape_, TopAbs_EDGE, TopAbs_FACE, map);
  std::vector<std::string> out;
  out.reserve(edges.Extent());
  for (int i = 1; i <= edges.Extent(); ++i) {
    const int k = map.FindIndex(edges(i));
    auto n = k ? joinAdjacent(*this, map(k)) : std::nullopt;
    out.push_back(n.value_or(""));
  }
  return out;
}

std::vector<std::string> NamedShape::edgeNames() const {
  std::vector<std::string> out;
  TopTools_IndexedDataMapOfShapeListOfShape map;
  TopExp::MapShapesAndUniqueAncestors(shape_, TopAbs_EDGE, TopAbs_FACE, map);
  for (int i = 1; i <= map.Extent(); ++i) {
    if (auto n = joinAdjacent(*this, map(i))) out.push_back(*n);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace cadx
