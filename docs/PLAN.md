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
| Sketch solver | PlaneGCS from FreeCAD 1.0 (LGPL), vendored, behind `cadx::sketch::solve` |
| Engine to UI link | Line-delimited JSON-RPC 2.0 over stdio, binary mesh buffers later |
| Tests | GoogleTest (engine), Rust unit tests (shell); Vitest + Playwright (UI) still to come |
| UI toolkit | None: strict TypeScript with a small DOM helper (`ui/src/dom.ts`) |

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
| Later | Engine compiled to WebAssembly: the app on GitHub Pages and the iPad | Full editing in the browser |

## Phase 0 checklist

- [x] Monorepo layout
- [x] Engine: `makeBox`, tessellate, return a mesh with per-triangle face IDs over JSON-RPC
- [x] UI: three.js viewport with orbit, edges and face picking
- [x] Shell: launch the engine and relay requests
- [x] CI: build the engine (Ubuntu, system OCCT) and type-check and build the UI
- [x] JSON-RPC API v0 (`engine/src/rpc.hpp`): engine-owned document with undo/redo, `newDocument`, `openDocument`,
      `addFeature`, `editFeature`, `deleteFeature`, `moveFeature`, `setParameters`, `setRollback`, `preview`,
      `solveSketch`, `measure`, `exportFile`, `importFile`. Every change returns the full state (mesh with face
      and edge ids, persistent names, feature and sketch status). Picking happens in the UI against those ids.
- [x] Persistent naming spike: extrude, fillet an edge, edit the sketch, check that the fillet survives
      (history-based names, `engine/src/naming.*`; 12 regression tests in `engine/tests/naming_test.cpp`)
- [x] Windows build: `scripts/build-windows.ps1` (prebuilt OCCT 7.9.3 + vcpkg) builds, tests and packages an NSIS installer
- [ ] Windows CI for the engine (vcpkg, cached)

## Phase 1 checklist

- [x] Sketch model and solver interface (`engine/src/sketch/`), PlaneGCS vendored behind it
- [x] Constraints: coincident, horizontal, vertical, parallel, perpendicular, tangent (edge and
      endpoint-to-endpoint), equal, pointOn, midpoint, symmetric, distance, distanceX/Y, angle,
      radius, diameter, fix; built-in `origin`, `xaxis`, `yaxis`
- [x] DOF count, conflicting and redundant constraint reporting
- [x] Profiles from solved sketches: lines, arcs, circles in any order; outer loop plus holes;
      construction geometry ignored
- [x] `solveSketch` RPC; `rebuild` solves sketches and reports their state
- [x] 30 engine tests (sketch solver, naming, solved-sketch features), all solids checked valid
- [x] Sketch mode in the UI: lines, rectangles, circles, arcs, points, construction; point/curve/grid snaps;
      auto coincident, point-on, horizontal and vertical; all constraint types; smart dimensions with
      expressions; live drag; local undo; DOF and conflict display
- [x] Sketches in the feature tree, on XY/XZ/YZ, offset datum planes or planar faces (`plane` spec)
- [x] Revolve and cut features; any number of solid features (unite, subtract, intersect)
- [x] Sketches with several separate regions and islands
- [x] Parameters and expressions table (`engine/src/expr.*`), usable in any feature value or dimension
- [x] Save and open `.cadx` files (zip: `manifest.json` + `part.json`, versioned); bare `.json` also opens
- [x] Part navigator: edit in place with live preview, suppress, rename, reorder, delete, roll back and insert
- [x] Naming regression suite passes (gate)

## Phase 2 checklist

- [x] Edge blend and chamfer (history naming via `engine/src/history.*`)
- [x] Shell (remove faces, inward thickness)
- [x] Holes: simple, counterbore, countersink, drill tip, blind or through, at every point of a sketch
- [x] Linear (one or two directions) and circular patterns of features or the whole body
- [x] Mirror features or the whole body about a plane or planar face
- [x] Datum planes (offset from a base plane, datum or face, flippable)
- [x] Primitives: block, cylinder, sphere, cone
- [x] STEP AP214 / IGES / STL / BREP export; STEP / IGES / BREP import (stored in the part as BREP text)
- [x] Measure: volume, area, mass by material, centre of mass, bounding box
- [ ] Datum axes and points; plane at an angle; plane through three points
- [ ] Variable-radius blends, face blends, draft, sweep, loft, thread features
- [ ] Project body edges into a sketch; sketch fillet/trim/extend/offset
- [ ] Model a real PrintNC part (gate)

## Persistent naming (as built)

- **Faces** are named by the feature that made them: `Extrude1/start`, `Extrude1/end`,
  `Extrude1/side(Sketch1/L1)`, `Fillet1/blend(<edge name>)`, `Fillet1/corner(<vertex name>)`.
  A face that an operation keeps or modifies keeps its name.
- **Edges** are named by their two adjacent faces, sorted: `Extrude1/end|Extrude1/side(Sketch1/L1)`.
  A single-face edge (seam) gets a `|seam` suffix. **Vertices** are named by their adjacent faces.
- Fillets follow tangent chains like NX; blend faces on edges the user did not pick are named from
  the edge that generated them.
- A reference that no longer resolves fails that feature with a clear message
  (`edge not found: …`) and suppresses later features. It is never rebound to a guessed edge.
- Booleans, blends, chamfers, shells, patterns and mirrors name their results from OCCT history in one
  place (`engine/src/history.*`): unchanged and modified faces keep their names, faces made from an edge
  are `<feature>/blend(<edge>)`, from a vertex `<feature>/corner(<vertex>)`, a face moved off its surface
  (a shell's inner wall) `<feature>/offset(<face>)`. A geometric fallback and finally `<feature>/face<n>`
  make sure every face has a name.
- Pattern and mirror copies are prefixed: `Pattern1[2]/Hole1/wall(P1)`, `Mirror1/Extrude1/end`.
- Hole faces: `Hole1/wall(P1)`, `/bottom`, `/tip`, `/cbWall`, `/cbFloor`, `/countersink`.
  Primitive faces: `Box1/top|bottom|front|back|left|right`, `Cylinder1/side|top|bottom`, `Sphere1/surface`.
- Known limits: two faces sharing more than one edge give an ambiguous edge name, and a face split in two
  by a cut gives both halves the same name; both are reported as errors when referenced, never guessed.
