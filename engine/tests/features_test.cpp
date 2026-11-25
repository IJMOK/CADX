// Feature regression suite: every feature type builds a valid solid with the
// expected volume, and its faces carry the expected persistent names.

#include "expr.hpp"
#include "part.hpp"

#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <set>
#include <string>

using nlohmann::json;
using namespace cadx;

namespace {

constexpr double kPi = std::numbers::pi;

json line(const std::string& id, double ax, double ay, double bx, double by) {
  return {{"id", id}, {"type", "line"}, {"a", {ax, ay}}, {"b", {bx, by}}};
}

json rect(const std::string& prefix, double x0, double y0, double x1, double y1) {
  return json::array({line(prefix + "1", x0, y0, x1, y0), line(prefix + "2", x1, y0, x1, y1),
                      line(prefix + "3", x1, y1, x0, y1), line(prefix + "4", x0, y1, x0, y0)});
}

json sketchF(const std::string& id, const json& entities, const json& plane = "XY") {
  return {{"id", id}, {"type", "sketch"}, {"plane", plane}, {"entities", entities}, {"constraints", json::array()}};
}

json point(const std::string& id, double x, double y) { return {{"id", id}, {"type", "point"}, {"p", {x, y}}}; }

json circle(const std::string& id, double x, double y, double r) {
  return {{"id", id}, {"type", "circle"}, {"center", {x, y}}, {"radius", r}};
}

json extrude(const std::string& id, const std::string& sketch, json distance, const std::string& op = "add") {
  return {{"id", id}, {"type", "extrude"}, {"sketch", sketch}, {"distance", distance}, {"op", op}};
}

// A 40 x 20 x 10 block from the origin.
json block(json more = json::array()) {
  json features = json::array({sketchF("Sketch1", rect("L", 0, 0, 40, 20)), extrude("Extrude1", "Sketch1", 10)});
  for (auto& f : more) features.push_back(f);
  return {{"features", features}};
}

double volume(const RebuildResult& r) {
  GProp_GProps g;
  BRepGProp::VolumeProperties(r.body.shape(), g);
  return g.Mass();
}

int solids(const RebuildResult& r) {
  int n = 0;
  for (TopExp_Explorer ex(r.body.shape(), TopAbs_SOLID); ex.More(); ex.Next()) ++n;
  return n;
}

std::set<std::string> names(const RebuildResult& r) {
  std::set<std::string> out;
  for (const auto& f : r.body.faces()) out.insert(f.name);
  return out;
}

void expectOk(const RebuildResult& r) {
  for (const auto& f : r.features) EXPECT_EQ(f.state, FeatureState::Ok) << f.id << ": " << f.message;
  ASSERT_FALSE(r.body.empty());
  EXPECT_TRUE(BRepCheck_Analyzer(r.body.shape()).IsValid()) << "invalid solid";
}

}  // namespace

TEST(Features, BossOnTopFaceAddsVolumeAndNamesItsFaces) {
  const auto r = rebuild(block(json::array({sketchF("Sketch2", rect("B", 10, 5, 20, 15), "Extrude1/end"),
                                            extrude("Extrude2", "Sketch2", 5)})));
  expectOk(r);
  EXPECT_NEAR(volume(r), 40 * 20 * 10 + 10 * 10 * 5, 1e-6);
  EXPECT_TRUE(names(r).count("Extrude2/side(Sketch2/B1)"));
  EXPECT_TRUE(names(r).count("Extrude2/end"));
  EXPECT_TRUE(names(r).count("Extrude1/end")) << "the top face keeps its name around the boss";
}

TEST(Features, CutThroughAllFromTheTopFace) {
  const auto r = rebuild(block(json::array(
      {sketchF("Sketch2", json::array({circle("C1", 20, 10, 3)}), "Extrude1/end"),
       {{"id", "Cut1"}, {"type", "extrude"}, {"sketch", "Sketch2"}, {"through", true}, {"op", "cut"}}})));
  expectOk(r);
  EXPECT_NEAR(volume(r), 40 * 20 * 10 - kPi * 9 * 10, 1e-3);
  EXPECT_TRUE(names(r).count("Cut1/side(Sketch2/C1)"));
}

