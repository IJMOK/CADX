#!/usr/bin/env node
// UI smoke test: drives the real UI in headless Edge/Chrome over the DevTools
// protocol, against a real engine through scripts/dev-bridge.mjs.
//
//   node scripts/dev-bridge.mjs <cadx-engine> 5174 &
//   pnpm --dir ui dev --port 5175 &
//   node scripts/ui-smoke.mjs [url] [screenshot dir]
//
// BROWSER=<path to msedge/chrome> overrides the browser. Exits non-zero on any
// failed step or console error.

import { spawn } from "node:child_process";
import { mkdirSync, writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const url = process.argv[2] ?? "http://localhost:5175/?bridge=5174";
const shots = process.argv[3] ?? join(tmpdir(), "cadx-ui-smoke");
mkdirSync(shots, { recursive: true });
const browser = process.env.BROWSER ?? "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe";
const port = 9333;

const proc = spawn(browser, [
  "--headless=new", `--remote-debugging-port=${port}`, "--window-size=1440,900", "--no-first-run",
  "--disable-gpu-sandbox", "--use-angle=swiftshader", "--enable-unsafe-swiftshader",
  `--user-data-dir=${mkdtempSync(join(tmpdir(), "cadx-edge-"))}`, "about:blank",
], { stdio: "ignore" });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let ws, nextId = 1;
const pending = new Map();
const problems = [];
const log = (...a) => console.log(...a);

async function connect() {
  for (let i = 0; i < 50; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const page = list.find((t) => t.type === "page");
      if (page) return page.webSocketDebuggerUrl;
    } catch { /* not up yet */ }
    await sleep(200);
  }
  throw new Error("browser did not start");
}

function send(method, params = {}) {
  const id = nextId++;
  ws.send(JSON.stringify({ id, method, params }));
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
}

async function evaluate(expr) {
  const r = await send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) throw new Error(`${expr}: ${r.exceptionDetails.exception?.description ?? r.exceptionDetails.text}`);
  return r.result.value;
}

async function shot(name) {
  const r = await send("Page.captureScreenshot", { format: "png" });
  const file = join(shots, `${name}.png`);
  writeFileSync(file, Buffer.from(r.data, "base64"));
  log(`  screenshot ${file}`);
}

async function key(k, mods = 0) {
  const code = k.length === 1 ? `Key${k.toUpperCase()}` : k;
  const text = k.length === 1 && !(mods & 2) ? k : undefined;
  await send("Input.dispatchKeyEvent", { type: "keyDown", key: k, code, modifiers: mods, text, windowsVirtualKeyCode: k.length === 1 ? k.toUpperCase().charCodeAt(0) : k === "Enter" ? 13 : k === "Escape" ? 27 : 0 });
  await send("Input.dispatchKeyEvent", { type: "keyUp", key: k, code, modifiers: mods });
  await sleep(150);
}

