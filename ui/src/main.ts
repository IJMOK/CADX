import { call, engineAvailable, type MeshData } from "./engine";
import { Viewport } from "./viewport";

interface FeatureStatus {
  id: string;
  state: "ok" | "failed" | "suppressed";
  message: string;
}

interface RebuildResult {
  features: FeatureStatus[];
  mesh?: MeshData;
  faceNames?: string[];
}

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const status = $("status");
const tree = $<HTMLUListElement>("tree");
const viewport = new Viewport($<HTMLCanvasElement>("viewport"));
const num = (id: string) => Number($<HTMLInputElement>(id).value);

let faceNames: string[] = [];
const setStatus = (text: string) => (status.textContent = text);

viewport.onHover = (face) => setStatus(face >= 0 ? (faceNames[face] ?? `Face ${face}`) : "");

// Demo part: a rectangle sketch, extruded, with the top-front edge filleted.
// The fillet references its edge by persistent name, so it follows any edit.
function demoDocument() {
  const w = num("p-width"), d = num("p-depth");
  const seg = (id: string, a: [number, number], b: [number, number]) => ({ id, a, b });
  return {
    sketches: [
      {
        id: "Sketch1",
        segments: [seg("L1", [0, 0], [w, 0]), seg("L2", [w, 0], [w, d]), seg("L3", [w, d], [0, d]), seg("L4", [0, d], [0, 0])],
      },
    ],
    features: [
      { id: "Extrude1", type: "extrude", sketch: "Sketch1", distance: num("p-height") },
      { id: "Fillet1", type: "fillet", radius: num("p-radius"), edges: ["Extrude1/end|Extrude1/side(Sketch1/L1)"] },
    ],
  };
}

function showTree(features: FeatureStatus[]) {
  tree.innerHTML = "";
  const sketch = document.createElement("li");
  sketch.textContent = "✎ Sketch1";
  tree.append(sketch);
  for (const f of features) {
    const li = document.createElement("li");
    const mark = f.state === "ok" ? "✓" : f.state === "failed" ? "✗" : "–";
    li.textContent = `${mark} ${f.id}`;
    li.className = f.state;
    if (f.message) li.title = f.message;
    tree.append(li);
  }
}

let firstBuild = true;

async function rebuild() {
  try {
    const r = await call<RebuildResult>("rebuild", { document: demoDocument() });
    showTree(r.features);
    if (r.mesh) {
      faceNames = r.faceNames ?? [];
      viewport.show(r.mesh, firstBuild);
      firstBuild = false;
    }
    const failed = r.features.find((f) => f.state === "failed");
    setStatus(failed ? `${failed.id}: ${failed.message}` : "Rebuilt");
  } catch (err) {
    setStatus(String(err instanceof Error ? err.message : err));
  }
}

for (const id of ["p-width", "p-depth", "p-height", "p-radius"]) {
  $(id).addEventListener("input", () => void rebuild());
}
$("btn-fit").addEventListener("click", () => viewport.fit());

async function start() {
  if (!engineAvailable()) {
    setStatus("Browser preview: no engine. Run the desktop app to build geometry.");
    return;
  }
  try {
    const info = await call<{ engine: string; version: string }>("ping");
    setStatus(`Engine ${info.version} connected`);
    await rebuild();
  } catch (err) {
    setStatus(String(err instanceof Error ? err.message : err));
  }
}

start();
