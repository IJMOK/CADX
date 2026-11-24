# CADX

An NX-style parametric CAD system: sketches, solid features with full history, and assemblies. CAM for CNC routing and turning comes later.

**Status:** Phase 0 (foundation). See [docs/PLAN.md](docs/PLAN.md).

## Architecture

```
┌──────────────────────── Desktop app (Tauri 2) ────────────────────────┐
│  ui/  (TypeScript, three.js)  ⇄ IPC ⇄  app/  (Rust shell)  ⇄ stdio ⇄  engine/  (C++20, OpenCASCADE) │
└────────────────────────────────────────────────────────────────────────┘
```

- **engine/**: a headless C++ process. It owns the document, feature tree, rebuild, persistent naming, solvers and the OCCT kernel. It speaks line-delimited JSON-RPC 2.0 on stdin/stdout.
- **app/**: the Tauri shell. It handles the window and file dialogs, launches the engine as a sidecar and relays requests.
- **ui/**: everything the user sees: the viewport, part navigator, dialogs and command finder.

The engine never depends on the UI, so it can be tested on its own and later compiled to WebAssembly.

## Building (Phase 0)

### Engine

Needs CMake 3.24+, a C++20 compiler, OpenCASCADE 7.6+ and nlohmann-json.

```bash
# Ubuntu / Debian
sudo apt install cmake g++ nlohmann-json3-dev libtbb-dev libgtest-dev \
  libocct-foundation-dev libocct-modeling-data-dev libocct-modeling-algorithms-dev \
  libocct-data-exchange-dev

# or any platform, via vcpkg (slow the first time: it builds OCCT)
#   cmake -B build -S engine -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

cmake -B build -S engine -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure      # naming regression tests
echo '{"jsonrpc":"2.0","id":1,"method":"makeBox","params":{"x":40,"y":20,"z":10}}' | ./build/cadx-engine
```

### UI + app

Needs Node 20+, pnpm and Rust (stable).

On Linux the shell also needs `libwebkit2gtk-4.1-dev librsvg2-dev`. Windows and macOS need nothing extra.

```bash
pnpm install --dir ui
cargo install tauri-cli --version "^2" --locked   # once
cd app
CADX_ENGINE=../build/cadx-engine cargo tauri dev
```

The window opens with a filleted demo part. Change width, depth, height or fillet radius in the toolbar: the part rebuilds and the fillet stays on the same edge. Hover over a face to see its persistent name.

`CADX_ENGINE` points the shell at the engine binary. In release builds it will be bundled as a sidecar.

## Repo layout

```
engine/   C++ geometry engine (CMake)
app/      Tauri shell (Rust), src-tauri layout
ui/       Web UI (Vite + TypeScript + three.js)
docs/     Plan and design notes
```
