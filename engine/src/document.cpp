#include "document.hpp"

#include "expr.hpp"

#include <algorithm>
#include <map>
#include <set>

using nlohmann::json;

namespace cadx {

namespace {

constexpr std::size_t kUndoLimit = 200;

json emptyDocument(const std::string& name) {
  return json{{"format", "cadx-part"},
              {"version", 1},
              {"name", name},
              {"units", "mm"},
              {"parameters", json::array()},
              {"features", json::array()}};
}

}  // namespace

std::string idPrefixFor(const std::string& type) {
  static const std::map<std::string, std::string> prefixes{
      {"sketch", "Sketch"},          {"extrude", "Extrude"},     {"revolve", "Revolve"},
      {"hole", "Hole"},              {"fillet", "Fillet"},       {"chamfer", "Chamfer"},
      {"shell", "Shell"},            {"linearPattern", "Pattern"}, {"circularPattern", "Pattern"},
      {"mirror", "Mirror"},          {"datumPlane", "Plane"},    {"box", "Box"},
      {"cylinder", "Cylinder"},      {"sphere", "Sphere"},       {"cone", "Cone"},
      {"import", "Import"}};
  auto it = prefixes.find(type);
  return it == prefixes.end() ? "Feature" : it->second;
}

Document::Document() { reset(); }

json Document::normalise(json doc) {
  if (!doc.is_object()) throw DocumentError("a document must be a JSON object");
  if (doc.contains("format") && doc.at("format") != "cadx-part") {
    throw DocumentError("not a CADX part (format " + doc.at("format").dump() + ")");
  }
  if (doc.value("version", 1) > 1) {
    throw DocumentError("this part was saved by a newer CADX (format version " +
                        std::to_string(doc.value("version", 1)) + ")");
  }
  doc["format"] = "cadx-part";
  doc["version"] = 1;
  if (!doc.contains("name")) doc["name"] = "Part1";
  if (!doc.contains("units")) doc["units"] = "mm";
  if (!doc.contains("parameters") || !doc.at("parameters").is_array()) doc["parameters"] = json::array();
  if (!doc.contains("features") || !doc.at("features").is_array()) doc["features"] = json::array();

  // Legacy sketches: move each in front of the first feature that uses it.
  if (doc.contains("sketches")) {
    json features = doc.at("features");
    for (const auto& sk : doc.at("sketches")) {
      json s = sk;
      s["type"] = "sketch";
      const std::string id = s.value("id", std::string());
      std::size_t at = features.size();
      for (std::size_t i = 0; i < features.size(); ++i) {
        if (features[i].value("sketch", std::string()) == id) { at = i; break; }
      }
      // Sketches nobody uses go before the first feature that is not a sketch.
      if (at == features.size()) {
        at = 0;
        while (at < features.size() && features[at].value("type", std::string()) == "sketch") ++at;
      }
      features.insert(features.begin() + static_cast<long>(at), s);
    }
    doc["features"] = features;
    doc.erase("sketches");
  }
  return doc;
}

void Document::load(json doc) {
  doc_ = normalise(std::move(doc));
  undo_.clear();
  redo_.clear();
  ++revision_;
}

void Document::reset(const std::string& name) { load(emptyDocument(name)); }

void Document::apply(json next, const std::string& label) {
  undo_.push_back({doc_, label});
  if (undo_.size() > kUndoLimit) undo_.erase(undo_.begin());
  redo_.clear();
  doc_ = std::move(next);
  ++revision_;
}

bool Document::undo() {
  if (undo_.empty()) return false;
  redo_.push_back({doc_, undo_.back().label});
  doc_ = std::move(undo_.back().doc);
  undo_.pop_back();
  ++revision_;
  return true;
}

bool Document::redo() {
  if (redo_.empty()) return false;
  undo_.push_back({doc_, redo_.back().label});
  doc_ = std::move(redo_.back().doc);
  redo_.pop_back();
  ++revision_;
  return true;
}

int Document::indexOf(const std::string& id) const {
  const json& fs = doc_.at("features");
  for (std::size_t i = 0; i < fs.size(); ++i) {
    if (fs[i].value("id", std::string()) == id) return static_cast<int>(i);
  }
  return -1;
}

std::string Document::uniqueId(const std::string& prefix) const {
  std::set<std::string> used;
  for (const auto& f : doc_.at("features")) used.insert(f.value("id", std::string()));
  for (int n = 1;; ++n) {
    std::string id = prefix + std::to_string(n);
    if (!used.count(id)) return id;
  }
}

json Document::withFeature(const json& feature, const std::string& afterId) const {
  if (!feature.is_object()) throw DocumentError("a feature must be a JSON object");
  json next = doc_;
  json& fs = next["features"];
  const std::string id = feature.value("id", std::string());
  if (!id.empty()) {
    const int i = indexOf(id);
    if (i >= 0) {
      fs[i].merge_patch(feature);
      return next;
    }
  }
  json f = feature;
  if (id.empty()) f["id"] = uniqueId(idPrefixFor(f.value("type", std::string())));
  if (!afterId.empty()) {
    const int i = indexOf(afterId);
    if (i < 0) throw DocumentError("no feature " + afterId);
    fs.insert(fs.begin() + i + 1, f);
  } else {
    fs.push_back(f);
  }
  return next;
}

std::string Document::addFeature(json feature, const std::string& afterId) {
  if (!feature.is_object() || !feature.contains("type")) throw DocumentError("a feature needs a type");
  std::string id = feature.value("id", std::string());
  if (id.empty()) {
    id = uniqueId(idPrefixFor(feature.at("type").get<std::string>()));
    feature["id"] = id;
  } else if (indexOf(id) >= 0) {
    throw DocumentError("there is already a feature " + id);
  }
  if (id.find_first_of("|()/[] .") != std::string::npos) throw DocumentError("invalid feature id: " + id);
  apply(withFeature(feature, afterId), "Add " + id);
  return id;
}

void Document::editFeature(const std::string& id, const json& patch) {
  const int i = indexOf(id);
  if (i < 0) throw DocumentError("no feature " + id);
  if (patch.contains("id") && patch.at("id") != id) throw DocumentError("a feature's id cannot be changed; edit its name");
  json next = doc_;
  next["features"][i].merge_patch(patch);
  if (next == doc_) return;
  std::string label = "Edit " + id;
  if (patch.size() == 1 && patch.contains("suppressed")) {
    label = (patch.at("suppressed").get<bool>() ? "Suppress " : "Unsuppress ") + id;
  }
  apply(std::move(next), label);
}

void Document::deleteFeature(const std::string& id) {
  const int i = indexOf(id);
  if (i < 0) throw DocumentError("no feature " + id);
  json next = doc_;
  next["features"].erase(static_cast<std::size_t>(i));
  apply(std::move(next), "Delete " + id);
}

void Document::moveFeature(const std::string& id, int index) {
  const int i = indexOf(id);
  if (i < 0) throw DocumentError("no feature " + id);
  json next = doc_;
  json& fs = next["features"];
  const int n = static_cast<int>(fs.size());
  index = std::max(0, std::min(index, n - 1));
  if (index == i) return;
  json f = fs[i];
  fs.erase(static_cast<std::size_t>(i));
  fs.insert(fs.begin() + index, f);
  apply(std::move(next), "Move " + id);
}

void Document::setParameters(const json& parameters) {
  if (!parameters.is_array()) throw DocumentError("parameters must be an array");
  std::set<std::string> names;
  for (const auto& p : parameters) {
    const std::string name = p.value("name", std::string());
    if (!isValidParameterName(name)) throw DocumentError("invalid parameter name '" + name + "'");
    if (!names.insert(name).second) throw DocumentError("duplicate parameter '" + name + "'");
  }
  json next = doc_;
  next["parameters"] = parameters;
  if (next == doc_) return;
  apply(std::move(next), "Edit parameters");
}

void Document::setName(const std::string& name) {
  json next = doc_;
  next["name"] = name;
  if (next == doc_) return;
  apply(std::move(next), "Rename part");
}

}  // namespace cadx
