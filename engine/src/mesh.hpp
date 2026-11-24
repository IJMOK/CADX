#pragma once

#include <TopoDS_Shape.hxx>

#include <cstdint>
#include <vector>

namespace cadx {

// Triangle mesh for the viewport. Vertices are not shared between faces,
// so each face shades flat and keeps crisp edges.
struct Mesh {
  std::vector<float> positions;     // xyz per vertex
  std::vector<std::uint32_t> indices;  // 3 per triangle
  std::vector<std::uint32_t> faceIds;  // 1 per triangle: index of the B-rep face it came from
  std::vector<float> edges;         // line segments, xyz xyz per segment (B-rep edges)
};

// Tessellate a shape. linearDeflection is in mm.
Mesh tessellate(const TopoDS_Shape& shape, double linearDeflection = 0.1,
                double angularDeflection = 0.3);

}  // namespace cadx