TEST(Features, FilletSurvivesALaterCut) {
  const std::string edge = "Extrude1/end|Extrude1/side(Sketch1/L1)";
  const auto r = rebuild(block(json::array(
      {{{"id", "Fillet1"}, {"type", "fillet"}, {"radius", 2}, {"edges", {edge}}},
       sketchF("Sketch2", json::array({circle("C1", 20, 12, 3)}), "XY"),
       {{"id", "Cut1"}, {"type", "extrude"}, {"sketch", "Sketch2"}, {"distance", 10}, {"op", "cut"}}})));
  expectOk(r);
  EXPECT_TRUE(names(r).count("Fillet1/blend(" + edge + ")"));
}

TEST(Features, RevolveMakesATube) {
  // Rectangle from r=5 to r=8, height 10, on XZ, revolved about the sketch's vertical axis (world Z).
  json doc{{"features", json::array({sketchF("Sketch1", rect("L", 5, 0, 8, 10), "XZ"),
                                     {{"id", "Revolve1"}, {"type", "revolve"}, {"sketch", "Sketch1"}, {"axis", "V"}}})}};
  const auto r = rebuild(doc);
  expectOk(r);
  EXPECT_NEAR(volume(r), kPi * (64 - 25) * 10, 1e-3);
  EXPECT_TRUE(names(r).count("Revolve1/side(Sketch1/L2)"));
}

TEST(Features, RevolveNamesFacesSquareToTheAxis) {
  // L1 (bottom) and L3 (top) are square to the axis: OCCT's revolve history skips them.
  json doc{{"features", json::array({sketchF("Sketch1", rect("L", 5, 0, 8, 10), "XZ"),
                                     {{"id", "Revolve1"}, {"type", "revolve"}, {"sketch", "Sketch1"}, {"axis", "V"}}})}};
  const auto r = rebuild(doc);
  expectOk(r);
  const auto n = names(r);
  EXPECT_EQ(n.size(), 4u);
  for (const char* e : {"L1", "L2", "L3", "L4"}) EXPECT_TRUE(n.count(std::string("Revolve1/side(Sketch1/") + e + ")")) << e;
}

TEST(Features, EveryFaceOfEveryFeatureHasAName) {
  json doc = block(json::array({
      {{"id", "Shell1"}, {"type", "shell"}, {"thickness", 2}, {"faces", {"Extrude1/start"}}},
      sketchF("Sketch2", json::array({point("P1", 10, 10)}), "Extrude1/end"),
      {{"id", "Hole1"}, {"type", "hole"}, {"sketch", "Sketch2"}, {"diameter", 4}, {"depth", 5}, {"form", "countersink"},
       {"csDiameter", 8}, {"tipAngle", 118}},
      {{"id", "Chamfer1"}, {"type", "chamfer"}, {"distance", 1}, {"edges", {"Extrude1/end|Extrude1/side(Sketch1/L1)"}}}}));
  const auto r = rebuild(doc);
  expectOk(r);
  for (const auto& f : r.body.faces()) {
    EXPECT_EQ(f.name.find('?'), std::string::npos) << f.name;
    EXPECT_FALSE(f.name.empty());
  }
}

TEST(Features, PartialRevolveHasStartAndEndFaces) {
  json doc{{"features", json::array({sketchF("Sketch1", rect("L", 5, 0, 8, 10), "XZ"),
                                     {{"id", "Revolve1"}, {"type", "revolve"}, {"sketch", "Sketch1"},
                                      {"axis", "V"}, {"angle", 90}}})}};
  const auto r = rebuild(doc);
  expectOk(r);
  EXPECT_NEAR(volume(r), kPi * (64 - 25) * 10 / 4, 1e-3);
  EXPECT_TRUE(names(r).count("Revolve1/start"));
  EXPECT_TRUE(names(r).count("Revolve1/end"));
}

