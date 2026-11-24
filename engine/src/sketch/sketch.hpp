#pragma once
// CADX sketch model and solver interface.
//
// This is the only sketch API the rest of the engine sees. The solver behind
// it (PlaneGCS today, see sketch_solver_planegcs.cpp) can be replaced without
// touching features, naming or the RPC layer.
//
// Sketch JSON:
// {
//   "id": "Sketch1",
//   "entities": [
//     { "id": "L1", "type": "line",   "a": [0,0], "b": [40,0] },
//     { "id": "C1", "type": "circle", "center": [20,10], "radius": 5 },
//     { "id": "A1", "type": "arc",    "center": [0,0], "radius": 5, "start": 0, "end": 90 },  // degrees, CCW
//     { "id": "P1", "type": "point",  "p": [5,5] },
//     ... "construction": true marks helper geometry that does not make profiles
//   ],
//   "constraints": [
//     { "id": "c1", "type": "coincident", "refs": ["L1.end", "L2.start"] },
//     { "id": "d1", "type": "distance", "refs": ["L1"], "value": 40 },
//     ...
//   ]
// }
//
// Point references: "<line>.start|end", "<arc>.start|end|center",
// "<circle>.center", "<point>", and the fixed "origin".
// Built-in fixed lines: "xaxis", "yaxis".
//
// Constraint types and their refs:
//   coincident     point, point
//   horizontal     line | point, point
//   vertical       line | point, point
//   parallel       line, line
//   perpendicular  line, line
//   tangent        line, circle|arc | circle|arc, circle|arc
//   equal          line, line (length) | circle|arc, circle|arc (radius)
//   pointOn        point, line|circle|arc
//   midpoint       point, line
//   symmetric      point, point, line|point
//   distance       line (length) | point, point | point, line          value (mm)
//   distanceX      point, point | point (from origin)                  value (mm, signed)
//   distanceY      point, point | point (from origin)                  value (mm, signed)
//   angle          line, line                                          value (degrees)
//   radius         circle|arc                                          value (mm)
//   diameter       circle|arc                                          value (mm)
//   fix            point | entity (all its points)

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace cadx::sketch {

struct Vec2 {
  double x = 0, y = 0;
};

enum class EntityType { Point, Line, Circle, Arc };

struct Entity {
  std::string id;
  EntityType type = EntityType::Line;
  bool construction = false;
  Vec2 a, b;                 // point: a; line: a -> b
  Vec2 center;               // circle, arc
  double radius = 0;         // circle, arc
  double startDeg = 0;       // arc, counter-clockwise from start to end
  double endDeg = 0;

  Vec2 arcStart() const;
  Vec2 arcEnd() const;
};

struct Constraint {
  std::string id;
  std::string type;
  std::vector<std::string> refs;
  double value = 0;
};

struct Sketch {
  std::string id;
  std::vector<Entity> entities;
  std::vector<Constraint> constraints;

  const Entity* find(const std::string& entityId) const;
};

enum class SolveState {
  Solved,        // all constraints satisfied
  Conflicting,   // constraints contradict each other; geometry left as given
  Failed,        // solver did not converge; geometry left as given
  Invalid,       // the sketch definition is wrong (bad refs, unknown types)
};

struct SolveResult {
  SolveState state = SolveState::Failed;
  Sketch sketch;                          // solved geometry (or the input, if not solved)
  int dof = 0;                            // remaining degrees of freedom; 0 = fully constrained
  std::vector<std::string> conflicting;   // constraint ids
  std::vector<std::string> redundant;     // constraint ids
  std::string message;
};

SolveResult solve(const Sketch& sketch);

Sketch fromJson(const nlohmann::json& j);
nlohmann::json toJson(const Sketch& s);
nlohmann::json toJson(const SolveResult& r);
const char* toString(SolveState s);

}  // namespace cadx::sketch
