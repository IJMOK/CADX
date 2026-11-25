# CADX conventions

- The plan lives in `docs/PLAN.md`. Keep its checklist up to date when work lands.
- The engine (`engine/`) is the single source of truth for geometry and the document. The UI never computes geometry.
- The engine protocol is line-delimited JSON-RPC 2.0 over stdin/stdout: one request per line, one response per line. Logs go to stderr only, never stdout.
- New geometry operations must record history so persistent names survive (see the "Persistent naming" section of the plan).
- C++: C++20, OCCT handles via `Handle(T)`, no exceptions crossing the RPC boundary; catch `Standard_Failure` and return a JSON-RPC error.
- TypeScript: strict mode; three.js for rendering; no UI framework (decided in Phase 1): plain DOM via `ui/src/dom.ts`.
- Engine stdout carries only JSON-RPC replies: `main.cpp` points fd 1 at stderr, so library prints can't corrupt it. Keep it that way.
- Every face a feature makes must get a name (`history.hpp` explains the fallbacks); tests check there is never a `?`.
- Before claiming UI work done, run `scripts/ui-smoke.mjs` (see README, Development).
- Units inside the engine are millimetres.
- Only `engine/src/sketch/sketch_solver_planegcs.cpp` may include PlaneGCS headers; everything else uses `sketch/sketch.hpp`.
- `engine/third_party/` is vendored upstream code: don't edit it, shim around it.