TEST(Features, ThroughHolesAtSketchPoints) {
  const auto r = rebuild(block(json::array(
      {sketchF("Sketch2", json::array({point("P1", 10, 10), point("P2", 30, 10)}), "Extrude1/end"),
       {{"id", "Hole1"}, {"type", "hole"}, {"sketch", "Sketch2"}, {"diameter", 6}, {"through", true}}})));
  expectOk(r);
  EXPECT_NEAR(volume(r), 40 * 20 * 10 - 2 * kPi * 9 * 10, 1e-3);
  EXPECT_TRUE(names(r).count("Hole1/wall(P1)"));
  EXPECT_TRUE(names(r).count("Hole1/wall(P2)"));
}

TEST(Features, CounterboreAndBlindDrilledHoles) {
  const auto r = rebuild(block(json::array(
      {sketchF("Sketch2", json::array({point("P1", 10, 10)}), "Extrude1/end"),
       {{"id", "Hole1"}, {"type", "hole"}, {"sketch", "Sketch2"}, {"diameter", 5}, {"depth", 8},
        {"form", "counterbore"}, {"cbDiameter", 9}, {"cbDepth", 3}},
       sketchF("Sketch3", json::array({point("P1", 30, 10)}), "Extrude1/end"),
       {{"id", "Hole2"}, {"type", "hole"}, {"sketch", "Sketch3"}, {"diameter", 5}, {"depth", 6}, {"tipAngle", 118}}})));
  expectOk(r);
  const double cb = kPi * 4.5 * 4.5 * 3 + kPi * 2.5 * 2.5 * 5;
  const double tip = 2.5 / std::tan(59 * kPi / 180);
  const double drilled = kPi * 2.5 * 2.5 * 6 + kPi * 2.5 * 2.5 * tip / 3;
  EXPECT_NEAR(volume(r), 40 * 20 * 10 - cb - drilled, 1e-2);
  EXPECT_TRUE(names(r).count("Hole1/cbFloor(P1)"));
  EXPECT_TRUE(names(r).count("Hole2/tip(P1)"));
}

TEST(Features, ChamferAnEdge) {
  const std::string edge = "Extrude1/end|Extrude1/side(Sketch1/L1)";
  const auto r = rebuild(block(json::array({{{"id", "Chamfer1"}, {"type", "chamfer"}, {"distance", 2}, {"edges", {edge}}}})));
  expectOk(r);
  EXPECT_NEAR(volume(r), 40 * 20 * 10 - 0.5 * 2 * 2 * 40, 1e-4);
  EXPECT_TRUE(names(r).count("Chamfer1/blend(" + edge + ")"));
}

TEST(Features, ShellRemovesTheTop) {
  const auto r = rebuild(block(json::array({{{"id", "Shell1"}, {"type", "shell"}, {"thickness", 1}, {"faces", {"Extrude1/end"}}}})));
  expectOk(r);
  EXPECT_NEAR(volume(r), 40 * 20 * 10 - 38 * 18 * 9, 1e-3);
  EXPECT_FALSE(names(r).count("Extrude1/end"));
}

TEST(Features, LinearPatternOfACut) {
  const auto r = rebuild(block(json::array(
      {sketchF("Sketch2", json::array({circle("C1", 5, 10, 2)}), "XY"),
       {{"id", "Cut1"}, {"type", "extrude"}, {"sketch", "Sketch2"}, {"distance", 10}, {"op", "cut"}},
       {{"id", "Pattern1"}, {"type", "linearPattern"}, {"features", {"Cut1"}}, {"direction", "X"}, {"count", 4}, {"spacing", 10}}})));
  expectOk(r);
  EXPECT_NEAR(volume(r), 40 * 20 * 10 - 4 * kPi * 4 * 10, 1e-3);
  EXPECT_TRUE(names(r).count("Pattern1[3]/Cut1/side(Sketch2/C1)"));
}

