#include "mesh.hpp"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Polygon3D.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

namespace cadx {

Mesh tessellate(const TopoDS_Shape& shape, double linearDeflection,
                double angularDeflection) {
  BRepMesh_IncrementalMesh mesher(shape, linearDeflection, false, angularDeflection, true);
  mesher.Perform();

  Mesh out;

  // Stable face numbering: the order of TopExp::MapShapes. Persistent names
  // replace this in Phase 1; for now the UI uses it only for picking feedback.
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);

  for (int fi = 1; fi <= faces.Extent(); ++fi) {
    const TopoDS_Face& face = TopoDS::Face(faces(fi));
    TopLoc_Location loc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;

    const gp_Trsf trsf = loc.Transformation();
    const auto base = static_cast<std::uint32_t>(out.positions.size() / 3);

    for (int i = 1; i <= tri->NbNodes(); ++i) {
      const gp_Pnt p = tri->Node(i).Transformed(trsf);
      out.positions.push_back(static_cast<float>(p.X()));
      out.positions.push_back(static_cast<float>(p.Y()));
      out.positions.push_back(static_cast<float>(p.Z()));
    }

    const bool reversed = face.Orientation() == TopAbs_REVERSED;
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
      int n1, n2, n3;
      tri->Triangle(i).Get(n1, n2, n3);
      if (reversed) std::swap(n2, n3);
      out.indices.push_back(base + n1 - 1);
      out.indices.push_back(base + n2 - 1);
      out.indices.push_back(base + n3 - 1);
      out.faceIds.push_back(static_cast<std::uint32_t>(fi - 1));
    }
  }

  // B-rep edges as polylines, taken from the triangulation of an adjacent face.
  TopTools_IndexedMapOfShape edgeMap;
  TopExp::MapShapes(shape, TopAbs_EDGE, edgeMap);
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  for (int ei = 1; ei <= edgeMap.Extent(); ++ei) {
    const TopoDS_Edge& edge = TopoDS::Edge(edgeMap(ei));
    const int fi = edgeFaces.FindIndex(edge);
    if (BRep_Tool::Degenerated(edge) || fi == 0 || edgeFaces(fi).IsEmpty()) continue;
    const TopoDS_Face& face = TopoDS::Face(edgeFaces(fi).First());

    TopLoc_Location loc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    Handle(Poly_PolygonOnTriangulation) poly =
        BRep_Tool::PolygonOnTriangulation(edge, tri, loc);
    if (poly.IsNull()) continue;

    const gp_Trsf trsf = loc.Transformation();
    const TColStd_Array1OfInteger& nodes = poly->Nodes();
    for (int i = nodes.Lower(); i < nodes.Upper(); ++i) {
      const gp_Pnt a = tri->Node(nodes(i)).Transformed(trsf);
      const gp_Pnt b = tri->Node(nodes(i + 1)).Transformed(trsf);
      for (const gp_Pnt& p : {a, b}) {
        out.edges.push_back(static_cast<float>(p.X()));
        out.edges.push_back(static_cast<float>(p.Y()));
        out.edges.push_back(static_cast<float>(p.Z()));
      }
      out.edgeIds.push_back(static_cast<std::uint32_t>(ei - 1));
    }
  }

  return out;
}

}  // namespace cadx
