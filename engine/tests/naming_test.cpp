// Persistent naming regression suite: edit something upstream, check that
// downstream references still land on the right geometry, or fail loudly.

#include "part.hpp"

#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Bnd_Box.hxx>
#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>

using nlohmann::json;
using namespace cadx;

namespace {

const std::string kTopFrontEdge = "Extrude1/end|Extrude1/side(Sketch1/L1)";
const std::string kBlend = "Fillet1/blend(" + kTopFrontEdge + ")";

json seg(const std::string& id, double ax, double ay, double bx, double by) {
  return {{"id", id}, {"type", "line"}, {"a", json::array({ax, ay})}, {"b", json::array({bx, by})}};
}

// Rectangle w x h: L1 front (y=0), L2 right, L3 back, L4 left.
json rectangle(double w, double h) {
  return json::array({seg("L1", 0, 0, w, 0), seg("L2", w, 0, w, h),
                      seg("L3", w, h, 0, h), seg("L4", 0, h, 0, 0)});
}

json part(const json& segments, const json& extraFeatures = json::array()) {
  json features = json::array(
      {{{"id", "Extrude1"}, {"type", "extrude"}, {"sketch", "Sketch1"}, {"distance", 10}}});
  for (const auto& f : extraFeatures) features.push_back(f);
  return {{"sketches", json::array({{{"id", "Sketch1"}, {"entities", segments}}})},
          {"features", features}};
}

json filletOn(const std::string& id, const std::string& edge, double r = 2) {
  return {{"id", id}, {"type", "fillet"}, {"radius", r}, {"edges", json::array({edge})}};
}

std::set<std::string> faceNames(const RebuildResult& r) {
  std::set<std::string> out;
  for (const auto& f : r.body.faces()) out.insert(f.name);
  return out;
}

Bnd_Box boundsOf(const RebuildResult& r, const std::string& faceName) {
  Bnd_Box box;
  for (const auto& f : r.body.faces()) {
    if (f.name == faceName) BRepBndLib::Add(f.face, box);
  }
  return box;
}

void expectAllOk(const RebuildResult& r) {
  for (const auto& f : r.features) {
    EXPECT_EQ(f.state, FeatureState::Ok) << f.id << ": " << f.message;
  }
  if (!r.body.empty()) {
    EXPECT_TRUE(BRepCheck_Analyzer(r.body.shape()).IsValid()) << "OCCT says the solid is invalid";
  }
}

// The blend for the top-front edge must hug y = 0 and z = 10, whatever the sketch size.
void expectBlendOnTopFrontEdge(const RebuildResult& r, double radius = 2) {
  const Bnd_Box box = boundsOf(r, kBlend);
  ASSERT_FALSE(box.IsVoid()) << "no face named " << kBlend;
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  const double tol = 0.05;
  EXPECT_NEAR(y0, 0.0, tol);
  EXPECT_NEAR(y1, radius, tol);
  EXPECT_NEAR(z0, 10.0 - radius, tol);
  EXPECT_NEAR(z1, 10.0, tol);
}

}  // namespace

TEST(Naming, ExtrudeNamesEveryFace) {
  const auto r = rebuild(part(rectangle(40, 20)));
  expectAllOk(r);
  EXPECT_EQ(faceNames(r), (std::set<std::string>{
                              "Extrude1/start", "Extrude1/end",
                              "Extrude1/side(Sketch1/L1)", "Extrude1/side(Sketch1/L2)",
                              "Extrude1/side(Sketch1/L3)", "Extrude1/side(Sketch1/L4)"}));
  EXPECT_EQ(r.body.faces().size(), 6u);
}

TEST(Naming, EdgeNamesAreUnique) {
  const auto r = rebuild(part(rectangle(40, 20)));
  const auto names = r.body.edgeNames();
  EXPECT_EQ(names.size(), 12u);
  EXPECT_EQ(std::set<std::string>(names.begin(), names.end()).size(), names.size());
  EXPECT_NE(std::find(names.begin(), names.end(), kTopFrontEdge), names.end());
}

TEST(Naming, FilletResolvesNamedEdge) {
  const auto r = rebuild(part(rectangle(40, 20), json::array({filletOn("Fillet1", kTopFrontEdge)})));
  expectAllOk(r);
  EXPECT_EQ(r.body.faces().size(), 7u);
  EXPECT_TRUE(faceNames(r).count(kBlend));
  expectBlendOnTopFrontEdge(r);
}

