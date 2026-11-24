# CADX conventions

- The plan lives in `docs/PLAN.md`. Keep its checklist up to date when work lands.
- The engine (`engine/`) is the single source of truth for geometry and the document. The UI never computes geometry.
- The engine protocol is line-delimited JSON-RPC 2.0 over stdin/stdout: one request per line, one response per line. Logs go to stderr only, never stdout.
- New geometry operations must record history so persistent names survive (see the "Persistent naming" section of the plan).
- C++: C++20, OCCT handles via `Handle(T)`, no exceptions crossing the RPC boundary; catch `Standard_Failure` and return a JSON-RPC error.
- TypeScript: strict mode; three.js for rendering; no UI framework yet (choose before Phase 1 dialogs).
- Units inside the engine are millimetres.
