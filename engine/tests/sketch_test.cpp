// Sketch solver tests: CADX's sketch interface over PlaneGCS.

#include "sketch/sketch.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>

using nlohmann::json;
using namespace cadx::sketch;

namespace {

constexpr double kTol = 1e-6;

json line(const std::string& id, double ax, double ay, double bx, double by) {
  return {{"id", id}, {"type", "line"}, {"a", json::array({ax, ay})}, {"b", json::array({bx, by})}};
}

json con(const std::string& id, const std::string& type, std::vector<std::string> refs, double value = 0) {
  json c{{"id", id}, {"type", type}, {"refs", refs}};
  if (value != 0) c["value"] = value;
  return c;
}

// Rectangle drawn roughly (as a user would), constrained to w x h with its
// first corner on the origin. Fully constrained: 0 DOF.
json rectangle(double w, double h) {
  return {{"id", "Sketch1"},
          {"entities", json::array({line("L1", 0.3, -0.2, 38, 1), line("L2", 38, 1, 41, 22),
                                    line("L3", 41, 22, -1, 19), line("L4", -1, 19, 0.3, -0.2)})},
          {"constraints", json::array({
                              con("c1", "coincident", {"L1.end", "L2.start"}),
                              con("c2", "coincident", {"L2.end", "L3.start"}),
                              con("c3", "coincident", {"L3.end", "L4.start"}),
                              con("c4", "coincident", {"L4.end", "L1.start"}),
                              con("h1", "horizontal", {"L1"}),
                              con("h2", "horizontal", {"L3"}),
                              con("v1", "vertical", {"L2"}),
                              con("v2", "vertical", {"L4"}),
                              con("o1", "coincident", {"L1.start", "origin"}),
                              con("dW", "distance", {"L1"}, w),
                              con("dH", "distance", {"L2"}, h),
                          })}};
}

Entity entity(const SolveResult& r, const std::string& id) {
  const Entity* e = r.sketch.find(id);
  EXPECT_NE(e, nullptr) << id;
  return e ? *e : Entity{};
}

bool has(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

TEST(Sketch, FullyConstrainedRectangleSolvesExactly) {
  const auto r = solve(fromJson(rectangle(40, 20)));
  ASSERT_EQ(r.state, SolveState::Solved) << r.message;
  EXPECT_EQ(r.dof, 0);
  EXPECT_TRUE(r.conflicting.empty());
  const Entity l2 = entity(r, "L2");
  EXPECT_NEAR(l2.a.x, 40, kTol);
  EXPECT_NEAR(l2.a.y, 0, kTol);
  EXPECT_NEAR(l2.b.x, 40, kTol);
  EXPECT_NEAR(l2.b.y, 20, kTol);
  EXPECT_NEAR(entity(r, "L1").a.x, 0, kTol);
}

TEST(Sketch, EditingADimensionMovesTheGeometry) {
  const auto r = solve(fromJson(rectangle(65, 12.5)));
  ASSERT_EQ(r.state, SolveState::Solved) << r.message;
  EXPECT_NEAR(entity(r, "L2").b.x, 65, kTol);
  EXPECT_NEAR(entity(r, "L2").b.y, 12.5, kTol);
}

TEST(Sketch, RemovingADimensionLeavesOneDegreeOfFreedom) {
  json j = rectangle(40, 20);
  auto& cs = j["constraints"];
  cs.erase(std::remove_if(cs.begin(), cs.end(), [](const json& c) { return c["id"] == "dW"; }), cs.end());
  const auto r = solve(fromJson(j));
  EXPECT_EQ(r.state, SolveState::Solved) << r.message;
  EXPECT_EQ(r.dof, 1);
}

TEST(Sketch, UnconstrainedLineHasFourDegreesOfFreedom) {
  const json j{{"id", "S"}, {"entities", json::array({line("L1", 0, 0, 10, 5)})}};
  const auto r = solve(fromJson(j));
  EXPECT_EQ(r.state, SolveState::Solved);
  EXPECT_EQ(r.dof, 4);
  EXPECT_NEAR(entity(r, "L1").b.x, 10, kTol);  // untouched
}

TEST(Sketch, ContradictoryDimensionsAreReportedAsConflicting) {
  // L3 is forced to equal L1 by the rectangle; giving it another length conflicts.
  json j = rectangle(40, 20);
  j["constraints"].push_back(con("dBad", "distance", {"L3"}, 50));
  const auto r = solve(fromJson(j));
  EXPECT_EQ(r.state, SolveState::Conflicting);
  EXPECT_TRUE(has(r.conflicting, "dBad")) << json(r.conflicting).dump();
  // Geometry is returned unchanged, not half-solved.
  EXPECT_NEAR(entity(r, "L1").b.x, 38, kTol);
}

TEST(Sketch, RedundantConstraintIsReportedButStillSolves) {
  json j = rectangle(40, 20);
  j["constraints"].push_back(con("pRed", "parallel", {"L1", "L3"}));  // already implied
  const auto r = solve(fromJson(j));
  EXPECT_EQ(r.state, SolveState::Solved) << r.message;
  EXPECT_FALSE(r.redundant.empty());
  EXPECT_NEAR(entity(r, "L2").b.x, 40, kTol);
}

TEST(Sketch, SlotWithTangentArcs) {
  // Two horizontal lines joined by two semicircular arcs, tangent at each join.
  const json j{
      {"id", "Slot"},
      {"entities",
       json::array({line("T", 0, 10, 50, 10), line("B", 50, -10, 0, -10),
                    {{"id", "AR"}, {"type", "arc"}, {"center", {50, 0}}, {"radius", 10}, {"start", -90}, {"end", 90}},
                    {{"id", "AL"}, {"type", "arc"}, {"center", {0, 0}}, {"radius", 10}, {"start", 90}, {"end", 270}}})},
      {"constraints",
       json::array({con("t1", "tangent", {"T.end", "AR.end"}), con("t2", "tangent", {"AR.start", "B.start"}),
                    con("t3", "tangent", {"B.end", "AL.end"}), con("t4", "tangent", {"AL.start", "T.start"}),
                    con("h1", "horizontal", {"T"}), con("o1", "coincident", {"AL.center", "origin"}),
                    con("r1", "radius", {"AL"}, 6), con("eq", "equal", {"AL", "AR"}),
                    con("len", "distance", {"AL.center", "AR.center"}, 30)})}};
  const auto r = solve(fromJson(j));
  ASSERT_EQ(r.state, SolveState::Solved) << r.message;
  EXPECT_EQ(r.dof, 0) << "redundant: " << json(r.redundant).dump();
  EXPECT_TRUE(r.redundant.empty());
  const Entity ar = entity(r, "AR");
  EXPECT_NEAR(ar.radius, 6, kTol);
  EXPECT_NEAR(ar.center.x, 30, kTol);
  EXPECT_NEAR(ar.center.y, 0, kTol);
  EXPECT_NEAR(entity(r, "T").a.y, 6, kTol);
  EXPECT_NEAR(entity(r, "B").a.y, -6, kTol);
}

TEST(Sketch, CircleDiameterAndPosition) {
  const json j{{"id", "S"},
               {"entities", json::array({{{"id", "C1"}, {"type", "circle"}, {"center", {3, 4}}, {"radius", 7}}})},
               {"constraints", json::array({con("dx", "distanceX", {"C1.center"}, 25),
                                            con("dy", "distanceY", {"C1.center"}, -10),
                                            con("d", "diameter", {"C1"}, 12)})}};
  const auto r = solve(fromJson(j));
  ASSERT_EQ(r.state, SolveState::Solved) << r.message;
  EXPECT_EQ(r.dof, 0);
  const Entity c = entity(r, "C1");
  EXPECT_NEAR(c.center.x, 25, kTol);
  EXPECT_NEAR(c.center.y, -10, kTol);
  EXPECT_NEAR(c.radius, 6, kTol);
}

TEST(Sketch, AngleBetweenLines) {
  const json j{{"id", "S"},
               {"entities", json::array({line("A", 0, 0, 10, 0), line("B", 0, 0, 7, 8)})},
               {"constraints", json::array({con("c", "coincident", {"A.start", "B.start"}),
                                            con("f", "fix", {"A"}), con("len", "distance", {"B"}, 10),
                                            con("ang", "angle", {"A", "B"}, 30)})}};
  const auto r = solve(fromJson(j));
  ASSERT_EQ(r.state, SolveState::Solved) << r.message;
  EXPECT_EQ(r.dof, 0);
  const Entity b = entity(r, "B");
  EXPECT_NEAR(b.b.x, 10 * std::cos(std::numbers::pi / 6), 1e-5);
  EXPECT_NEAR(b.b.y, 10 * std::sin(std::numbers::pi / 6), 1e-5);
}

TEST(Sketch, SymmetricAndMidpoint) {
  const json j{{"id", "S"},
               {"entities", json::array({line("L", -8, 1, 12, -1), {{"id", "M"}, {"type", "point"}, {"p", {1, 2}}}})},
               {"constraints", json::array({con("s", "symmetric", {"L.start", "L.end", "yaxis"}),
                                            con("h", "horizontal", {"L"}), con("len", "distance", {"L"}, 20),
                                            con("y", "distanceY", {"L.start"}, 5),
                                            con("m", "midpoint", {"M", "L"})})}};
  const auto r = solve(fromJson(j));
  ASSERT_EQ(r.state, SolveState::Solved) << r.message;
  EXPECT_EQ(r.dof, 0);
  EXPECT_NEAR(entity(r, "L").a.x, -10, kTol);
  EXPECT_NEAR(entity(r, "L").b.x, 10, kTol);
  EXPECT_NEAR(entity(r, "M").a.x, 0, kTol);
  EXPECT_NEAR(entity(r, "M").a.y, 5, kTol);
}

TEST(Sketch, BadReferenceIsInvalidNotACrash) {
  json j = rectangle(40, 20);
  j["constraints"].push_back(con("oops", "coincident", {"L9.end", "L1.start"}));
  const auto r = solve(fromJson(j));
  EXPECT_EQ(r.state, SolveState::Invalid);
  EXPECT_NE(r.message.find("L9"), std::string::npos);
}

TEST(Sketch, JsonRoundTrip) {
  const Sketch s = fromJson(rectangle(40, 20));
  const Sketch t = fromJson(toJson(s));
  ASSERT_EQ(t.entities.size(), 4u);
  ASSERT_EQ(t.constraints.size(), s.constraints.size());
  EXPECT_EQ(t.constraints.back().value, 20);
}
