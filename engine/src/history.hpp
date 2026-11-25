#pragma once
// Naming the result of an OCCT operation from its history.
//
// Every face of the result gets a name, in this order of preference:
//   1. it is a face of an input, unchanged          -> that face's name
//   2. it is Modified() from an input face and lies on that face's surface
//                                                   -> that face's name
//      (Modified but moved off the surface, e.g. a shell's inner wall)
//                                                   -> "<feature>/offset(<face>)"
//   3. it is Generated() from an input edge         -> "<feature>/blend(<edge>)"
//      (edges in `priorityEdges` are tried first; a fillet passes the edges
//       the user picked, so a tangent chain is named after the picked edge)
//   4. it is Generated() from an input vertex       -> "<feature>/corner(<vertex>)"
//   5. it is Generated() from an input face         -> "<feature>/from(<face>)"
//   6. it lies on an input face (geometric fallback, for operations with
//      incomplete history)                          -> that face's name
//   7. otherwise                                    -> "<feature>/face<n>", n counting
//      only these leftovers, in result order (stable for a stable result)

#include "naming.hpp"

#include <BRepBuilderAPI_MakeShape.hxx>
#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace cadx {

struct HistoryInput {
  const NamedShape* shape;
  std::vector<std::pair<TopoDS_Edge, std::string>> priorityEdges;  // optional
};

NamedShape nameFromHistory(const TopoDS_Shape& result, BRepBuilderAPI_MakeShape& op,
                           const std::vector<HistoryInput>& inputs, const std::string& featureId);

// Names a shape that was moved or copied by a transform (BRepBuilderAPI_Transform
// or Moved()): faces map one to one, in TopExp order, and get `prefix` + old name.
NamedShape renameTransformed(const NamedShape& source, const TopoDS_Shape& moved, const std::string& prefix);

// True if the face lies on (within tol of) `onto`'s surface at a point inside `face`.
bool faceLiesOn(const TopoDS_Face& face, const TopoDS_Face& onto, double tol = 1e-5);

}  // namespace cadx
