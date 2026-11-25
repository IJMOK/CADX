#pragma once
// JSON-RPC API v0. One Session per engine process.
//
// Document methods (each returns the full state, see stateJson below):
//   newDocument {name?}                    openDocument {document}
//   addFeature {feature, after?}           editFeature {id, feature (merge patch)}
//   deleteFeature {id}                     moveFeature {id, index}
//   setParameters {parameters}             setName {name}
//   setRollback {id | null}                undo · redo · getState
//   importFile {path, op?}                 (STEP / IGES / BREP becomes an import feature)
// Queries:
//   ping · getDocument · preview {feature, after?} (state with the feature applied, nothing recorded)
//   solveSketch {sketch} · measure · exportFile {path, format?: step|iges|stl|brep}
//   rebuild {document} (stateless rebuild of a given document, for tests and tools)
//
// State: { revision, name, document, canUndo, undoLabel, canRedo, redoLabel, rollback,
//          features: [{id, state, message}], sketches: [{state, dof, ..., sketch, placement}],
//          datums: [{id, origin, xDir, yDir, normal}], parameterValues: {name: value},
//          expressionErrors: [...], mesh?, faceNames?, edgeNames? }

#include "document.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace cadx {

struct MethodNotFound : std::invalid_argument {
  using std::invalid_argument::invalid_argument;
};

class Session {
 public:
  // Throws MethodNotFound for an unknown method; other exceptions are errors.
  nlohmann::json handle(const std::string& method, const nlohmann::json& params);

  Document& document() { return doc_; }

 private:
  nlohmann::json state(const nlohmann::json* overrideDoc = nullptr);
  Document doc_;
  std::string rollback_;
};

}  // namespace cadx