TEST(Features, CircularPatternOfABoss) {
  json doc{{"features", json::array({
      sketchF("Sketch1", json::array({circle("C1", 0, 0, 20)})), extrude("Disc", "Sketch1", 5),
      sketchF("Sketch2", json::array({circle("C1", 15, 0, 2)}), "Disc/end"), extrude("Peg", "Sketch2", 5),
      {{"id", "Pattern1"}, {"type", "circularPattern"}, {"features", {"Peg"}}, {"axis", "Z"}, {"count", 6}}})}};
  const auto r = rebuild(doc);
  expectOk(r);
  EXPECT_NEAR(volume(r), kPi * 400 * 5 + 6 * kPi * 4 * 5, 1e-2);
}

TEST(Features, MirrorTheBody) {
  const auto r = rebuild(block(json::array({{{"id", "Mirror1"}, {"type", "mirror"}, {"body", true}, {"plane", "Extrude1/side(Sketch1/L4)"}}})));
  expectOk(r);
  EXPECT_NEAR(volume(r), 2 * 40 * 20 * 10, 1e-3);
}

TEST(Features, SketchOnAnOffsetDatumPlane) {
  json doc{{"features", json::array({{{"id", "Plane1"}, {"type", "datumPlane"}, {"base", "XY"}, {"offset", 25}},
                                     sketchF("Sketch1", rect("L", 0, 0, 10, 10), "Plane1"),
                                     extrude("Extrude1", "Sketch1", 5)})}};
  const auto r = rebuild(doc);
  expectOk(r);
  ASSERT_EQ(r.datums.size(), 1u);
  GProp_GProps g;
  BRepGProp::VolumeProperties(r.body.shape(), g);
  EXPECT_NEAR(g.CentreOfMass().Z(), 27.5, 1e-6);
}

TEST(Features, XZSketchExtrudesTowardsMinusY) {
  json doc{{"features", json::array({sketchF("Sketch1", rect("L", 0, 0, 10, 10), "XZ"), extrude("Extrude1", "Sketch1", 5)})}};
  const auto r = rebuild(doc);
  expectOk(r);
  GProp_GProps g;
  BRepGProp::VolumeProperties(r.body.shape(), g);
  EXPECT_NEAR(g.CentreOfMass().Y(), -2.5, 1e-6);
  EXPECT_NEAR(g.CentreOfMass().Z(), 5, 1e-6);
}

TEST(Features, SymmetricExtrude) {
  json doc{{"features", json::array({sketchF("Sketch1", rect("L", 0, 0, 10, 10)),
                                     {{"id", "Extrude1"}, {"type", "extrude"}, {"sketch", "Sketch1"}, {"distance", 8}, {"direction", "symmetric"}}})}};
  const auto r = rebuild(doc);
  expectOk(r);
  GProp_GProps g;
  BRepGProp::VolumeProperties(r.body.shape(), g);
  EXPECT_NEAR(g.Mass(), 800, 1e-6);
  EXPECT_NEAR(g.CentreOfMass().Z(), 0, 1e-6);
}

TEST(Features, SeparateRegionsAndIslandsInOneSketch) {
  // Two squares side by side; the second has a hole with an island inside it.
  json entities = rect("A", 0, 0, 10, 10);
  for (auto& e : rect("B", 20, 0, 40, 20)) entities.push_back(e);
  for (auto& e : rect("H", 24, 4, 36, 16)) entities.push_back(e);
  entities.push_back(circle("I", 30, 10, 2));
  const auto r = rebuild({{"features", json::array({sketchF("Sketch1", entities), extrude("Extrude1", "Sketch1", 1)})}});
  expectOk(r);
  EXPECT_EQ(solids(r), 3);
  EXPECT_NEAR(volume(r), 100 + (400 - 144) + kPi * 4, 1e-3);
}

TEST(Features, PrimitivesWithBooleans) {
  json doc{{"features", json::array({
      {{"id", "Box1"}, {"type", "box"}, {"length", 20}, {"width", 20}, {"height", 20}},
      {{"id", "Cyl1"}, {"type", "cylinder"}, {"radius", 5}, {"height", 20}, {"position", {10, 10, 0}}, {"op", "cut"}},
      {{"id", "Ball"}, {"type", "sphere"}, {"radius", 3}, {"position", {10, 10, 25}}}})}};
  const auto r = rebuild(doc);
  expectOk(r);
  EXPECT_NEAR(volume(r), 8000 - kPi * 25 * 20 + 4.0 / 3 * kPi * 27, 1e-2);
  EXPECT_TRUE(names(r).count("Box1/top"));
  EXPECT_TRUE(names(r).count("Cyl1/side"));
  EXPECT_TRUE(names(r).count("Ball/surface"));
}