TEST(Naming, FilletSurvivesDimensionEdit) {
  const json fillet = json::array({filletOn("Fillet1", kTopFrontEdge)});
  for (auto [w, h] : {std::pair{40.0, 20.0}, {60.0, 30.0}, {15.0, 50.0}}) {
    const auto r = rebuild(part(rectangle(w, h), fillet));
    SCOPED_TRACE("rectangle " + std::to_string(w) + " x " + std::to_string(h));
    expectAllOk(r);
    expectBlendOnTopFrontEdge(r);
  }
}

TEST(Naming, FilletSurvivesAddedSegment) {
  // Chamfer the back-right corner of the sketch with a new segment L5:
  // the face count changes, the top-front edge must still be found.
  const json pentagon = json::array({seg("L1", 0, 0, 40, 0), seg("L2", 40, 0, 40, 10),
                                     seg("L5", 40, 10, 30, 20), seg("L3", 30, 20, 0, 20),
                                     seg("L4", 0, 20, 0, 0)});
  const auto r = rebuild(part(pentagon, json::array({filletOn("Fillet1", kTopFrontEdge)})));
  expectAllOk(r);
  EXPECT_EQ(r.body.faces().size(), 8u);
  expectBlendOnTopFrontEdge(r);
}

TEST(Naming, SegmentOrderDoesNotMatter) {
  const json rotated = json::array({seg("L3", 40, 20, 0, 20), seg("L4", 0, 20, 0, 0),
                                    seg("L1", 0, 0, 40, 0), seg("L2", 40, 0, 40, 20)});
  const auto r = rebuild(part(rotated, json::array({filletOn("Fillet1", kTopFrontEdge)})));
  expectAllOk(r);
  expectBlendOnTopFrontEdge(r);
}

TEST(Naming, ReversedSketchDirectionKeepsNames) {
  // Same rectangle drawn clockwise.
  const json cw = json::array({seg("L4", 0, 0, 0, 20), seg("L3", 0, 20, 40, 20),
                               seg("L2", 40, 20, 40, 0), seg("L1", 40, 0, 0, 0)});
  const auto r = rebuild(part(cw, json::array({filletOn("Fillet1", kTopFrontEdge)})));
  expectAllOk(r);
  expectBlendOnTopFrontEdge(r);
}

TEST(Naming, SecondFilletFollowsTangentChainAndNamesEveryFace) {
  // Fillet the front-right vertical edge after the top-front fillet. OCCT
  // carries the blend along the tangent chain (over the first blend and the
  // top-right edge); every resulting face must still get a unique name.
  const std::string vertical = "Extrude1/side(Sketch1/L1)|Extrude1/side(Sketch1/L2)";
  const json features = json::array({filletOn("Fillet1", kTopFrontEdge), filletOn("Fillet2", vertical, 1)});
  const auto r = rebuild(part(rectangle(40, 20), features));
  expectAllOk(r);
  const auto names = faceNames(r);
  EXPECT_EQ(names.size(), r.body.faces().size()) << "face names must be unique";
  EXPECT_TRUE(names.count(kBlend));
  EXPECT_TRUE(names.count("Fillet2/blend(" + vertical + ")"));
  EXPECT_TRUE(names.count("Fillet2/blend(Extrude1/side(Sketch1/L2)|" + kBlend + ")"));
}

TEST(Naming, SecondFilletSurvivesDimensionEdit) {
  const std::string vertical = "Extrude1/side(Sketch1/L1)|Extrude1/side(Sketch1/L2)";
  const json features = json::array({filletOn("Fillet1", kTopFrontEdge), filletOn("Fillet2", vertical, 1)});
  for (auto [w, h] : {std::pair{40.0, 20.0}, {70.0, 35.0}}) {
    const auto r = rebuild(part(rectangle(w, h), features));
    SCOPED_TRACE("rectangle " + std::to_string(w) + " x " + std::to_string(h));
    expectAllOk(r);
    // The vertical blend sits at the front-right corner: x near w, y near 0.
    const Bnd_Box box = boundsOf(r, "Fillet2/blend(" + vertical + ")");
    ASSERT_FALSE(box.IsVoid());
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x1, w, 0.05);
    EXPECT_NEAR(y0, 0.0, 0.05);
  }
}

