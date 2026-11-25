#pragma once
// A part document and its rebuild. Geometry is never stored: the part is
// rebuilt from its parameters, sketches and features, and every face gets a
// persistent name on the way (see naming.hpp and history.hpp).
//
// Document JSON (format "cadx-part", version 1):
// {
//   "parameters": [{ "name": "w", "expression": "40" }],
//   "sketches":   [ <sketch> ],      // legacy: sketches outside the tree, on XY unless "plane" says otherwise
//   "features": [                     // tree order
//     { "id": "Sketch1", "type": "sketch", "plane": "XY", "entities": [...], "constraints": [...] },
//     { "id": "Extrude1", "type": "extrude", "sketch": "Sketch1", "distance": "w / 2" },
//     ...
//   ]
// }
//
// Every numeric field may be an expression string (see expr.hpp).
// Every feature may carry "suppressed": true and a display "name".
//
// Feature types (all lengths mm, angles degrees):
//   sketch        plane, entities, constraints
//   datumPlane    base (plane spec), offset
//   extrude       sketch, distance, direction: normal|reverse|symmetric|twoSided, distance2,
//                 through: true (through all), op
//   revolve       sketch, axis (sketch line id | "H" | "V" | "X" | "Y" | "Z" | edge name), angle, op
//   hole          sketch (its points are the centres), diameter, depth | through: true,
//                 form: simple|counterbore|countersink, cbDiameter, cbDepth, csDiameter, csAngle,
//                 tipAngle (0 = flat bottom)
//   fillet        edges, radius
//   chamfer       edges, distance
//   shell         faces (removed), thickness (inwards)
//   linearPattern features | body: true, direction, count, spacing, [direction2, count2, spacing2]
//   circularPattern features | body: true, axis, count, angle (total; 360 = full circle)
//   mirror        features | body: true, plane
//   box           length, width, height, position [x,y,z], op
//   cylinder      radius, height, position, axis (X|Y|Z), op
//   sphere        radius, position, op
//   cone          r1, r2, height, position, axis, op
//   import        brep (OCCT BRep text), op
//
// op: "add" (default; makes the body if there is none), "cut", "intersect", "new" (first body only).
//
// Plane specs: "XY" | "XZ" | "YZ" | "<datum id>" | "<planar face name>", or an object
//   { "base": <plane spec>, "offset": mm, "flip": bool }.
// Direction specs: "X" | "-X" | "Y" | ... | "<linear edge name>" | "<planar face name>" (its normal).
// Axis specs: "X" | "Y" | "Z" (through the origin) | "<linear or circular edge name>" |
//   "<cylindrical face name>".

#include "naming.hpp"
#include "sketch/sketch.hpp"

#include <gp_Ax3.hxx>
#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace cadx {

enum class FeatureState { Ok, Failed, Suppressed };

struct FeatureStatus {
  std::string id;
  FeatureState state = FeatureState::Ok;
  std::string message;  // empty when Ok
};

struct DatumInfo {
  std::string id;
  gp_Ax3 plane;
};

struct RebuildResult {
  std::vector<sketch::SolveResult> sketches;   // every sketch, solved, in document order
  std::map<std::string, gp_Ax3> placements;    // sketch id -> its plane, once resolved
  std::vector<DatumInfo> datums;
  NamedShape body;                     // the last good body
  std::vector<FeatureStatus> features; // one per feature, in tree order
  std::vector<std::string> expressionErrors;
};

struct RebuildOptions {
  std::string stopAfter;  // roll back: build the tree only up to and including this feature
};

RebuildResult rebuild(const nlohmann::json& document, const RebuildOptions& options = {});

const char* toString(FeatureState s);

}  // namespace cadx
