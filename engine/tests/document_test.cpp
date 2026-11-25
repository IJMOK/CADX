// Document, undo/redo and RPC session tests.

#include "document.hpp"
#include "rpc.hpp"

#include <gtest/gtest.h>

using nlohmann::json;
using namespace cadx;

namespace {

json rectSketch() {
  auto line = [](const char* id, double ax, double ay, double bx, double by) {
    return json{{"id", id}, {"type", "line"}, {"a", {ax, ay}}, {"b", {bx, by}}};
  };
  return {{"type", "sketch"}, {"plane", "XY"},
          {"entities", json::array({line("L1", 0, 0, 10, 0), line("L2", 10, 0, 10, 10), line("L3", 10, 10, 0, 10),
                                    line("L4", 0, 10, 0, 0)})},
          {"constraints", json::array()}};
}

}  // namespace

TEST(Document, AddEditDeleteWithUndoRedo) {
  Document d;
  const std::string s = d.addFeature(rectSketch());
  EXPECT_EQ(s, "Sketch1");
  const std::string e = d.addFeature({{"type", "extrude"}, {"sketch", s}, {"distance", 5}});
  EXPECT_EQ(e, "Extrude1");
  d.editFeature(e, {{"distance", 8}});
  EXPECT_EQ(d.data()["features"][1]["distance"], 8);
  EXPECT_EQ(d.undoLabel(), "Edit Extrude1");
  ASSERT_TRUE(d.undo());
  EXPECT_EQ(d.data()["features"][1]["distance"], 5);
  ASSERT_TRUE(d.redo());
  EXPECT_EQ(d.data()["features"][1]["distance"], 8);
  d.deleteFeature(e);
  EXPECT_EQ(d.data()["features"].size(), 1u);
  d.undo();
  EXPECT_EQ(d.data()["features"].size(), 2u);
}

TEST(Document, NewChangeClearsRedo) {
  Document d;
  d.addFeature(rectSketch());
  d.undo();
  EXPECT_TRUE(d.canRedo());
  d.addFeature(rectSketch());
  EXPECT_FALSE(d.canRedo());
}

TEST(Document, MoveFeature) {
  Document d;
  d.addFeature(rectSketch());
  d.addFeature({{"type", "box"}, {"length", 1}, {"width", 1}, {"height", 1}});
  d.moveFeature("Box1", 0);
  EXPECT_EQ(d.data()["features"][0]["id"], "Box1");
}

TEST(Document, LegacySketchesMoveIntoTheTree) {
  json legacy{{"sketches", json::array({{{"id", "Sketch1"}, {"entities", json::array()}}})},
              {"features", json::array({{{"id", "Extrude1"}, {"type", "extrude"}, {"sketch", "Sketch1"}, {"distance", 1}}})}};
  const json n = Document::normalise(legacy);
  ASSERT_EQ(n["features"].size(), 2u);
  EXPECT_EQ(n["features"][0]["id"], "Sketch1");
  EXPECT_EQ(n["features"][0]["type"], "sketch");
  EXPECT_FALSE(n.contains("sketches"));
  EXPECT_EQ(n["format"], "cadx-part");
}

TEST(Document, RejectsNewerFormat) {
  EXPECT_THROW(Document::normalise({{"format", "cadx-part"}, {"version", 99}}), DocumentError);
}

TEST(Document, ParameterNamesAreChecked) {
  Document d;
  EXPECT_THROW(d.setParameters(json::array({{{"name", "2x"}, {"expression", "1"}}})), DocumentError);
  EXPECT_THROW(d.setParameters(json::array({{{"name", "a"}, {"expression", "1"}}, {{"name", "a"}, {"expression", "2"}}})),
               DocumentError);
}

TEST(Session, BuildsThroughTheRpcInterface) {
  Session s;
  s.handle("newDocument", {{"name", "Test"}});
  json st = s.handle("addFeature", {{"feature", rectSketch()}});
  EXPECT_EQ(st["id"], "Sketch1");
  st = s.handle("addFeature", {{"feature", {{"type", "extrude"}, {"sketch", "Sketch1"}, {"distance", 5}}}});
  ASSERT_TRUE(st.contains("mesh"));
  EXPECT_EQ(st["features"][1]["state"], "ok");
  EXPECT_EQ(st["faceNames"].size(), 6u);
  EXPECT_EQ(st["mesh"]["edgeIds"].size() * 6, st["mesh"]["edges"].size());
  EXPECT_FALSE(st["sketches"][0]["placement"].is_null());

  const json m = s.handle("measure", json::object());
  EXPECT_NEAR(m["volume"].get<double>(), 500, 1e-6);

  // Preview changes nothing.
  const int rev = st["revision"];
  json pv = s.handle("preview", {{"feature", {{"id", "Extrude1"}, {"distance", 10}}}});
  EXPECT_EQ(pv["revision"], rev);
  EXPECT_NEAR(s.handle("measure", json::object())["volume"].get<double>(), 500, 1e-6);

  // Rollback, then add: the new feature goes in at the rollback point.
  s.handle("setRollback", {{"id", "Sketch1"}});
  st = s.handle("addFeature", {{"feature", {{"type", "box"}, {"length", 1}, {"width", 1}, {"height", 1}}}});
  EXPECT_EQ(st["document"]["features"][1]["id"], "Box1");
  EXPECT_EQ(st["rollback"], "Box1");
  s.handle("setRollback", {{"id", nullptr}});

  st = s.handle("undo", json::object());
  EXPECT_EQ(st["document"]["features"].size(), 2u);
  EXPECT_THROW(s.handle("noSuchMethod", json::object()), MethodNotFound);
}
