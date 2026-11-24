#pragma once
// A part document and its rebuild. Geometry is never stored: the part is
// rebuilt from its sketches and features, and every face gets a persistent
// name on the way (see naming.hpp).
//
// Document JSON (Phase 1 spike):
// {
//   "sketches": [{ "id": "Sketch1",
//                  "segments": [{ "id": "L1", "a": [0,0], "b": [40,0] }, ...] }],
//   "features": [{ "id": "Extrude1", "type": "extrude", "sketch": "Sketch1", "distance": 10 },
//                { "id": "Fillet1", "type": "fillet", "radius": 2,
//                  "edges": ["Extrude1/end|Extrude1/side(Sketch1/L1)"] }]
// }
// Sketches lie on the XY plane; segments must form one closed loop, in order.

#include "naming.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace cadx {

enum class FeatureState { Ok, Failed, Suppressed };

struct FeatureStatus {
  std::string id;
  FeatureState state = FeatureState::Ok;
  std::string message;  // empty when Ok
};

struct RebuildResult {
  NamedShape body;                     // the last good body
  std::vector<FeatureStatus> features; // one per feature, in tree order
};

RebuildResult rebuild(const nlohmann::json& document);

const char* toString(FeatureState s);

}  // namespace cadx
