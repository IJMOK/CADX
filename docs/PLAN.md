# CADX plan (summary)

The living plan, with diagrams, is the "NX-style CAD/CAM — Architecture & Plan" doc. This file is the in-repo summary.

## Principles

- One environment and one part navigator. No workbench switching.
- Persistent naming from day one: features reference geometry by how it was made, never by face index.
- The engine is headless and owns the truth. The UI is a view that sends commands.
- Everything can be undone; every feature can be edited in place with a live preview.
- Failures are local: a broken feature shows red with a reason, and later features are suppressed rather than corrupted.

## Stack

| Layer | Choice |
| --- | --- |
| Shell | Tauri 2 (Rust) |
| UI | TypeScript, Vite, three.js |
| Kernel | OpenCASCADE 7.6+ (LGPL, linked dynamically) |
| Engine | C++20, CMake, vcpkg or system packages |
| Sketch solver | PlaneGCS (LGPL) behind our own interface |
| Engine to UI link | Line-delimited JSON-RPC 2.0 over stdio, binary mesh buffers later |
| Tests | GoogleTest (engine), Vitest + Playwright (UI) |

## Data model

- **Document** holds parts and assemblies (later drawings and CAM setups).
- **Part** holds parameters, datums, sketches and features in tree order.
- **Feature** holds type, inputs (values or expressions), references (persistent names), and suppressed and error state.
- **Assembly** holds component instances (part + placement) and mates.
- **File format:** `.cadx` is a zip containing `manifest.json`, a JSON file per part or assembly, and an optional BREP cache. Versioned from day one.

## Roadmap

| Phase | Scope | Gate |
| --- | --- | --- |
| 0 Foundation | Monorepo and CI, OCCT box to three.js mesh, JSON-RPC v0 | A cube you can orbit and pick, built in CI |
| 1 Sketch + first features | Sketcher with PlaneGCS, extrude, revolve, cut, feature tree, parameters, save and open | Naming regression suite passes |
| 2 Real parts | Fillet, chamfer, shell, holes, patterns, mirror, datums, STEP | Model a real PrintNC part |
| 3 Assemblies | Components, mates, motion, exploded views, interference, BOM | Assemble the 4th-axis unit |
| 4 Polish + drawings | Radial menus, dynamic handles, drawings, synchronous modelling | CADX is the default CAD for new parts |
| 5 Router CAM | Setups, 2.5D ops, adaptive clearing, grblHAL post, simulation | Cut a real part on the PrintNC |
| 6 Turning + 4th axis | Lathe-mode ops, G33/G76 threading, indexed then wrapped 4-axis | |

## Phase 0 checklist

- [x] Monorepo layout
- [x] Engine: `makeBox`, tessellate, return a mesh with per-triangle face IDs over JSON-RPC
- [x] UI: three.js viewport with orbit, edges and face picking
- [x] Shell: launch the engine and relay requests
- [x] CI: build the engine (Ubuntu, system OCCT) and type-check and build the UI
- [ ] Define JSON-RPC API v0 properly (`openDocument`, `addFeature`, `editFeature`, `rebuild`, `getMesh`, `pick`)
- [ ] Persistent naming spike: extrude, fillet an edge, edit the sketch, check that the fillet survives
- [ ] Windows CI for the engine (vcpkg, cached)
