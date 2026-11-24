import { call, engineAvailable, type MeshData } from "./engine";
import { Viewport } from "./viewport";

interface FeatureStatus {
  id: string;
  state: "ok" | "failed" | "suppressed";
  message: string;
}

interface SketchStatus {
  state: "solved" | "conflicting" | "failed" | "invalid";
  dof: number;
  conflicting: string[];
  message: string;
  sketch: { id: string };
}

interface RebuildResult {
  sketches: SketchStatus[];
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

// Demo part: a roughly drawn rectangle, fully constrained by dimensions, with
// an optional hole, extruded, and the top-front edge filleted. The fillet
// references its edge by persistent name, so it follows every edit.
function demoDocument() {
  const line = (id: string, a: [number, number], b: [number, number]) => ({ id, type: "line", a, b });
  const con = (id: string, type: string, refs: string[], value?: number) => ({ id, type, refs, ...(value ? { value } : {}) });
  const w = num("p-width"), d = num("p-depth"), hole = num("p-hole");
  const entities: object[] = [
    line("L1", [0, 0], [40, 0]), line("L2", [40, 0], [40, 20]),
    line("L3", [40, 20], [0, 20]), line("L4", [0, 20], [0, 0]),
  ];
  const constraints: object[] = [
    con("c1", "coincident", ["L1.end", "L2.start"]), con("c2", "coincident", ["L2.end", "L3.start"]),
    con("c3", "coincident", ["L3.end", "L4.start"]), con("c4", "coincident", ["L4.end", "L1.start"]),
    con("h1", "horizontal", ["L1"]), con("h2", "horizontal", ["L3"]),
    con("v1", "vertical", ["L2"]), con("v2", "vertical", ["L4"]),
    con("o1", "coincident", ["L1.start", "origin"]),
    con("width", "distance", ["L1"], w), con("depth", "distance", ["L2"], d),
  ];
  if (hole > 0) {
    entities.push({ id: "C1", type: "circle", center: [w * 0.75, d / 2], radius: hole / 2 });
    constraints.push(con("holeX", "distanceX", ["C1.center"], w * 0.75), con("holeY", "distanceY", ["C1.center"], d / 2),
      con("holeD", "diameter", ["C1"], hole));
  }
  return {
    sketches: [{ id: "Sketch1", entities, constraints }],
    features: [
      { id: "Extrude1", type: "extrude", sketch: "Sketch1", distance: num("p-height") },
      { id: "Fillet1", type: "fillet", radius: num("p-radius"), edges: ["Extrude1/end|Extrude1/side(Sketch1/L1)"] },
    ],
  };
}

function showTree(sketches: SketchStatus[], features: FeatureStatus[]) {
  tree.innerHTML = "";
  for (const s of sketches) {
    const li = document.createElement("li");
    const detail = s.state !== "solved" ? s.state : s.dof === 0 ? "fully constrained" : `${s.dof} DOF free`;
    li.textContent = `✎ ${s.sketch.id} (${detail})`;
    li.className = s.state === "solved" ? "ok" : "failed";
    if (s.conflicting.length) li.title = `Conflicting: ${s.conflicting.join(", ")}`;
    tree.append(li);
  }
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
    showTree(r.sketches, r.features);
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

for (const id of ["p-width", "p-depth", "p-height", "p-radius", "p-hole"]) {
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
