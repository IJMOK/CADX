#pragma once
// Turns a solved sketch into planar faces for features to use. Lines, arcs
// and circles may be in any order; they are joined into closed loops. Loops
// are nested by containment: a loop inside an outer boundary is a hole, a
// loop inside a hole is a new island, and so on. Separate regions are fine.

#include "sketch/sketch.hpp"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <stdexcept>
#include <string>
#include <vector>

namespace cadx {

struct ProfileError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Profile {
  TopoDS_Shape shape;               // one face, or a compound of faces; XY plane, normal +Z
  std::vector<TopoDS_Face> faces;   // the faces of `shape`
  // Every edge of the face, with the sketch entity that made it ("Sketch1/L1").
  std::vector<std::pair<TopoDS_Edge, std::string>> edges;
};

Profile buildProfile(const sketch::Sketch& solved);

}  // namespace cadx
