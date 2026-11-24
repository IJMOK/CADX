// Persistent naming regression suite: edit something upstream, check that
// downstream references still land on the right geometry, or fail loudly.

#include "part.hpp"

#include <BRepBndLib.hxx>
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
  return {{"id", id}, {"a", json::array({ax, ay})}, {"b", json::array({bx, by})}};
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
  return {{"sketches", json::array({{{"id", "Sketch1"}, {"segments", segments}}})},
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