TEST(Features, ParametersDriveDimensions) {
  json doc = block();
  doc["parameters"] = json::array({{{"name", "h"}, {"expression", "w / 4"}}, {{"name", "w"}, {"expression", "40"}}});
  doc["features"][1]["distance"] = "h * 2";
  const auto r = rebuild(doc);
  expectOk(r);
  EXPECT_NEAR(volume(r), 40 * 20 * 20, 1e-6);
}

TEST(Features, BadExpressionFailsTheFeatureWithAReason) {
  json doc = block();
  doc["features"][1]["distance"] = "nope + 1";
  const auto r = rebuild(doc);
  ASSERT_EQ(r.features.size(), 2u);
  EXPECT_EQ(r.features[1].state, FeatureState::Failed);
  EXPECT_NE(r.features[1].message.find("unknown name 'nope'"), std::string::npos) << r.features[1].message;
  EXPECT_EQ(r.features[1].message.rfind("distance:", 0), 0u) << r.features[1].message;
  EXPECT_FALSE(r.expressionErrors.empty());
}

TEST(Features, SuppressedFeatureIsSkipped) {
  json doc = block(json::array({{{"id", "Fillet1"}, {"type", "fillet"}, {"radius", 2}, {"suppressed", true},
                                 {"edges", {"Extrude1/end|Extrude1/side(Sketch1/L1)"}}}}));
  const auto r = rebuild(doc);
  EXPECT_EQ(r.features[2].state, FeatureState::Suppressed);
  EXPECT_NEAR(volume(r), 8000, 1e-6);
}

TEST(Features, RollbackStopsAfterAFeature) {
  json doc = block(json::array({sketchF("Sketch2", rect("B", 10, 5, 20, 15), "Extrude1/end"), extrude("Extrude2", "Sketch2", 5)}));
  RebuildOptions opt;
  opt.stopAfter = "Extrude1";
  const auto r = rebuild(doc, opt);
  EXPECT_NEAR(volume(r), 8000, 1e-6);
  EXPECT_EQ(r.features[3].state, FeatureState::Suppressed);
}

TEST(Features, CutThatRemovesEverythingFailsClearly) {
  const auto r = rebuild(block(json::array({{{"id", "Box1"}, {"type", "box"}, {"length", 100}, {"width", 100},
                                             {"height", 100}, {"position", {-10, -10, -10}}, {"op", "cut"}}})));
  EXPECT_EQ(r.features[2].state, FeatureState::Failed);
  EXPECT_NEAR(volume(r), 8000, 1e-6) << "the last good body is kept";
}

TEST(Expressions, Arithmetic) {
  ParamValues p{{"a", 3}, {"b", 4}};
  EXPECT_DOUBLE_EQ(evaluate("a * b + 2", p), 14);
  EXPECT_DOUBLE_EQ(evaluate("-(a + b) / 2", p), -3.5);
  EXPECT_DOUBLE_EQ(evaluate("2 ^ 3 ^ 2", p), 512);  // right associative
  EXPECT_DOUBLE_EQ(evaluate("sqrt(a^2 + b^2)", p), 5);
  EXPECT_NEAR(evaluate("sin(30)", p), 0.5, 1e-12);  // degrees
  EXPECT_DOUBLE_EQ(evaluate("max(a, b, 1)", p), 4);
  EXPECT_THROW(evaluate("a +", p), ExprError);
  EXPECT_THROW(evaluate("c", p), ExprError);
  EXPECT_THROW(evaluate("1 / 0", p), ExprError);
}

TEST(Expressions, ParameterCycleIsAnError) {
  json params = json::array({{{"name", "a"}, {"expression", "b + 1"}}, {{"name", "b"}, {"expression", "a"}}});
  EXPECT_THROW(evaluateParameters(params), ExprError);
}