TEST(Naming, MissingReferenceFailsLoudlyAndIsNotRebound) {
  // L1 is replaced by a segment with a new id: the fillet's edge is gone.
  json segments = rectangle(40, 20);
  segments[0]["id"] = "L9";
  const auto r = rebuild(part(segments, json::array({filletOn("Fillet1", kTopFrontEdge)})));
  ASSERT_EQ(r.features.size(), 2u);
  EXPECT_EQ(r.features[0].state, FeatureState::Ok);
  EXPECT_EQ(r.features[1].state, FeatureState::Failed);
  EXPECT_NE(r.features[1].message.find("edge not found"), std::string::npos);
  EXPECT_EQ(r.body.faces().size(), 6u);  // last good body: the plain extrude
  EXPECT_FALSE(faceNames(r).count(kBlend));
}

TEST(Naming, FailureSuppressesDownstreamFeatures) {
  const json features = json::array({filletOn("Fillet1", "no|such-edge"),
                                     filletOn("Fillet2", kTopFrontEdge)});
  const auto r = rebuild(part(rectangle(40, 20), features));
  ASSERT_EQ(r.features.size(), 3u);
  EXPECT_EQ(r.features[1].state, FeatureState::Failed);
  EXPECT_EQ(r.features[2].state, FeatureState::Suppressed);
}

TEST(Naming, OversizedFilletFailsWithoutCrashing) {
  const auto r = rebuild(part(rectangle(40, 20), json::array({filletOn("Fillet1", kTopFrontEdge, 50)})));
  EXPECT_EQ(r.features[1].state, FeatureState::Failed);
  EXPECT_EQ(r.body.faces().size(), 6u);
}

// ------------------------------------------------- solved, constrained sketches

namespace {

json con(const std::string& id, const std::string& type, std::vector<std::string> refs, double value = 0) {
  json c{{"id", id}, {"type", type}, {"refs", refs}};
  if (value != 0) c["value"] = value;
  return c;
}

// A roughly drawn rectangle, constrained to w x h at the origin.
json constrainedPart(double w, double h, const json& extraFeatures, const json& extraEntities = json::array(),
                     const json& extraConstraints = json::array()) {
  json entities = json::array({seg("L1", 0.4, -0.3, 37, 1), seg("L2", 37, 1, 42, 23), seg("L3", 42, 23, -2, 18),
                               seg("L4", -2, 18, 0.4, -0.3)});
  for (const auto& e : extraEntities) entities.push_back(e);
  json constraints = json::array({con("c1", "coincident", {"L1.end", "L2.start"}),
                                  con("c2", "coincident", {"L2.end", "L3.start"}),
                                  con("c3", "coincident", {"L3.end", "L4.start"}),
                                  con("c4", "coincident", {"L4.end", "L1.start"}),
                                  con("h1", "horizontal", {"L1"}), con("h2", "horizontal", {"L3"}),
                                  con("v1", "vertical", {"L2"}), con("v2", "vertical", {"L4"}),
                                  con("o1", "coincident", {"L1.start", "origin"}),
                                  con("dW", "distance", {"L1"}, w), con("dH", "distance", {"L2"}, h)});
  for (const auto& c : extraConstraints) constraints.push_back(c);
  json features = json::array({{{"id", "Extrude1"}, {"type", "extrude"}, {"sketch", "Sketch1"}, {"distance", 10}}});
  for (const auto& f : extraFeatures) features.push_back(f);
  return {{"sketches", json::array({{{"id", "Sketch1"}, {"entities", entities}, {"constraints", constraints}}})},
          {"features", features}};
}

}  // namespace

TEST(SolvedSketch, FilletSurvivesEditingASketchDimension) {
  for (auto [w, h] : {std::pair{40.0, 20.0}, {75.0, 12.0}}) {
    const auto r = rebuild(constrainedPart(w, h, json::array({filletOn("Fillet1", kTopFrontEdge)})));
    SCOPED_TRACE("dimensions " + std::to_string(w) + " x " + std::to_string(h));
    ASSERT_EQ(r.sketches.size(), 1u);
    EXPECT_EQ(r.sketches[0].state, sketch::SolveState::Solved);
    EXPECT_EQ(r.sketches[0].dof, 0);
    expectAllOk(r);
    expectBlendOnTopFrontEdge(r);
    double x0, y0, z0, x1, y1, z1;
    boundsOf(r, kBlend).Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x1 - x0, w, 0.05);  // blend runs the full (new) width
  }
}

