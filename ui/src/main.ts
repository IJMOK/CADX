import { call, engineAvailable, type MeshData } from "./engine";
import { Viewport } from "./viewport";

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const status = $("status");
const tree = $<HTMLUListElement>("tree");
const viewport = new Viewport($<HTMLCanvasElement>("viewport"));

const setStatus = (text: string) => (status.textContent = text);

viewport.onHover = (face) => setStatus(face >= 0 ? `Face ${face}` : "");

async function build(label: string, method: string, params: Record<string, unknown>) {
  try {
    const mesh = await call<MeshData>(method, params);
    viewport.show(mesh);
    tree.innerHTML = "";
    const li = document.createElement("li");
    li.textContent = label;
    tree.append(li);
    setStatus(`${label}: ${mesh.indices.length / 3} triangles`);
  } catch (err) {
    setStatus(String(err instanceof Error ? err.message : err));
  }
}

$("btn-box").addEventListener("click", () => build("Box (40 × 20 × 10)", "makeBox", { x: 40, y: 20, z: 10 }));
$("btn-cyl").addEventListener("click", () => build("Cylinder (r10, h30)", "makeCylinder", { r: 10, h: 30 }));
$("btn-fit").addEventListener("click", () => viewport.fit());

async function start() {
  if (!engineAvailable()) {
    setStatus("Browser preview: no engine. Run the desktop app to build geometry.");
    return;
  }
  try {
    const info = await call<{ engine: string; version: string }>("ping");
    setStatus(`Engine ${info.version} connected`);
    await build("Box (40 × 20 × 10)", "makeBox", { x: 40, y: 20, z: 10 });
  } catch (err) {
    setStatus(String(err instanceof Error ? err.message : err));
  }
}

start();
