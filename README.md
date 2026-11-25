# CADX

An NX-style parametric CAD system: constrained sketches, solid features with full history and persistent naming, parameters, and STEP/IGES/STL exchange. Assemblies, drawings and CAM come later (see [docs/PLAN.md](docs/PLAN.md)).

**Status:** Phases 0 and 1 done, Phase 2 mostly done. Windows installer builds.

## What it does

- **Sketcher**: lines, rectangles, circles, arcs, points, construction geometry. Snaps to points, curves and the grid, and adds coincident, point-on and horizontal/vertical constraints as you draw. Constraints: coincident, horizontal, vertical, parallel, perpendicular, tangent, equal, midpoint, point on curve, symmetric, fix. Dimensions: length, distance, horizontal/vertical distance, radius, diameter, angle. Values can be expressions. Drag geometry and it re-solves live. Shows remaining degrees of freedom and names conflicting constraints.
- **Sketch planes**: XY, XZ, YZ, offset datum planes, or any planar face of the part.
- **Features**: extrude (one way, reversed, symmetric, two-sided, through all), revolve (full or partial, about a sketch line, sketch axis or edge), hole (simple, counterbore, countersink, drilled tip, through or blind, one per sketch point), edge blend, chamfer, shell, linear pattern (one or two directions), circular pattern, mirror (features or the whole body), datum plane, block, cylinder, sphere, cone. Each solid feature can unite, subtract or intersect.
- **History**: a part navigator with every sketch and feature. Edit any of them in place with a live preview, suppress, rename, reorder, delete, or roll back to any point and insert features there. Undo and redo everything.
- **Persistent naming**: features refer to faces and edges by how they were made (`Extrude1/end|Extrude1/side(Sketch1/L1)`), never by index. Change a dimension upstream and the blend stays on its edge. If a reference really disappears, that feature turns red with a reason and later features are suppressed. CADX never guesses a substitute.
- **Parameters**: a table of named expressions (`width / 2 + 3`, `sqrt(a^2 + b^2)`, `sin(30)`) usable in any feature value or sketch dimension.
- **Files**: `.cadx` (a zip with a versioned JSON part). Export STEP AP214, IGES, STL and BREP. Import STEP, IGES and BREP, as a new part or into the current one.
- **Tools**: measure (volume, area, mass by material, centre of mass, bounding box), command finder (Ctrl+K), keyboard shortcuts (F1), standard views, perspective or orthographic, shaded or wireframe, two example parts.

## Using it

| Mouse | |
| --- | --- |
| Middle drag, or Alt + left drag | Rotate |
| Right drag, or Shift + middle drag | Pan |
| Wheel | Zoom at the cursor |
| Left click | Select (Shift or Ctrl to add) |
| Right click | Context menu |

Typical workflow: **S** to sketch (pick a plane or face), draw with **L**/**R**/**C**/**A**, dimension with **D**, finish with **Ctrl+Enter**, then **X** to extrude. Select edges and press **B** to blend them. Double-click any navigator item to edit it. **Ctrl+E** opens the parameters table. **File ▸ Open example** loads a bracket or a flange to explore.

## Architecture

```
┌──────────────────────── Desktop app (Tauri 2) ────────────────────────┐
│  ui/  (TypeScript, three.js)  ⇄ IPC ⇄  app/  (Rust shell)  ⇄ stdio ⇄  engine/  (C++20, OpenCASCADE) │
└────────────────────────────────────────────────────────────────────────┘
```

- **engine/**: a headless C++ process. It owns the document, undo history, feature tree, rebuild, persistent naming, sketch solver and the OCCT kernel. It speaks line-delimited JSON-RPC 2.0 on stdin/stdout. The API is documented in [engine/src/rpc.hpp](engine/src/rpc.hpp) and the document format in [engine/src/part.hpp](engine/src/part.hpp).
- **app/**: the Tauri shell. It owns the window and file dialogs, runs the engine as a sidecar, relays requests, and reads and writes `.cadx` zips.
- **ui/**: the viewport, part navigator, ribbon, task panel and sketcher. No UI framework: plain TypeScript and three.js.

The engine never depends on the UI, so it can be tested on its own and later compiled to WebAssembly.

## Building

### Windows (installer)

Needs Visual Studio 2022+ or Build Tools with the C++ workload, Python 3, Node 20+ with pnpm, Rust stable and the Tauri CLI (`cargo install tauri-cli --version "^2" --locked`).

1. Download OpenCASCADE 7.9.3 for Windows, `opencascade-7.9.3-vc14-64-combined.zip`, from the [OCCT releases](https://github.com/Open-Cascade-SAS/OCCT/releases/tag/V7_9_3), and unzip it.
2. Get the smaller dependencies with vcpkg:
   ```powershell
   git clone https://github.com/microsoft/vcpkg C:\deps\vcpkg
   C:\deps\vcpkg\bootstrap-vcpkg.bat
   C:\deps\vcpkg\vcpkg install nlohmann-json eigen3 boost-graph boost-math gtest --triplet x64-windows
   ```
3. Build everything and make the installer:
   ```powershell
   .\scripts\build-windows.ps1 -OcctDir C:\deps\occt\opencascade-7.9.3-vc14-64 -VcpkgRoot C:\deps\vcpkg
   ```
   The installer lands in `app\src-tauri\target\release\bundle\nsis\`. It installs per user (no admin rights), bundles the engine with its DLLs and the MSVC runtime, and registers `.cadx` files.

### Linux (engine)

```bash
sudo apt install cmake g++ nlohmann-json3-dev libtbb-dev libgtest-dev libeigen3-dev libboost-dev \
  libocct-foundation-dev libocct-modeling-data-dev libocct-modeling-algorithms-dev \
  libocct-data-exchange-dev

cmake -B build -S engine -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure      # naming, sketch, feature and document tests
echo '{"jsonrpc":"2.0","id":1,"method":"ping"}' | ./build/cadx-engine
```

On Linux the shell also needs `libwebkit2gtk-4.1-dev librsvg2-dev`.

### Development

```bash
pnpm install --dir ui
cd app
CADX_ENGINE=../build/cadx-engine cargo tauri dev     # on Windows also put the OCCT DLL folders on PATH
```

`CADX_ENGINE` points the shell at an engine binary. Without it, the shell looks for `cadx-engine` next to its own executable, which is where the installer puts it.

The UI also runs in a plain browser against a real engine, which is handy for quick iteration and for automated tests:

```bash
node scripts/dev-bridge.mjs path/to/cadx-engine 5174   # relays HTTP on 127.0.0.1 to the engine
pnpm --dir ui dev --port 5175                          # open http://localhost:5175/?bridge=5174
node scripts/ui-smoke.mjs                              # drives the UI in headless Edge/Chrome, with screenshots
```

The smoke test sketches and dimensions a rectangle with real mouse clicks, extrudes it, picks an edge in the viewport and blends it, sketches on a face and cuts through, edits from the navigator, and opens the example parts, failing on any error. File dialogs and `.cadx` save need the desktop shell, so they are covered by the Rust tests instead.

## Repo layout

```
engine/   C++ geometry engine (CMake)
app/      Tauri shell (Rust), src-tauri layout
ui/       Web UI (Vite + TypeScript + three.js)
scripts/  Windows build and bundle staging
docs/     Plan and design notes
```

## Licences

OpenCASCADE (LGPL 2.1 with the OCCT exception) is linked dynamically; the installer ships its DLLs unmodified. PlaneGCS (LGPL 2.1) is vendored as source in `engine/third_party/planegcs`, with its licence, and compiled into the engine. CADX has no licence file of its own yet; choose one before distributing builds.