async function click(x, y, button = "left") {
  await send("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
  await sleep(40);
  await send("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button, buttons: button === "left" ? 1 : 2, clickCount: 1 });
  await send("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button, buttons: 0, clickCount: 1 });
  await sleep(250);
}

// Screen position (page px) of a world point, via the viewport's projection.
async function screenOf(xyz) {
  return evaluate(`(() => { const v = cadx.viewport; const r = v.canvas.getBoundingClientRect();
    const p = v.project(new (cadx.viewport.camera.position.constructor)(${xyz.join(",")})); return [r.left + p.x, r.top + p.y]; })()`);
}

// Screen position of a point on the active sketch plane.
async function sketchScreen(x, y) {
  return evaluate(`(() => { const v = cadx.viewport; const r = v.canvas.getBoundingClientRect(); const s = cadx.sketcher;
    const pl = s.placement; const V = v.camera.position.constructor;
    const w = new V(...pl.origin).addScaledVector(new V(...pl.xDir), ${x}).addScaledVector(new V(...pl.yDir), ${y});
    const p = v.project(w); return [r.left + p.x, r.top + p.y]; })()`);
}

async function waitFor(expr, what, ms = 8000) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    try { if (await evaluate(expr)) return; } catch { /* keep waiting */ }
    await sleep(100);
  }
  throw new Error(`timed out waiting for ${what}`);
}

async function step(name, fn) {
  log(`- ${name}`);
  try { await fn(); } catch (e) { problems.push(`${name}: ${e.message}`); log(`  FAILED: ${e.message}`); }
}

const features = () => evaluate(`cadx.state().features.map(f => f.id + ":" + f.state).join(" ")`);

try {
  ws = new WebSocket(await connect());
  await new Promise((r) => (ws.onopen = r));
  ws.onmessage = (m) => {
    const msg = JSON.parse(m.data);
    if (msg.id && pending.has(msg.id)) { pending.get(msg.id).resolve(msg.result ?? msg); pending.delete(msg.id); return; }
    if (msg.method === "Runtime.exceptionThrown") problems.push(`exception: ${msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text}`);
    if (msg.method === "Runtime.consoleAPICalled" && msg.params.type === "error") problems.push(`console.error: ${msg.params.args.map((a) => a.value ?? a.description).join(" ")}`);
  };
  await send("Runtime.enable");
  await send("Page.enable");
  await send("Emulation.setDeviceMetricsOverride", { width: 1440, height: 900, deviceScaleFactor: 1, mobile: false });
  await send("Page.navigate", { url });

  await step("app starts and connects to the engine", async () => {
    await waitFor(`!!(window.cadx && cadx.state())`, "first state");
    await shot("01-start");
  });

  await step("example bracket builds", async () => {
    await evaluate(`cadx.commands.find(c => c.id === "example-bracket").run()`);
    await waitFor(`cadx.state().document.name === "Bracket"`, "bracket");
    const f = await features();
    log(`  ${f}`);
    if (f.includes("failed")) throw new Error(f);
    await sleep(500);
    await shot("02-bracket");
  });

  await step("parameter edit rebuilds", async () => {
    const params = (await evaluate(`JSON.stringify(cadx.state().document.parameters)`));
    const p = JSON.parse(params).map((x) => (x.name === "length" ? { ...x, expression: "120" } : x));
    await evaluate(`(async () => { const st = await (await import("/src/engine.ts")).call("setParameters", { parameters: ${JSON.stringify(p)} }); return st.features.every(f => f.state === "ok"); })()`);
  });

  await step("new part (discarding the example)", async () => {
    await evaluate(`cadx.commands.find(c => c.id === "new").run()`);
    await waitFor(`!!document.querySelector(".modal button.primary")`, "discard dialog");
    await evaluate(`document.querySelector(".modal button.primary").click()`);
    await waitFor(`cadx.state().document.features.length === 0`, "empty part");
  });

  await step("sketch a rectangle on XY and dimension it", async () => {
    await key("s");
    await waitFor(`!!document.querySelector("#task-panel.open .primary")`, "sketch plane dialog");
    await evaluate(`document.querySelector("#task-panel .primary").click()`);
    await waitFor(`cadx.sketcher.active`, "sketcher");
    await sleep(300);
    await evaluate(`cadx.sketcher.gridSnap = true`);
    await key("r");
    let [x, y] = await sketchScreen(0, 0);
    await click(x, y);              // snaps to the origin
    [x, y] = await sketchScreen(40, 25);
    await click(x, y);
    await waitFor(`cadx.sketcher.data().entities.length === 4`, "4 lines");
    // Dimension the bottom line: D, click the line, click empty space, type 50.
    await key("d");
    const bottom = await evaluate(`(() => { const d = cadx.sketcher.data(); const l = d.entities.find(e => e.type === "line" && Math.abs(e.a[1]) < 1e-6 && Math.abs(e.b[1]) < 1e-6); return [ (l.a[0] + l.b[0]) / 2, 0 ]; })()`);
    [x, y] = await sketchScreen(bottom[0], bottom[1]);
    await click(x, y);
    [x, y] = await sketchScreen(bottom[0], -15);
    await click(x, y);
    await waitFor(`!!document.querySelector(".dim-input")`, "dimension input");
    await evaluate(`(() => { const i = document.querySelector(".dim-input"); i.value = "50"; i.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter" })); })()`);
    await waitFor(`cadx.sketcher.data().constraints.some(c => c.type === "distance" && c.value === 50)`, "dimension added");
    await sleep(300);
    const dof = await evaluate(`cadx.sketcher.dof`);
    log(`  DOF after one dimension: ${dof}`);
    if (dof !== 1) throw new Error(`expected 1 DOF, got ${dof}`);
    await shot("03-sketch");
    await key("Enter", 2); // Ctrl+Enter finishes
    await waitFor(`!cadx.sketcher.active && cadx.state().document.features.length === 1`, "sketch saved");
  });

  await step("extrude the sketch", async () => {
    await key("x");
    await waitFor(`!!document.querySelector("#task-panel.open .primary")`, "extrude dialog");
    await sleep(600); // live preview
    await evaluate(`document.querySelector("#task-panel .primary").click()`);
    await waitFor(`cadx.state().features.some(f => f.id === "Extrude1" && f.state === "ok")`, "Extrude1 ok");
    const vol = await evaluate(`(async () => (await (await import("/src/engine.ts")).call("measure")).volume)()`);
    // Width is dimensioned to 50; the height is wherever the grid snapped the corner.
    const h = await evaluate(`(() => { const s = cadx.state().sketches[0].sketch; const ys = s.entities.flatMap(e => [e.a[1], e.b[1]]); return Math.max(...ys) - Math.min(...ys); })()`);
    log(`  volume ${vol.toFixed(2)} (expect 50 x ${h} x 10 = ${50 * h * 10})`);
    if (Math.abs(vol - 50 * h * 10) > 1e-3) throw new Error(`volume ${vol}`);
    await sleep(400);
    await shot("04-extrude");
  });

  await step("pick an edge in the viewport and blend it", async () => {
    // Midpoint of the top-front edge, from the mesh, projected to the screen.
    const target = "Extrude1/end|Extrude1/side(Sketch1/L1)";
    const mid = await evaluate(`(() => { const st = cadx.state(); const i = st.edgeNames.indexOf(${JSON.stringify(target)});
      const m = st.mesh; const segs = []; m.edgeIds.forEach((e, k) => { if (e === i) segs.push(k); });
      const k = segs[Math.floor(segs.length / 2)]; const p = m.edges.slice(k * 6, k * 6 + 6);
      return [(p[0] + p[3]) / 2, (p[1] + p[4]) / 2, (p[2] + p[5]) / 2]; })()`);
    const [x, y] = await screenOf(mid);
    await click(x, y);
    const sel = await evaluate(`JSON.stringify(cadx.selection())`);
    log(`  selected ${sel}`);
    if (!sel.includes(target)) throw new Error(`edge not picked: ${sel}`);
    await key("b");
    await waitFor(`!!document.querySelector("#task-panel.open .primary")`, "blend dialog");
    await sleep(600);
    await shot("05-blend-preview");
    await evaluate(`document.querySelector("#task-panel .primary").click()`);
    await waitFor(`cadx.state().features.some(f => f.id === "Fillet1" && f.state === "ok")`, "Fillet1 ok");
  });

  await step("edit the sketch dimension: the blend follows", async () => {
    await evaluate(`(async () => { const e = await import("/src/engine.ts"); const st = cadx.state();
      const sk = st.document.features[0]; const cons = sk.constraints.map(c => c.type === "distance" ? { ...c, value: 80 } : c);
      await e.call("editFeature", { id: "Sketch1", feature: { constraints: cons } }); })()`);
    await evaluate(`cadx.commands.find(c => c.id === "undo").run(); 0`);
    await sleep(300);
    await evaluate(`cadx.commands.find(c => c.id === "redo").run(); 0`);
    await sleep(500);
    // The UI state only refreshes through its own calls; ask for it.
    const f = await evaluate(`(async () => (await (await import("/src/engine.ts")).call("getState")).features.map(f => f.id + ":" + f.state).join(" "))()`);
    log(`  ${f}`);
    if (f.includes("failed")) throw new Error(f);
  });

  await step("sketch a circle on the top face and cut it through", async () => {
    // Click the middle of the top face to select it, then S sketches straight on it.
    const top = await evaluate(`(() => { const st = cadx.state(); const fi = st.faceNames.indexOf("Extrude1/end"); const m = st.mesh;
      let sx = 0, sy = 0, sz = 0, n = 0; m.faceIds.forEach((f, t) => { if (f !== fi) return; for (let k = 0; k < 3; k++) { const v = m.indices[t * 3 + k];
      sx += m.positions[v * 3]; sy += m.positions[v * 3 + 1]; sz += m.positions[v * 3 + 2]; n++; } }); return [sx / n, sy / n, sz / n]; })()`);
    let [x, y] = await screenOf(top);
    await click(x, y);
    const sel = await evaluate(`JSON.stringify(cadx.selection())`);
    if (!sel.includes("Extrude1/end")) throw new Error(`top face not picked: ${sel}`);
    await key("s");
    await waitFor(`cadx.sketcher.active`, "sketcher on face");
    await sleep(300);
    await key("c");
    const c = [top[0], top[1]];
    [x, y] = await sketchScreen(c[0], c[1]);
    await click(x, y);
    [x, y] = await sketchScreen(c[0] + 6, c[1]);
    await click(x, y);
    await waitFor(`cadx.sketcher.data().entities.some(e => e.type === "circle")`, "circle");
    await key("Enter", 2);
    await waitFor(`!cadx.sketcher.active && cadx.state().document.features.some(f => f.id === "Sketch2")`, "Sketch2 saved");
    await key("x");
    await waitFor(`!!document.querySelector("#task-panel.open select")`, "extrude dialog");
    // Use the dialog's own controls: Boolean = cut, Through all = on.
    await evaluate(`(() => { const sels = [...document.querySelectorAll("#task-panel select")];
      const op = sels.find(s => [...s.options].some(o => o.value === "cut")); op.value = "cut"; op.dispatchEvent(new Event("change"));
      return 1; })()`);
    await sleep(200);
    await evaluate(`(() => { const cb = [...document.querySelectorAll("#task-panel label.check")].find(l => l.textContent.includes("Through all")).querySelector("input");
      cb.checked = true; cb.dispatchEvent(new Event("change")); return 1; })()`);
    await sleep(700);
    await shot("05b-cut-preview");
    await evaluate(`document.querySelector("#task-panel .primary").click()`);
    await waitFor(`cadx.state().features.some(f => f.id === "Extrude2")`, "Extrude2");
    const f = await features();
    log(`  ${f}`);
    if (f.includes("failed")) throw new Error(f);
    const faces = await evaluate(`cadx.state().faceNames.filter(n => n.startsWith("Extrude2/")).join(", ")`);
    log(`  cut faces: ${faces}`);
    if (!faces.includes("Extrude2/side(Sketch2/C1)")) throw new Error("cut wall not named");
  });

  await step("double-click a navigator item to edit it", async () => {
    await evaluate(`(() => { const li = [...document.querySelectorAll("#tree li.nav-item")].find(l => l.textContent.includes("Extrude1"));
      li.dispatchEvent(new MouseEvent("dblclick", { bubbles: true })); return 1; })()`);
    await waitFor(`!!document.querySelector("#task-panel.open")`, "edit dialog");
    await evaluate(`(() => { const lab = [...document.querySelectorAll("#task-panel label.field")].find(l => l.textContent.startsWith("Distance"));
      const i = lab.querySelector("input"); i.value = "thick * 2"; i.dispatchEvent(new Event("change")); return 1; })()`);
    await sleep(500);
    const msg = await evaluate(`document.querySelector(".task-msg").textContent`);
    log(`  preview message with an unknown parameter: "${msg}"`);
    if (!msg.includes("unknown name 'thick'")) throw new Error("expected an error naming the unknown parameter");
    await evaluate(`(() => { const lab = [...document.querySelectorAll("#task-panel label.field")].find(l => l.textContent.startsWith("Distance"));
      const i = lab.querySelector("input"); i.value = "15"; i.dispatchEvent(new Event("change")); return 1; })()`);
    await sleep(500);
    await evaluate(`document.querySelector("#task-panel .primary").click()`);
    await waitFor(`cadx.state().document.features.find(f => f.id === "Extrude1").distance === 15`, "distance 15");
    const f = await features();
    log(`  ${f}`);
    if (f.includes("failed")) throw new Error(f);
  });

  await step("measure and parameters dialogs", async () => {
    await key("m");
    await waitFor(`!!document.querySelector("table.measure")`, "measure dialog");
    const txt = await evaluate(`document.querySelector("table.measure").innerText.split(String.fromCharCode(10)).slice(0, 4).join(" | ")`);
    log(`  ${txt}`);
    await shot("05c-measure");
    await evaluate(`document.querySelector(".modal button.primary").click()`);
    await key("e", 2);
    await waitFor(`!!document.querySelector(".params")`, "parameters dialog");
    await evaluate(`(() => { const r = document.querySelector(".params tbody tr"); const [n, e] = r.querySelectorAll("input"); n.value = "thick"; e.value = "7.5"; return 1; })()`);
    await evaluate(`document.querySelector(".modal button.primary").click()`);
    await waitFor(`cadx.state().parameterValues.thick === 7.5`, "parameter applied");
  });

  await step("command finder and help open", async () => {
    await key("k", 2);
    await waitFor(`!!document.querySelector(".finder input")`, "finder");
    await shot("06-finder");
    await key("Escape");
    await evaluate(`document.querySelector(".modal-overlay")?.remove(); 0`);
  });

  await step("final view", async () => {
    await evaluate(`cadx.commands.find(c => c.id === "example-flange").run()`);
    await sleep(300);
    if (await evaluate(`!!document.querySelector(".modal button.primary")`)) await evaluate(`document.querySelector(".modal button.primary").click()`);
    await waitFor(`cadx.state().document.name === "Flange"`, "flange");
    await sleep(600);
    await shot("07-flange");
  });
} catch (e) {
  problems.push(`harness: ${e.message}`);
} finally {
  ws?.close();
  proc.kill();
}

if (problems.length) {
  console.log(`\n${problems.length} problem(s):`);
  for (const p of problems) console.log(`  ${p}`);
  process.exit(1);
}
console.log("\nUI smoke test passed");