TEST(SolvedSketch, CircleInsideBecomesAHoleWithItsOwnName) {
  const json circle = {{"id", "C1"}, {"type", "circle"}, {"center", {10, 8}}, {"radius", 3}};
  const json place = json::array({con("cx", "distanceX", {"C1.center"}, 30), con("cy", "distanceY", {"C1.center"}, 10),
                                  con("cd", "diameter", {"C1"}, 8)});
  const auto r = rebuild(constrainedPart(40, 20, json::array(), json::array({circle}), place));
  expectAllOk(r);
  EXPECT_EQ(r.body.faces().size(), 7u);
  const Bnd_Box box = boundsOf(r, "Extrude1/side(Sketch1/C1)");
  ASSERT_FALSE(box.IsVoid());
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  EXPECT_NEAR((x0 + x1) / 2, 30, 0.05);
  EXPECT_NEAR(x1 - x0, 8, 0.05);
  // The hole wall and the top face share a named edge (a circle edge has a seam too).
  const auto edges = r.body.edgeNames();
  EXPECT_NE(std::find(edges.begin(), edges.end(), "Extrude1/end|Extrude1/side(Sketch1/C1)"), edges.end());
}

TEST(SolvedSketch, SlotProfileWithArcs) {
  const json entities = json::array(
      {seg("T", 0, 10, 50, 10), seg("B", 50, -10, 0, -10),
       {{"id", "AR"}, {"type", "arc"}, {"center", {50, 0}}, {"radius", 10}, {"start", -90}, {"end", 90}},
       {{"id", "AL"}, {"type", "arc"}, {"center", {0, 0}}, {"radius", 10}, {"start", 90}, {"end", 270}}});
  const json constraints = json::array(
      {con("t1", "tangent", {"T.end", "AR.end"}), con("t2", "tangent", {"AR.start", "B.start"}),
       con("t3", "tangent", {"B.end", "AL.end"}), con("t4", "tangent", {"AL.start", "T.start"}),
       con("h1", "horizontal", {"T"}), con("o1", "coincident", {"AL.center", "origin"}),
       con("r1", "radius", {"AL"}, 6), con("eq", "equal", {"AL", "AR"}),
       con("len", "distance", {"AL.center", "AR.center"}, 30)});
  const json doc = {{"sketches", json::array({{{"id", "Sketch1"}, {"entities", entities}, {"constraints", constraints}}})},
                    {"features", json::array({{{"id", "Extrude1"}, {"type", "extrude"}, {"sketch", "Sketch1"}, {"distance", 5}}})}};
  const auto r = rebuild(doc);
  expectAllOk(r);
  EXPECT_EQ(faceNames(r), (std::set<std::string>{"Extrude1/start", "Extrude1/end", "Extrude1/side(Sketch1/T)",
                                                 "Extrude1/side(Sketch1/B)", "Extrude1/side(Sketch1/AL)",
                                                 "Extrude1/side(Sketch1/AR)"}));
  double x0, y0, z0, x1, y1, z1;
  boundsOf(r, "Extrude1/side(Sketch1/AR)").Get(x0, y0, z0, x1, y1, z1);
  EXPECT_NEAR(x1, 36, 0.05);  // right arc: centre 30, radius 6
}

TEST(SolvedSketch, OpenProfileFailsTheExtrudeWithAReason) {
  json segments = rectangle(40, 20);
  segments.erase(segments.begin() + 2);  // remove L3
  const auto r = rebuild(part(segments));
  EXPECT_EQ(r.features[0].state, FeatureState::Failed);
  EXPECT_NE(r.features[0].message.find("open"), std::string::npos) << r.features[0].message;
}

TEST(SolvedSketch, ConflictingSketchFailsTheExtrudeAndNamesTheConstraints) {
  const auto r = rebuild(constrainedPart(40, 20, json::array(), json::array(),
                                         json::array({con("bad", "distance", {"L3"}, 55)})));
  EXPECT_EQ(r.sketches[0].state, sketch::SolveState::Conflicting);
  EXPECT_EQ(r.features[0].state, FeatureState::Failed);
  EXPECT_NE(r.features[0].message.find("bad"), std::string::npos) << r.features[0].message;
}

TEST(SolvedSketch, ConstructionGeometryDoesNotMakeFaces) {
  json helper = seg("K1", 0, 0, 40, 20);
  helper["construction"] = true;
  const auto r = rebuild(constrainedPart(40, 20, json::array(), json::array({helper})));
  expectAllOk(r);
  EXPECT_EQ(r.body.faces().size(), 6u);
}
