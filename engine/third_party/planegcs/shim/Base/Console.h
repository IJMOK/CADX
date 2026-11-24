// CADX shim: minimal stand-in for FreeCAD's Base::Console.
// Log output is dropped; warnings and errors go to stderr (never stdout,
// which carries the engine's JSON-RPC protocol).
#pragma once
#include <cstdarg>
#include <cstdio>

namespace Base {

class ConsoleSingleton {
 public:
  void Log(const char* /*fmt*/, ...) {}
  void Message(const char* /*fmt*/, ...) {}
  void Warning(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
  }
  void Error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
  }
};

inline ConsoleSingleton& Console() {
  static ConsoleSingleton instance;
  return instance;
}

}  // namespace Base
