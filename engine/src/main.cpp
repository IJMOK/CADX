// CADX engine: line-delimited JSON-RPC 2.0 over stdin/stdout.
// One request per line in, one response per line out. Logs go to stderr.
// The methods are documented in rpc.hpp.

#include "rpc.hpp"

#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <Standard_Failure.hxx>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define cadx_dup _dup
#define cadx_dup2 _dup2
#define cadx_fileno _fileno
#define cadx_fdopen _fdopen
#else
#include <unistd.h>
#define cadx_dup dup
#define cadx_dup2 dup2
#define cadx_fileno fileno
#define cadx_fdopen fdopen
#endif

using nlohmann::json;

namespace {

json error(const json& id, int code, const std::string& message) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

// Replies go to a private copy of stdout. File descriptor 1 itself is pointed
// at stderr, so anything a library prints (OCCT's STEP and IGES translators
// write progress to stdout) lands in the log instead of corrupting the protocol.
FILE* takeOverStdout() {
  std::fflush(stdout);
  const int replyFd = cadx_dup(cadx_fileno(stdout));
  cadx_dup2(cadx_fileno(stderr), cadx_fileno(stdout));
#ifdef _WIN32
  _setmode(replyFd, _O_BINARY);  // no CRLF translation: one line in, one line out
  return cadx_fdopen(replyFd, "wb");
#else
  return cadx_fdopen(replyFd, "w");
#endif
}

}  // namespace

int main() {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
#endif
  FILE* replies = takeOverStdout();
  if (!replies) {
    std::cerr << "cadx-engine: could not set up the reply stream" << std::endl;
    return 1;
  }
  // OCCT messages to stderr as well (belt and braces).
  const Handle(Message_Messenger)& messenger = Message::DefaultMessenger();
  messenger->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
  messenger->AddPrinter(new Message_PrinterOStream("cerr", false, Message_Warning));

  std::ios::sync_with_stdio(false);
  std::cerr << "cadx-engine ready" << std::endl;

  cadx::Session session;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    json id = nullptr;
    json reply;
    try {
      const json req = json::parse(line);
      id = req.value("id", json(nullptr));
      const std::string method = req.at("method").get<std::string>();
      const json params = req.value("params", json::object());
      reply = json{{"jsonrpc", "2.0"}, {"id", id}, {"result", session.handle(method, params)}};
    } catch (const json::parse_error& e) {
      reply = error(nullptr, -32700, e.what());
    } catch (const cadx::MethodNotFound& e) {
      reply = error(id, -32601, e.what());
    } catch (const Standard_Failure& e) {
      reply = error(id, -32000, std::string("geometry error: ") + e.GetMessageString());
    } catch (const std::exception& e) {
      reply = error(id, -32603, e.what());
    } catch (...) {
      reply = error(id, -32603, "unknown engine error");
    }
    // Invalid UTF-8 (from a file name, say) must not kill the reply.
    std::string out = reply.dump(-1, ' ', false, json::error_handler_t::replace);
    out += '\n';
    std::fwrite(out.data(), 1, out.size(), replies);
    std::fflush(replies);
  }
  return 0;
}
