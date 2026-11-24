#include "sketch/sketch.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

using nlohmann::json;

namespace cadx::sketch {

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;

Vec2 vec(const json& j) {
  if (!j.is_array() || j.size() != 2) throw std::invalid_argument("expected [x, y], got " + j.dump());
  return {j.at(0).get<double>(), j.at(1).get<double>()};
}

json vec(const Vec2& v) { return json::array({v.x, v.y}); }

EntityType entityType(const std::string& t) {
  if (t == "point") return EntityType::Point;
  if (t == "line") return EntityType::Line;
  if (t == "circle") return EntityType::Circle;
  if (t == "arc") return EntityType::Arc;
  throw std::invalid_argument("unknown entity type: " + t);
}

const char* entityType(EntityType t) {
  switch (t) {
    case EntityType::Point: return "point";
    case EntityType::Line: return "line";
    case EntityType::Circle: return "circle";
    case EntityType::Arc: return "arc";
  }
  return "?";
}

}  // namespace

Vec2 Entity::arcStart() const {
  return {center.x + radius * std::cos(startDeg * kDegToRad), center.y + radius * std::sin(startDeg * kDegToRad)};
}

Vec2 Entity::arcEnd() const {
  return {center.x + radius * std::cos(endDeg * kDegToRad), center.y + radius * std::sin(endDeg * kDegToRad)};
}

const Entity* Sketch::find(const std::string& entityId) const {
  for (const auto& e : entities) {
    if (e.id == entityId) return &e;
  }
  return nullptr;
}

const char* toString(SolveState s) {
  switch (s) {
    case SolveState::Solved: return "solved";
    case SolveState::Conflicting: return "conflicting";
    case SolveState::Failed: return "failed";
    case SolveState::Invalid: return "invalid";
  }
  return "?";
}

Sketch fromJson(const json& j) {
  Sketch s;
  s.id = j.at("id").get<std::string>();
  for (const auto& je : j.value("entities", json::array())) {
    Entity e;
    e.id = je.at("id").get<std::string>();
    if (e.id.empty() || e.id.find_first_of(".|()/ ") != std::string::npos || e.id == "origin" ||
        e.id == "xaxis" || e.id == "yaxis") {
      throw std::invalid_argument("invalid entity id: '" + e.id + "'");
    }
    if (s.find(e.id)) throw std::invalid_argument("duplicate entity id: " + e.id);
    e.type = entityType(je.at("type").get<std::string>());
    e.construction = je.value("construction", false);
    switch (e.type) {
      case EntityType::Point: e.a = vec(je.at("p")); break;
      case EntityType::Line: e.a = vec(je.at("a")); e.b = vec(je.at("b")); break;
      case EntityType::Circle:
        e.center = vec(je.at("center"));
        e.radius = je.at("radius").get<double>();
        break;
      case EntityType::Arc:
        e.center = vec(je.at("center"));
        e.radius = je.at("radius").get<double>();
        e.startDeg = je.at("start").get<double>();
        e.endDeg = je.at("end").get<double>();
        break;
    }
    s.entities.push_back(e);
  }
  for (const auto& jc : j.value("constraints", json::array())) {
    Constraint c;
    c.id = jc.at("id").get<std::string>();
    c.type = jc.at("type").get<std::string>();
    c.refs = jc.at("refs").get<std::vector<std::string>>();
    c.value = jc.value("value", 0.0);
    s.constraints.push_back(c);
  }
  return s;
}

json toJson(const Sketch& s) {
  json entities = json::array();
  for (const auto& e : s.entities) {
    json je{{"id", e.id}, {"type", entityType(e.type)}};
    if (e.construction) je["construction"] = true;
    switch (e.type) {
      case EntityType::Point: je["p"] = vec(e.a); break;
      case EntityType::Line: je["a"] = vec(e.a); je["b"] = vec(e.b); break;
      case EntityType::Circle: je["center"] = vec(e.center); je["radius"] = e.radius; break;
      case EntityType::Arc:
        je["center"] = vec(e.center);
        je["radius"] = e.radius;
        je["start"] = e.startDeg;
        je["end"] = e.endDeg;
        break;
    }
    entities.push_back(je);
  }
  json constraints = json::array();
  for (const auto& c : s.constraints) {
    json jc{{"id", c.id}, {"type", c.type}, {"refs", c.refs}};
    if (c.value != 0) jc["value"] = c.value;
    constraints.push_back(jc);
  }
  return {{"id", s.id}, {"entities", entities}, {"constraints", constraints}};
}

json toJson(const SolveResult& r) {
  return {{"state", toString(r.state)},
          {"dof", r.dof},
          {"conflicting", r.conflicting},
          {"redundant", r.redundant},
          {"message", r.message},
          {"sketch", toJson(r.sketch)}};
}

}  // namespace cadx::sketch
