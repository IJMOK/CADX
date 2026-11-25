#pragma once
// The open part document, owned by the engine, with undo and redo.
//
// The document is plain JSON (see part.hpp for its shape). Every change goes
// through apply(), which records the previous state for undo. The engine
// rebuilds geometry from the document; nothing geometric is stored here.

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cadx {

struct DocumentError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Document {
 public:
  Document();

  const nlohmann::json& data() const { return doc_; }
  int revision() const { return revision_; }

  // Replace the whole document (open or new). Clears undo history.
  void load(nlohmann::json doc);
  void reset(const std::string& name = "Part1");

  // Record a change.
  void apply(nlohmann::json next, const std::string& label);

  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  std::string undoLabel() const { return undo_.empty() ? "" : undo_.back().label; }
  std::string redoLabel() const { return redo_.empty() ? "" : redo_.back().label; }
  bool undo();
  bool redo();

  // Editing helpers. Each records one undo step and returns the affected feature id.
  std::string addFeature(nlohmann::json feature, const std::string& afterId = "");
  void editFeature(const std::string& id, const nlohmann::json& patch);
  void deleteFeature(const std::string& id);
  void moveFeature(const std::string& id, int index);
  void setParameters(const nlohmann::json& parameters);
  void setName(const std::string& name);

  // The document with `feature` added (or, if its id exists, merged into the
  // existing one), without recording anything. For live previews.
  nlohmann::json withFeature(const nlohmann::json& feature, const std::string& afterId = "") const;

  std::string uniqueId(const std::string& prefix) const;
  int indexOf(const std::string& id) const;  // -1 if absent

  // Bring an older or hand-written document to the current format: features
  // array, parameters array, legacy top-level sketches moved into the tree.
  static nlohmann::json normalise(nlohmann::json doc);

 private:
  struct Step {
    nlohmann::json doc;
    std::string label;
  };
  nlohmann::json doc_;
  std::vector<Step> undo_, redo_;
  int revision_ = 0;
};

// Default id prefix for a feature type ("extrude" -> "Extrude").
std::string idPrefixFor(const std::string& type);

}  // namespace cadx
