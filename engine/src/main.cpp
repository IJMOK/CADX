// CADX engine: line-delimited JSON-RPC 2.0 over stdin/stdout.
// One request per line in, one response per line out. Logs go to stderr.

#include "mesh.hpp"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <Standard_Failure.hxx>

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

using nlohmann::json;

namespace {

constexpr const char* kVersion = "0.1.0";

json meshToJson(const cadx::Mesh& m) {
  return json{{"positions", m.positions},
              {"indices", m.indices},
              {"faceIds", m.faceIds},
              {"edges", m.edges}};
}

json handle(const std::string& method, const json& params) {
  if (method == "ping") {
    return json{{"engine", "cadx"}, {"version", kVersion}};
  }
  if (method == "makeBox") {
    const double x = params.value("x", 40.0);
    const double y = params.value("y", 20.0);
    const double z = params.value("z", 10.0);
    return meshToJson(cadx::tessellate(BRepPrimAPI_MakeBox(x, y, z).Shape()));
  }
  if (method == "makeCylinder") {
    const double r = params.value("r", 10.0);
    const double h = params.value("h", 30.0);
    return meshToJson(cadx::tessellate(BRepPrimAPI_MakeCylinder(r, h).Shape()));
  }
  throw std::invalid_argument("method not found: " + method);
}

json error(const json& id, int code, const std::string& message) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

}  // namespace

int main() {
  std::ios::sync_with_stdio(false);
  std::cerr << "cadx-engine " << kVersion << " ready" << std::endl;

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    json id = nullptr;
    json reply;
    try {
      const json req = json::parse(line);
      id = req.value("id", json(nullptr));
      const std::string method = req.at("method").get<std::string>();
      const json params = req.value("params", json::object());
      reply = json{{"jsonrpc", "2.0"}, {"id", id}, {"result", handle(method, params)}};
    } catch (const json::parse_error& e) {
      reply = error(nullptr, -32700, e.what());
    } catch (const std::invalid_argument& e) {
      reply = error(id, -32601, e.what());
    } catch (const Standard_Failure& e) {
      reply = error(id, -32000, std::string("geometry error: ") + e.GetMessageString());
    } catch (const std::exception& e) {
      reply = error(id, -32603, e.what());
    }
    std::cout << reply.dump() << '\n' << std::flush;
  }
  return 0;
}
