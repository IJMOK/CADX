#pragma once
// Turns a solved sketch into a planar face for features to use. Lines, arcs
// and circles may be in any order; they are joined into closed loops. The
// largest loop is the outer boundary, loops inside it become holes.

#include "sketch/sketch.hpp"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <stdexcept>
#include <string>
#include <vector>

namespace cadx {

struct ProfileError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Profile {
  TopoDS_Face face;  // on the XY plane, normal +Z
  // Every edge of the face, with the sketch entity that made it ("Sketch1/L1").
  std::vector<std::pair<TopoDS_Edge, std::string>> edges;
};

Profile buildProfile(const sketch::Sketch& solved);

}  // namespace cadx
