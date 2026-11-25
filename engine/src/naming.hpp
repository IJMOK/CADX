#pragma once
// Persistent naming: every face carries a name derived from the feature
// history that created it. Edges and vertices are named from the faces
// around them, so they survive any rebuild that keeps those face names.
//
//   face   : "Extrude1/side(Sketch1/L1)", "Extrude1/end", "Fillet1/blend(<edge>)"
//   edge   : "<faceA>|<faceB>"            (adjacent face names, sorted)
//   vertex : "<faceA>|<faceB>|<faceC>"    (adjacent face names, sorted)

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>

#include <optional>
#include <string>
#include <vector>

namespace cadx {

struct NamedFace {
  TopoDS_Face face;
  std::string name;
};

class NamedShape {
 public:
  NamedShape() = default;
  NamedShape(TopoDS_Shape shape, std::vector<NamedFace> faces)
      : shape_(std::move(shape)), faces_(std::move(faces)) {}

  const TopoDS_Shape& shape() const { return shape_; }
  const std::vector<NamedFace>& faces() const { return faces_; }
  bool empty() const { return shape_.IsNull(); }

  // Name of a face of this shape (matched by identity, ignoring orientation).
  std::optional<std::string> nameOf(const TopoDS_Face& face) const;

  // Topological names built from adjacent face names. nullopt if any
  // adjacent face is unnamed.
  std::optional<std::string> edgeName(const TopoDS_Edge& edge) const;
  std::optional<std::string> vertexName(const TopoDS_Vertex& vertex) const;

  // All edges whose name equals `name` (normally exactly one).
  std::vector<TopoDS_Edge> findEdges(const std::string& name) const;

  // Name of every edge in TopExp::MapShapes(shape, EDGE) order ("" if unnamed);
  // index i matches Mesh::edgeIds.
  std::vector<std::string> edgeNamesIndexed() const;

  // Every edge name, for debugging and the UI.
  std::vector<std::string> edgeNames() const;

 private:
  TopoDS_Shape shape_;
  std::vector<NamedFace> faces_;
};

}  // namespace cadx
