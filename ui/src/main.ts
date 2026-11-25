// CADX desktop UI: ribbon, part navigator, viewport, task panel and sketcher.
// The engine owns the document; every change is an RPC that returns the new state.

import { getCurrentWindow } from "@tauri-apps/api/window";
import { FEATURES, FeatureDialog, ICONS, short, type DialogHost } from "./dialogs";
import { $, clear, confirmBox, fmt, h, modal, promptBox, toast } from "./dom";
import { call, engineAvailable, errorText, inDesktopApp } from "./engine";
import { bracketExample, flangeExample } from "./examples";
import * as files from "./files";
import { Navigator } from "./navigator";
import { Sketcher } from "./sketcher";
import type { Feature, Measure, Pick, Placement, SketchData, SketchStatus, State } from "./types";
import { Viewport, type DisplayMode, type ViewName } from "./viewport";

// ------------------------------------------------------------------ app state

const viewport = new Viewport($<HTMLCanvasElement>("viewport"));
const statusText = $("status-text");
const statusPick = $("status-pick");
const taskPanel = $("task-panel");

let state: State | null = null;
let filePath: string | null = null;
let savedRevision = -1;
let selection: Pick[] = [];
let dialog: FeatureDialog | null = null;
let lastSketch: string | undefined;
let showSketches = true;
let showDatums = true;
let firstGeometry = true;
let sketchSession: { id: string | null; plane: unknown } | null = null;

const setStatus = (t: string) => (statusText.textContent = t);
const dirty = () => state !== null && state.revision !== savedRevision;

const sketcher = new Sketcher(viewport, {
  solve: (sketch) => call<SketchStatus>("solveSketch", { sketch }),
  finish: () => void finishSketch(),
  cancel: () => cancelSketch(),
  status: (t) => setStatus(t),
});
sketcher.onChange = () => renderSketchRibbon();

const nav = new Navigator($("tree"), {
  edit: (id) => editFeature(id),
  select: (id) => viewport.highlightFeature(id ?? ""),
  menu: (id, x, y) => featureMenu(id, x, y),
  rollTo: (id) => void run(call<State>("setRollback", { id })),
});

// ------------------------------------------------------------- state display

function show(st: State, committed = true): void {
  if (committed) state = st;
  const placements = new Map(st.sketches.map((s) => [s.sketch.id, s.placement]));
  const used = new Set(st.document.features.map((f) => f.sketch as string).filter(Boolean));
  const sk: { sketch: SketchData; placement: Placement; dim: boolean }[] = [];
  for (const s of st.sketches) {
    const p = placements.get(s.sketch.id);
    if (!p || s.sketch.id === sketcher.id) continue;
    const f = st.document.features.find((x) => x.id === s.sketch.id);
    if (f?.suppressed || f?.hidden) continue;
    sk.push({ sketch: s.sketch, placement: p, dim: used.has(s.sketch.id) });
  }
  viewport.setSketches(sk);
  viewport.show(st.mesh, st.faceNames ?? [], st.edgeNames ?? [], firstGeometry && !!st.mesh);
  if (st.mesh) firstGeometry = false;
  const size = st.mesh ? Math.max(40, boundsSize(st.mesh.positions) * 0.6) : 60;
  viewport.setDatums(st.datums, size);
  if (committed) {
    nav.render(st);
    updateTitle();
    updateRibbon();
    const failed = st.features.find((f) => f.state === "failed");
    if (!sketcher.active) {
      if (failed) setStatus(`✗ ${failed.id}: ${failed.message}`);
      else if (st.expressionErrors.length) setStatus(`⚠ ${st.expressionErrors[0]}`);
    }
  }
}

function boundsSize(pos: number[]): number {
  let lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < pos.length; i += 3) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], pos[i + k]); hi[k] = Math.max(hi[k], pos[i + k]); }
  return Math.hypot(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) || 1;
}

async function run(p: Promise<State>, okText?: string): Promise<State | null> {
  try {
    const st = await p;
    show(st);
    if (okText) setStatus(okText);
    return st;
  } catch (err) {
    toast(errorText(err), "error", 6000);
    return null;
  }
}

function updateTitle(): void {
  if (!state) return;
  const name = filePath ? files.baseName(filePath) : `${state.name} (unsaved)`;
  const t = `${dirty() ? "• " : ""}${name} — CADX`;
  document.title = t;
  if (inDesktopApp()) void getCurrentWindow().setTitle(t).catch(() => undefined);
}

// -------------------------------------------------------------- commands

interface Command {
  id: string;
  label: string;
  icon: string;
  group: string;
  key?: string;
  run: () => void;
  enabled?: () => boolean;
  ribbon?: boolean;
}

const hasBody = () => !!state?.mesh;
const notSketching = () => !sketcher.active && !dialog;

const commands: Command[] = [
  { id: "new", label: "New part", icon: "🗋", group: "File", key: "Ctrl+N", run: () => void newPart() },
  { id: "open", label: "Open…", icon: "📂", group: "File", key: "Ctrl+O", run: () => void openFile() },
  { id: "save", label: "Save", icon: "💾", group: "File", key: "Ctrl+S", run: () => void saveFile(false) },
  { id: "saveAs", label: "Save as…", icon: "💾", group: "File", key: "Ctrl+Shift+S", run: () => void saveFile(true), ribbon: false },
  { id: "import", label: "Import STEP/IGES…", icon: "⇩", group: "File", run: () => void importFile(), ribbon: false },
  { id: "export-step", label: "Export STEP…", icon: "⇧", group: "File", run: () => void exportFile("step"), enabled: hasBody, ribbon: false },
  { id: "export-iges", label: "Export IGES…", icon: "⇧", group: "File", run: () => void exportFile("iges"), enabled: hasBody, ribbon: false },
  { id: "export-stl", label: "Export STL…", icon: "⇧", group: "File", run: () => void exportFile("stl"), enabled: hasBody, ribbon: false },
  { id: "export-brep", label: "Export BREP…", icon: "⇧", group: "File", run: () => void exportFile("brep"), enabled: hasBody, ribbon: false },
  { id: "example-bracket", label: "Open example: Bracket", icon: "★", group: "File", run: () => void openExample(bracketExample()), ribbon: false },
  { id: "example-flange", label: "Open example: Flange", icon: "★", group: "File", run: () => void openExample(flangeExample()), ribbon: false },
  { id: "undo", label: "Undo", icon: "↶", group: "Edit", key: "Ctrl+Z", run: () => undo(), enabled: () => sketcher.active || !!state?.canUndo },
  { id: "redo", label: "Redo", icon: "↷", group: "Edit", key: "Ctrl+Y", run: () => redo(), enabled: () => sketcher.active || !!state?.canRedo },

  { id: "sketch", label: "Sketch", icon: "✎", group: "Sketch", key: "S", run: () => newSketch(), enabled: notSketching },

  { id: "extrude", label: "Extrude", icon: FEATURES.extrude.icon, group: "Feature", key: "X", run: () => newFeature("extrude"), enabled: notSketching },
  { id: "revolve", label: "Revolve", icon: FEATURES.revolve.icon, group: "Feature", key: "R", run: () => newFeature("revolve"), enabled: notSketching },
  { id: "hole", label: "Hole", icon: FEATURES.hole.icon, group: "Feature", key: "H", run: () => newFeature("hole"), enabled: () => notSketching() && hasBody() },
  { id: "fillet", label: "Edge Blend", icon: FEATURES.fillet.icon, group: "Feature", key: "B", run: () => newFeature("fillet"), enabled: () => notSketching() && hasBody() },
  { id: "chamfer", label: "Chamfer", icon: FEATURES.chamfer.icon, group: "Feature", key: "C", run: () => newFeature("chamfer"), enabled: () => notSketching() && hasBody() },
  { id: "shell", label: "Shell", icon: FEATURES.shell.icon, group: "Feature", run: () => newFeature("shell"), enabled: () => notSketching() && hasBody() },

  { id: "linearPattern", label: "Linear Pattern", icon: FEATURES.linearPattern.icon, group: "Pattern", run: () => newFeature("linearPattern"), enabled: () => notSketching() && hasBody() },
  { id: "circularPattern", label: "Circular Pattern", icon: FEATURES.circularPattern.icon, group: "Pattern", run: () => newFeature("circularPattern"), enabled: () => notSketching() && hasBody() },
  { id: "mirror", label: "Mirror", icon: FEATURES.mirror.icon, group: "Pattern", run: () => newFeature("mirror"), enabled: () => notSketching() && hasBody() },

  { id: "datumPlane", label: "Datum Plane", icon: FEATURES.datumPlane.icon, group: "Datum", run: () => newFeature("datumPlane"), enabled: notSketching },

  { id: "box", label: "Block", icon: FEATURES.box.icon, group: "Primitive", run: () => newFeature("box"), enabled: notSketching },
  { id: "cylinder", label: "Cylinder", icon: FEATURES.cylinder.icon, group: "Primitive", run: () => newFeature("cylinder"), enabled: notSketching },
  { id: "sphere", label: "Sphere", icon: FEATURES.sphere.icon, group: "Primitive", run: () => newFeature("sphere"), enabled: notSketching },
  { id: "cone", label: "Cone", icon: FEATURES.cone.icon, group: "Primitive", run: () => newFeature("cone"), enabled: notSketching },

  { id: "parameters", label: "Parameters", icon: "ƒ", group: "Tools", key: "Ctrl+E", run: () => void parametersDialog(), enabled: notSketching },
  { id: "measure", label: "Measure", icon: "⚖", group: "Tools", key: "M", run: () => void measureDialog(), enabled: hasBody },
  { id: "finder", label: "Command Finder", icon: "🔍", group: "Tools", key: "Ctrl+K", run: () => commandFinder() },

  { id: "fit", label: "Fit", icon: "⤢", group: "View", key: "F", run: () => viewport.fit() },
  { id: "view-iso", label: "Isometric", icon: "◇", group: "View", key: "Home", run: () => viewport.setView("iso") },
  { id: "view-top", label: "Top", icon: "⊤", group: "View", key: "Ctrl+1", run: () => setView("top") },
  { id: "view-front", label: "Front", icon: "▭", group: "View", key: "Ctrl+2", run: () => setView("front") },
  { id: "view-right", label: "Right", icon: "▯", group: "View", key: "Ctrl+3", run: () => setView("right") },
  { id: "view-bottom", label: "Bottom view", icon: "⊥", group: "View", run: () => setView("bottom"), ribbon: false },
  { id: "view-back", label: "Back view", icon: "▭", group: "View", run: () => setView("back"), ribbon: false },
  { id: "view-left", label: "Left view", icon: "▯", group: "View", run: () => setView("left"), ribbon: false },
  { id: "ortho", label: "Orthographic", icon: "⧈", group: "View", key: "O", run: () => { viewport.setOrthographic(!viewport.orthographic); updateRibbon(); } },
  { id: "display", label: "Display mode", icon: "◐", group: "View", key: "D", run: () => cycleDisplay() },
  { id: "toggle-sketches", label: "Show/hide sketches", icon: "✎", group: "View", run: () => { showSketches = !showSketches; viewport.setSketchesVisible(showSketches); }, ribbon: false },
  { id: "toggle-datums", label: "Show/hide datum planes", icon: "▱", group: "View", run: () => { showDatums = !showDatums; viewport.setDatumsVisible(showDatums); }, ribbon: false },
  { id: "help", label: "Keyboard shortcuts", icon: "?", group: "Help", key: "F1", run: () => void helpDialog(), ribbon: false },
  { id: "about", label: "About CADX", icon: "ℹ", group: "Help", run: () => void aboutDialog(), ribbon: false },
];

const byId = (id: string) => commands.find((c) => c.id === id)!;

function setView(v: ViewName): void {
  viewport.setView(v);
}

function cycleDisplay(): void {
  const modes: DisplayMode[] = ["shaded-edges", "shaded", "wireframe"];
  viewport.setDisplay(modes[(modes.indexOf(viewport.display) + 1) % modes.length]);
  setStatus(`Display: ${viewport.display.replace("-", " with ")}`);
}

// ---------------------------------------------------------------- ribbon

const ribbon = $("ribbon");
const ribbonButtons = new Map<string, HTMLButtonElement>();

function buildRibbon(): void {
  clear(ribbon);
  ribbonButtons.clear();
  const fileBtn = h("button", { class: "file-btn", onclick: (e: MouseEvent) => fileMenu(e.currentTarget as HTMLElement) }, "File ▾");
  ribbon.append(fileBtn);
  const groups = new Map<string, Command[]>();
  for (const c of commands) {
    if (c.ribbon === false || c.group === "File" && c.id !== "save" && c.id !== "open" && c.id !== "new") continue;
    if (c.group === "Help") continue;
    if (!groups.has(c.group)) groups.set(c.group, []);
    groups.get(c.group)!.push(c);
  }
  for (const [name, list] of groups) {
    const g = h("div", { class: "rgroup" },
      h("div", { class: "rbuttons" }, list.map((c) => {
        const b = h("button", { class: name === "View" ? "rbtn icon-only" : "rbtn", title: `${c.label}${c.key ? ` (${c.key})` : ""}`, onclick: () => c.run() },
          h("span", { class: "ri" }, c.icon), h("span", { class: "rl" }, c.label));
        ribbonButtons.set(c.id, b);
        return b;
      })),
      h("div", { class: "rname" }, name));
    ribbon.append(g);
  }
}

function updateRibbon(): void {
  for (const [id, b] of ribbonButtons) {
    const c = byId(id);
    b.disabled = c.enabled ? !c.enabled() : false;
  }
  ribbonButtons.get("ortho")?.classList.toggle("on", viewport.orthographic);
  const u = ribbonButtons.get("undo"), r = ribbonButtons.get("redo");
  if (u) u.title = state?.canUndo ? `Undo ${state.undoLabel} (Ctrl+Z)` : "Undo (Ctrl+Z)";
  if (r) r.title = state?.canRedo ? `Redo ${state.redoLabel} (Ctrl+Y)` : "Redo (Ctrl+Y)";
}

// Sketch ribbon (replaces the model ribbon while sketching).
const sketchBar = $("sketch-bar");

function renderSketchRibbon(): void {
  clear(sketchBar);
  if (!sketcher.active) { sketchBar.style.display = "none"; ribbon.style.display = ""; return; }
  sketchBar.style.display = "";
  ribbon.style.display = "none";
  const tool = (id: Parameters<Sketcher["setTool"]>[0], icon: string, label: string, key: string) =>
    h("button", { class: `rbtn${sketcher.currentTool === id ? " on" : ""}`, title: `${label} (${key})`, onclick: () => sketcher.setTool(id) },
      h("span", { class: "ri" }, icon), h("span", { class: "rl" }, label));
  const can = new Set(sketcher.applicable());
  const con = (type: string, icon: string, label: string, key?: string) =>
    h("button", { class: "rbtn small", disabled: !can.has(type), title: `${label}${key ? ` (${key})` : ""}`, onclick: () => sketcher.applyConstraint(type) },
      h("span", { class: "ri" }, icon), h("span", { class: "rl" }, label));
  const dof = sketcher.dof;
  sketchBar.append(
    h("div", { class: "rgroup" }, h("div", { class: "rbuttons" },
      h("button", { class: "rbtn finish", title: "Finish sketch (Ctrl+Enter)", onclick: () => void finishSketch() }, h("span", { class: "ri" }, "✔"), h("span", { class: "rl" }, "Finish")),
      h("button", { class: "rbtn", title: "Discard changes (Esc twice)", onclick: () => cancelSketch() }, h("span", { class: "ri" }, "✕"), h("span", { class: "rl" }, "Cancel"))),
      h("div", { class: "rname" }, "Sketch")),
    h("div", { class: "rgroup" }, h("div", { class: "rbuttons" },
      tool("select", "⬉", "Select", "Esc"), tool("line", "╱", "Line", "L"), tool("rect", "▭", "Rectangle", "R"),
      tool("circle", "○", "Circle", "C"), tool("arc", "◠", "Arc", "A"), tool("point", "•", "Point", "P")),
      h("div", { class: "rname" }, "Curve")),
    h("div", { class: "rgroup" }, h("div", { class: "rbuttons" },
      tool("dim", "↔", "Dimension", "D"), tool("dimX", "⇿", "Horiz. dim", "Shift+H"), tool("dimY", "↕", "Vert. dim", "Shift+V")),
      h("div", { class: "rname" }, "Dimension")),
    h("div", { class: "rgroup" }, h("div", { class: "rbuttons grid" },
      con("coincident", "⦿", "Coincident", "I"), con("horizontal", "H", "Horizontal", "H"), con("vertical", "V", "Vertical", "V"),
      con("parallel", "∥", "Parallel"), con("perpendicular", "⊥", "Perpendicular"), con("tangent", "T", "Tangent", "T"),
      con("equal", "=", "Equal", "E"), con("midpoint", "M", "Midpoint"), con("pointOn", "◦", "Point on curve"),
      con("symmetric", "⇹", "Symmetric"), con("fix", "⚓", "Fix")),
      h("div", { class: "rname" }, "Constraint")),
    h("div", { class: "rgroup" }, h("div", { class: "rbuttons" },
      h("button", { class: "rbtn", disabled: !sketcher.hasSelection(), title: "Toggle construction (G)", onclick: () => sketcher.toggleConstruction() }, h("span", { class: "ri" }, "┄"), h("span", { class: "rl" }, "Construction")),
      h("button", { class: "rbtn", disabled: !sketcher.hasSelection(), title: "Delete (Del)", onclick: () => sketcher.deleteSelection() }, h("span", { class: "ri" }, "🗑"), h("span", { class: "rl" }, "Delete")),
      h("button", { class: `rbtn${sketcher.gridSnap ? " on" : ""}`, title: "Snap to grid", onclick: () => { sketcher.gridSnap = !sketcher.gridSnap; renderSketchRibbon(); } }, h("span", { class: "ri" }, "#"), h("span", { class: "rl" }, "Grid snap"))),
      h("div", { class: "rname" }, "Edit")),
    h("div", { class: "sketch-dof" + (dof === 0 ? " full" : "") }, dof === null ? "" : dof === 0 ? "Fully constrained" : `${dof} DOF`),
  );
}

// --------------------------------------------------------------- menus

function popupMenu(x: number, y: number, items: ({ label: string; run: () => void; disabled?: boolean; key?: string } | "-")[]): void {
  document.querySelector(".popup")?.remove();
  const menu = h("div", { class: "popup" },
    items.map((it) => it === "-" ? h("hr") : h("button", { disabled: it.disabled, onclick: () => { menu.remove(); it.run(); } },
      h("span", null, it.label), it.key ? h("kbd", null, it.key) : null)));
  document.body.append(menu);
  const r = menu.getBoundingClientRect();
  menu.style.left = `${Math.min(x, window.innerWidth - r.width - 4)}px`;
  menu.style.top = `${Math.min(y, window.innerHeight - r.height - 4)}px`;
  const close = (e: Event) => {
    if (!menu.contains(e.target as Node)) { menu.remove(); document.removeEventListener("mousedown", close, true); }
  };
  setTimeout(() => document.addEventListener("mousedown", close, true));
}

function fileMenu(anchor: HTMLElement): void {
  const r = anchor.getBoundingClientRect();
  const recent = files.recentFiles();
  const cmd = (id: string) => { const c = byId(id); return { label: c.label, run: c.run, key: c.key, disabled: c.enabled ? !c.enabled() : false }; };
  popupMenu(r.left, r.bottom + 2, [
    cmd("new"), cmd("open"), cmd("save"), cmd("saveAs"), "-", cmd("import"),
    cmd("export-step"), cmd("export-iges"), cmd("export-stl"), cmd("export-brep"),
    "-", cmd("example-bracket"), cmd("example-flange"),
    ...(recent.length ? ["-" as const, ...recent.map((p) => ({ label: `↺ ${files.baseName(p)}`, run: () => void openPath(p) }))] : []),
    "-", cmd("help"), cmd("about"),
  ]);
}

function featureMenu(id: string, x: number, y: number): void {
  if (!state) return;
  const f = state.document.features.find((ff) => ff.id === id);
  if (!f) return;
  const idx = state.document.features.indexOf(f);
  const n = state.document.features.length;
  popupMenu(x, y, [
    { label: f.type === "sketch" ? "Edit sketch" : "Edit…", run: () => editFeature(id), key: "dbl-click" },
    ...(f.type === "sketch" ? [{ label: "Edit sketch plane…", run: () => editSketchPlane(id) }] : []),
    { label: "Rename…", run: () => void renameFeature(id) },
    { label: f.suppressed ? "Unsuppress" : "Suppress", run: () => void run(call<State>("editFeature", { id, feature: { suppressed: !f.suppressed } })) },
    ...(f.type === "sketch" ? [{ label: f.hidden ? "Show sketch" : "Hide sketch", run: () => void run(call<State>("editFeature", { id, feature: { hidden: !f.hidden } })) }] : []),
    "-",
    { label: "Roll back to here", run: () => void run(call<State>("setRollback", { id })) },
    { label: "Roll to end", run: () => void run(call<State>("setRollback", { id: null })), disabled: !state.rollback },
    { label: "Move up", run: () => void run(call<State>("moveFeature", { id, index: idx - 1 })), disabled: idx === 0 },
    { label: "Move down", run: () => void run(call<State>("moveFeature", { id, index: idx + 1 })), disabled: idx === n - 1 },
    "-",
    { label: "Delete", run: () => void deleteFeature(id), key: "Del" },
  ]);
}

function viewportMenu(p: Pick | null, e: MouseEvent): void {
  if (sketcher.active || dialog) return;
  const items: ({ label: string; run: () => void } | "-")[] = [];
  if (p?.kind === "face") {
    selection = [p];
    viewport.setSelection(selection);
    items.push({ label: "Sketch on this face", run: () => newSketch() },
      { label: "Datum plane from face", run: () => newFeature("datumPlane") },
      { label: "Shell, removing this face", run: () => newFeature("shell") },
      { label: "Mirror body about this face", run: () => newFeature("mirror") }, "-");
  }
  if (p?.kind === "edge") {
    if (!selection.some((s) => s.name === p.name)) selection = [...selection.filter((s) => s.kind === "edge"), p];
    viewport.setSelection(selection);
    items.push({ label: "Edge blend", run: () => newFeature("fillet") }, { label: "Chamfer", run: () => newFeature("chamfer") }, "-");
  }
  items.push({ label: "Fit", run: () => viewport.fit() }, { label: "Isometric", run: () => viewport.setView("iso") },
    { label: "Top", run: () => setView("top") }, { label: "Front", run: () => setView("front") }, { label: "Right", run: () => setView("right") });
  if (p) items.push("-", { label: `Copy name: ${short(p.name)}`, run: () => void navigator_clip(p.name) });
  popupMenu(e.clientX, e.clientY, items);
}

async function navigator_clip(text: string): Promise<void> {
  try { await window.navigator.clipboard.writeText(text); toast("Copied", "ok", 1200); } catch { /* clipboard not available */ }
}

// ------------------------------------------------------------ selection

viewport.onHover = (p) => {
  statusPick.textContent = p ? `${p.kind === "face" ? "Face" : "Edge"}: ${p.name}` : "";
};

viewport.onClick = (p, e) => {
  if (sketchSession && !sketcher.active) return;
  if (dialog) {
    dialog.pick(p);
    return;
  }
  const add = e.shiftKey || e.ctrlKey;
  if (!p) { if (!add) selection = []; }
  else if (add) {
    const i = selection.findIndex((s) => s.kind === p.kind && s.name === p.name);
    if (i >= 0) selection.splice(i, 1); else selection.push(p);
  } else selection = [p];
  viewport.setSelection(selection);
  setStatus(selection.length ? `${selection.length} selected · ${short(selection[selection.length - 1].name)}` : "");
};

viewport.onContext = (p, e) => viewportMenu(p, e);

// --------------------------------------------------------------- dialogs

const dialogHost: DialogHost = {
  state: () => state!,
  preview: async (feature, editing) => {
    try {
      const st = await call<State>("preview", { feature: editing ? feature : { ...feature, id: undefined } });
      show(st, false);
      return st;
    } catch (err) {
      setStatus(errorText(err));
      return null;
    }
  },
  commit: async (feature, editing) => {
    if (feature.type === "sketch" && !editing) {
      // New sketch: the plane is chosen; open the sketcher.
      void startNewSketch(feature.plane);
      return true;
    }
    const st = editing
      ? await run(call<State>("editFeature", { id: feature.id, feature: { ...feature, id: undefined } }))
      : await run(call<State>("addFeature", { feature }));
    if (!st) return false;
    const id = editing ? feature.id : st.id;
    const status = st.features.find((s) => s.id === id);
    if (status?.state === "failed") toast(`${id}: ${status.message}`, "error", 6000);
    else setStatus(`${editing ? "Edited" : "Added"} ${id}`);
    if (!editing && feature.type !== "sketch") selection = [];
    viewport.setSelection(selection);
    return true;
  },
  cancel: () => { if (state) show(state); },
  setPickFilter: (k) => { viewport.filter = k; viewport.showBasePlanes(false); },
  setSelection: (picks) => viewport.setSelection(picks),
  sketchLines: (id) => {
    const f = state?.document.features.find((x) => x.id === id);
    return ((f?.entities as { id: string; type: string }[]) ?? []).filter((e) => e.type === "line").map((e) => e.id);
  },
};

function openDialog(type: string, feature: Feature, editing: boolean): void {
  if (dialog) dialog.cancel();
  const def = FEATURES[type];
  if (!def) { toast(`No dialog for ${type}`, "error"); return; }
  dialog = new FeatureDialog(taskPanel, def, feature, editing, dialogHost, () => {
    dialog = null;
    viewport.setSelection(selection);
    updateRibbon();
  });
  updateRibbon();
}

function newFeature(type: string): void {
  if (!state || sketcher.active) return;
  const def = FEATURES[type];
  const f = { type, ...def.defaults({ state, selection, lastSketch }) } as Feature;
  openDialog(type, f, false);
}

function editFeature(id: string): void {
  if (!state || sketcher.active) return;
  const f = state.document.features.find((x) => x.id === id);
  if (!f) return;
  if (f.type === "sketch") { void editSketch(id); return; }
  if (f.type === "import") { openDialog("import", f, true); return; }
  openDialog(f.type, f, true);
}

function editSketchPlane(id: string): void {
  const f = state?.document.features.find((x) => x.id === id);
  if (!f) return;
  openDialog("sketchPlane", { id: f.id, type: "sketch", plane: f.plane ?? "XY" }, true);
}

async function renameFeature(id: string): Promise<void> {
  const f = state?.document.features.find((x) => x.id === id);
  if (!f) return;
  const name = await promptBox("Rename", `Display name for ${id}`, f.name ?? "");
  if (name === null) return;
  await run(call<State>("editFeature", { id, feature: { name: name.trim() || null } }));
}

async function deleteFeature(id: string): Promise<void> {
  if (!state) return;
  const users = state.document.features.filter((f) => f.sketch === id || (Array.isArray(f.features) && (f.features as string[]).includes(id)));
  if (users.length && !(await confirmBox("Delete", `${users.map((u) => u.id).join(", ")} use${users.length === 1 ? "s" : ""} ${id} and will fail. Delete anyway?`, "Delete"))) return;
  await run(call<State>("deleteFeature", { id }), `Deleted ${id} (Ctrl+Z to undo)`);
}

// --------------------------------------------------------------- sketching

function newSketch(): void {
  if (!state || sketcher.active || dialog) return;
  const face = selection.find((s) => s.kind === "face")?.name;
  if (face) { void startNewSketch(face); return; }
  // Choose a plane: base planes are shown; click one, a face or a datum, or use the dialog.
  viewport.showBasePlanes(true);
  openDialog("sketchPlane", { type: "sketch", plane: "XY" } as unknown as Feature, false);
  viewport.filter = "face";
  setStatus("Pick a plane or a planar face for the sketch, then OK.");
}

// Clicking a base plane while choosing a sketch plane.
viewport.canvas.addEventListener("click", (e) => {
  if (!dialog || dialog.feature.type !== "sketch") return;
  const base = viewport.pickBasePlane(e);
  if (base && !viewport.pickAt(e, "face")) dialog.pick({ kind: "face", name: base });
});

async function startNewSketch(plane: unknown): Promise<void> {
  viewport.showBasePlanes(false);
  try {
    const st = await call<State>("preview", { feature: { type: "sketch", plane, entities: [], constraints: [] } });
    const s = st.sketches.find((x) => x.sketch.id === st.id);
    const failed = st.features.find((f) => f.id === st.id && f.state === "failed");
    if (!s?.placement || failed) { toast(failed?.message ?? "Could not place the sketch", "error"); return; }
    sketchSession = { id: null, plane };
    enterSketch({ id: st.id!, entities: [], constraints: [] }, s.placement);
  } catch (err) {
    toast(errorText(err), "error");
  }
}

async function editSketch(id: string): Promise<void> {
  if (!state) return;
  const s = state.sketches.find((x) => x.sketch.id === id);
  const f = state.document.features.find((x) => x.id === id)!;
  if (!s?.placement) { toast(`${id} has no plane (the face it was on may be gone). Edit its plane first.`, "error"); return; }
  sketchSession = { id, plane: f.plane };
  // Keep the user's own entities/constraints (values may be expressions); positions from the solve.
  enterSketch({ id, entities: (f.entities as SketchData["entities"]) ?? [], constraints: (f.constraints as SketchData["constraints"]) ?? [] }, s.placement);
}

function enterSketch(sk: SketchData, placement: Placement): void {
  selection = [];
  viewport.setSelection([]);
  viewport.setGhost(true);
  sketcher.start(sk, placement);
  if (state) show(state);
  renderSketchRibbon();
  updateRibbon();
}

function leaveSketch(): void {
  sketcher.stop();
  sketchSession = null;
  viewport.setGhost(false);
  renderSketchRibbon();
  if (state) show(state);
  updateRibbon();
}

async function finishSketch(): Promise<void> {
  if (!sketcher.active || !sketchSession) return;
  const data = sketcher.data();
  const session = sketchSession;
  let st: State | null;
  if (session.id === null) {
    st = await run(call<State>("addFeature", { feature: { type: "sketch", plane: session.plane, entities: data.entities, constraints: data.constraints } }));
    if (st?.id) lastSketch = st.id;
  } else {
    st = await run(call<State>("editFeature", { id: session.id, feature: { entities: data.entities, constraints: data.constraints } }));
    lastSketch = session.id;
  }
  if (!st) return;
  leaveSketch();
  setStatus(`Sketch ${lastSketch} saved. Extrude (X), revolve (R) or hole (H) it next.`);
}

function cancelSketch(): void {
  leaveSketch();
  setStatus("Sketch changes discarded.");
}

// ---------------------------------------------------------------- tools

async function parametersDialog(): Promise<void> {
  if (!state) return;
  type Row = { name: HTMLInputElement; expr: HTMLInputElement; tr: HTMLTableRowElement };
  const rows: Row[] = [];
  const tbody = h("tbody");
  const values = state.parameterValues;
  const addRow = (name = "", expr = "") => {
    const r: Row = {
      name: h("input", { type: "text", value: name, placeholder: "name", spellcheck: false }),
      expr: h("input", { type: "text", value: expr, placeholder: "expression, e.g. width / 2", spellcheck: false }),
      tr: h("tr"),
    };
    const val = name in values ? fmt(values[name], 4) : "";
    r.tr.append(h("td", null, r.name), h("td", null, r.expr), h("td", { class: "val" }, val),
      h("td", null, h("button", { title: "Remove", onclick: () => { r.tr.remove(); rows.splice(rows.indexOf(r), 1); } }, "×")));
    rows.push(r);
    tbody.append(r.tr);
    return r;
  };
  for (const p of state.document.parameters) addRow(p.name, String(p.expression));
  const body = h("div", { class: "params" },
    h("p", { class: "muted" }, "Use parameter names in any dimension or feature value, e.g. ", h("code", null, "width / 2 + 3"), ". Angles in degrees."),
    h("table", null, h("thead", null, h("tr", null, h("th", null, "Name"), h("th", null, "Expression"), h("th", null, "Value"), h("th"))), tbody),
    h("button", { onclick: () => addRow().name.focus() }, "+ Add parameter"),
    state.expressionErrors.length ? h("div", { class: "task-msg error" }, state.expressionErrors.join("\n")) : null);
  if (!rows.length) addRow();
  const r = await modal("Parameters", body, [{ label: "Cancel", value: "no" }, { label: "Apply", value: "yes", primary: true }]);
  if (r !== "yes") return;
  const params = rows.filter((x) => x.name.value.trim()).map((x) => ({ name: x.name.value.trim(), expression: x.expr.value.trim() || "0" }));
  await run(call<State>("setParameters", { parameters: params }), "Parameters updated");
}

const DENSITIES: [string, number][] = [["Aluminium 6061", 2.7], ["Steel", 7.85], ["Stainless 304", 8.0], ["Brass", 8.5],
  ["PLA", 1.24], ["PETG", 1.27], ["ABS", 1.04], ["Nylon", 1.14], ["Acetal (POM)", 1.41], ["MDF", 0.75], ["Oak", 0.75]];

async function measureDialog(): Promise<void> {
  try {
    const m = await call<Measure>("measure");
    if (m.empty) { toast("Nothing to measure yet.", "info"); return; }
    const massOut = h("span");
    const sel = h("select", { onchange: () => upd() }, DENSITIES.map(([n, d]) => h("option", { value: String(d) }, `${n} (${d} g/cm³)`)));
    const upd = () => { massOut.textContent = `${fmt((m.volume / 1000) * Number(sel.value), 2)} g`; };
    upd();
    const size = [0, 1, 2].map((i) => m.bboxMax[i] - m.bboxMin[i]);
    const row = (k: string, v: Node | string) => h("tr", null, h("th", null, k), h("td", null, v));
    const body = h("table", { class: "measure" },
      row("Volume", `${fmt(m.volume, 2)} mm³  (${fmt(m.volume / 1000, 3)} cm³)`),
      row("Surface area", `${fmt(m.area, 2)} mm²`),
      row("Material", sel), row("Mass", massOut),
      row("Centre of mass", m.centreOfMass.map((v) => fmt(v, 3)).join(", ")),
      row("Size (X × Y × Z)", size.map((v) => fmt(v, 3)).join(" × ") + " mm"),
      row("Bounds", `${m.bboxMin.map((v) => fmt(v, 2)).join(", ")}  →  ${m.bboxMax.map((v) => fmt(v, 2)).join(", ")}`),
      row("Topology", `${m.solids} solid${m.solids === 1 ? "" : "s"}, ${m.faces} faces, ${m.edges} edges`));
    await modal("Measure body", body, [{ label: "Close", value: "ok", primary: true }]);
  } catch (err) {
    toast(errorText(err), "error");
  }
}

function commandFinder(): void {
  const input = h("input", { type: "text", placeholder: "Type a command…", class: "wide", spellcheck: false });
  const list = h("div", { class: "finder-list" });
  let matches: Command[] = [];
  let active = 0;
  const render = () => {
    const q = input.value.toLowerCase().trim();
    matches = commands.filter((c) => (c.enabled ? c.enabled() : true) && (`${c.label} ${c.group} ${c.id}`.toLowerCase().includes(q)));
    active = Math.min(active, Math.max(0, matches.length - 1));
    clear(list);
    matches.slice(0, 14).forEach((c, i) => list.append(h("div", { class: `finder-item${i === active ? " active" : ""}`, onmousedown: () => pick(c) },
      h("span", { class: "ri" }, c.icon), h("span", null, c.label), h("span", { class: "muted" }, c.group), c.key ? h("kbd", null, c.key) : null)));
  };
  const overlay = h("div", { class: "modal-overlay", onmousedown: (e: MouseEvent) => { if (e.target === overlay) overlay.remove(); } },
    h("div", { class: "modal finder" }, input, list));
  const pick = (c: Command) => { overlay.remove(); c.run(); };
  input.addEventListener("input", () => { active = 0; render(); });
  input.addEventListener("keydown", (e) => {
    e.stopPropagation();
    if (e.key === "Escape") overlay.remove();
    if (e.key === "ArrowDown") { active = Math.min(active + 1, matches.length - 1); render(); e.preventDefault(); }
    if (e.key === "ArrowUp") { active = Math.max(active - 1, 0); render(); e.preventDefault(); }
    if (e.key === "Enter" && matches[active]) pick(matches[active]);
  });
  document.body.append(overlay);
  render();
  input.focus();
}

async function helpDialog(): Promise<void> {
  const rows = commands.filter((c) => c.key).map((c) => h("tr", null, h("td", null, h("kbd", null, c.key!)), h("td", null, c.label)));
  const sketchKeys: [string, string][] = [["L / R / C / A / P", "Line, rectangle, circle, arc, point"], ["D", "Dimension"],
    ["H / V / I / T / E", "Horizontal, vertical, coincident, tangent, equal"], ["G", "Toggle construction"], ["Del", "Delete selection"],
    ["Esc", "End the current tool / clear selection"], ["Ctrl+Enter", "Finish sketch"]];
  const mouse: [string, string][] = [["Middle drag (or Alt+left)", "Rotate"], ["Right drag (or Shift+middle)", "Pan"], ["Wheel", "Zoom at cursor"],
    ["Left click", "Select (Shift/Ctrl to add)"], ["Right click", "Context menu"]];
  await modal("Keyboard and mouse", h("div", { class: "help" },
    h("h4", null, "Model"), h("table", null, rows),
    h("h4", null, "Sketch"), h("table", null, sketchKeys.map(([k, v]) => h("tr", null, h("td", null, h("kbd", null, k)), h("td", null, v)))),
    h("h4", null, "Mouse"), h("table", null, mouse.map(([k, v]) => h("tr", null, h("td", null, k), h("td", null, v))))),
    [{ label: "Close", value: "ok", primary: true }]);
}

async function aboutDialog(): Promise<void> {
  let version = "?";
  try { version = (await call<{ version: string }>("ping")).version; } catch { /* no engine */ }
  await modal("About CADX", h("div", null,
    h("p", null, h("strong", null, "CADX"), " — parametric CAD with persistent naming."),
    h("p", { class: "muted" }, `Engine ${version} · OpenCASCADE kernel · PlaneGCS sketch solver (both LGPL).`)),
    [{ label: "Close", value: "ok", primary: true }]);
}

// ----------------------------------------------------------------- files

async function confirmDiscard(): Promise<boolean> {
  if (!dirty()) return true;
  return confirmBox("Unsaved changes", `${state?.name ?? "The part"} has unsaved changes. Discard them?`, "Discard");
}

async function newPart(): Promise<void> {
  if (sketcher.active) cancelSketch();
  if (!(await confirmDiscard())) return;
  const st = await run(call<State>("newDocument", { name: "Part1" }));
  if (st) { filePath = null; savedRevision = st.revision; firstGeometry = true; selection = []; updateTitle(); viewport.setView("iso"); }
}

async function openFile(): Promise<void> {
  if (!(await confirmDiscard())) return;
  const path = await files.pickOpen();
  if (path) await openPath(path, true);
}

async function openPath(path: string, confirmed = false): Promise<void> {
  if (!confirmed && !(await confirmDiscard())) return;
  if (sketcher.active) cancelSketch();
  try {
    let st: State | null;
    if (files.isNative(path)) {
      const doc = await files.readDocument(path);
      st = await run(call<State>("openDocument", { document: doc }));
      if (st) filePath = path;
    } else {
      // A STEP/IGES/BREP file opens as a new part holding the import.
      const name = files.baseName(path).replace(/\.[^.]+$/, "");
      await call<State>("newDocument", { name });
      st = await run(call<State>("importFile", { path }));
      filePath = null;
    }
    if (!st) return;
    savedRevision = files.isNative(path) ? st.revision : -1;
    firstGeometry = true;
    selection = [];
    show(st);
    viewport.setView("iso");
    files.addRecent(path);
    setStatus(`Opened ${files.baseName(path)}`);
  } catch (err) {
    toast(`Could not open ${files.baseName(path)}: ${errorText(err)}`, "error", 7000);
    files.removeRecent(path);
  }
}

async function openExample(doc: State["document"]): Promise<void> {
  if (sketcher.active) cancelSketch();
  if (!(await confirmDiscard())) return;
  const st = await run(call<State>("openDocument", { document: doc }));
  if (!st) return;
  filePath = null;
  savedRevision = -1;
  firstGeometry = true;
  selection = [];
  show(st);
  viewport.setView("iso");
  setStatus(`Example ${doc.name}: edit its parameters (Ctrl+E) or double-click a feature.`);
}

async function saveFile(as: boolean): Promise<boolean> {
  if (!state) return false;
  let path = filePath;
  if (as || !path) path = await files.pickSave(state.name);
  if (!path) return false;
  try {
    const { document, revision } = await call<{ document: State["document"]; revision: number }>("getDocument");
    const stem = files.baseName(path).replace(/\.cadx$/i, "");
    if (document.name !== stem && (!filePath || as)) {
      const st = await call<State>("setName", { name: stem });
      document.name = stem;
      show(st);
      savedRevision = st.revision;
    } else savedRevision = revision;
    await files.writeDocument(path, document);
    filePath = path;
    files.addRecent(path);
    updateTitle();
    setStatus(`Saved ${files.baseName(path)}`);
    return true;
  } catch (err) {
    toast(`Save failed: ${errorText(err)}`, "error", 7000);
    return false;
  }
}

async function importFile(): Promise<void> {
  const path = await files.pickImport();
  if (!path) return;
  const st = await run(call<State>("importFile", { path }));
  if (st) { firstGeometry = true; show(st); setStatus(`Imported ${files.baseName(path)} as ${st.id}`); }
}

async function exportFile(format: string): Promise<void> {
  if (!state) return;
  const path = await files.pickExport(format, filePath ? files.baseName(filePath).replace(/\.cadx$/i, "") : state.name);
  if (!path) return;
  try {
    await call("exportFile", { path, format });
    toast(`Exported ${files.baseName(path)}`, "ok");
  } catch (err) {
    toast(`Export failed: ${errorText(err)}`, "error", 7000);
  }
}

// -------------------------------------------------------------- undo/redo

function undo(): void {
  if (sketcher.active) { sketcher.undo(); return; }
  if (dialog) return;
  if (state?.canUndo) void run(call<State>("undo"), `Undid ${state.undoLabel}`);
}

function redo(): void {
  if (sketcher.active) { sketcher.redo(); return; }
  if (dialog) return;
  if (state?.canRedo) void run(call<State>("redo"), `Redid ${state.redoLabel}`);
}

// --------------------------------------------------------------- keyboard

function keyName(e: KeyboardEvent): string {
  const parts = [];
  if (e.ctrlKey || e.metaKey) parts.push("Ctrl");
  if (e.shiftKey && (e.ctrlKey || e.metaKey || e.key.length > 1)) parts.push("Shift");
  let k = e.key.length === 1 ? e.key.toUpperCase() : e.key;
  if (k === " ") k = "Space";
  parts.push(k);
  return parts.join("+");
}

document.addEventListener("keydown", (e) => {
  const t = e.target as HTMLElement;
  if (t instanceof HTMLInputElement || t instanceof HTMLTextAreaElement || t instanceof HTMLSelectElement) {
    if (e.key === "Escape") t.blur();
    return;
  }
  if (document.querySelector(".modal-overlay")) return;
  const k = keyName(e);

  if (sketcher.active) {
    const map: Record<string, () => void> = {
      Escape: () => sketcher.escape(), Delete: () => sketcher.deleteSelection(), Backspace: () => sketcher.deleteSelection(),
      L: () => sketcher.setTool("line"), R: () => sketcher.setTool("rect"), C: () => sketcher.setTool("circle"),
      A: () => sketcher.setTool("arc"), P: () => sketcher.setTool("point"), D: () => sketcher.setTool("dim"),
      "Shift+H": () => sketcher.setTool("dimX"), "Shift+V": () => sketcher.setTool("dimY"),
      H: () => sketcher.applyConstraint("horizontal"), V: () => sketcher.applyConstraint("vertical"),
      I: () => sketcher.applyConstraint("coincident"), T: () => sketcher.applyConstraint("tangent"),
      E: () => sketcher.applyConstraint("equal"), G: () => sketcher.toggleConstruction(),
      "Ctrl+Z": () => sketcher.undo(), "Ctrl+Y": () => sketcher.redo(), "Ctrl+Shift+Z": () => sketcher.redo(),
      "Ctrl+A": () => sketcher.selectAll(), "Ctrl+Enter": () => void finishSketch(), F: () => viewport.fit(),
      "Ctrl+K": () => commandFinder(),
    };
    // Shift+H / Shift+V arrive as "H"/"V" with shiftKey.
    const sk = e.shiftKey && !e.ctrlKey && e.key.length === 1 ? `Shift+${e.key.toUpperCase()}` : k;
    const f = map[sk] ?? map[k];
    if (f) { e.preventDefault(); f(); renderSketchRibbon(); }
    return;
  }

  if (k === "Escape") {
    if (dialog) dialog.cancel();
    else { selection = []; viewport.setSelection([]); nav.selected = null; viewport.highlightFeature(""); if (state) nav.render(state); }
    return;
  }
  if (k === "Enter" && dialog) { e.preventDefault(); void dialog.ok(); return; }
  if ((k === "Delete" || k === "Backspace") && nav.selected && !dialog) { e.preventDefault(); void deleteFeature(nav.selected); return; }
  if (k === "Ctrl+Shift+Z") { e.preventDefault(); redo(); return; }
  if (k === "/") { e.preventDefault(); commandFinder(); return; }
  const c = commands.find((x) => x.key === k);
  if (c && (!c.enabled || c.enabled())) { e.preventDefault(); c.run(); }
});

// ----------------------------------------------------------------- start

buildRibbon();
renderSketchRibbon();

async function start(): Promise<void> {
  if (!engineAvailable()) {
    setStatus("Browser preview: no engine. Run the CADX desktop app to build geometry.");
    $("tree").append(h("li", { class: "nav-empty" }, "No engine in a plain browser."));
    return;
  }
  try {
    const info = await call<{ version: string }>("ping");
    setStatus(`Engine ${info.version} ready. Start with a sketch (S) or a primitive.`);
    const st = await call<State>("getState");
    savedRevision = st.revision;
    show(st);
    viewport.setView("iso");
    const path = inDesktopApp() ? await files.startupFile() : null;
    if (path) await openPath(path, true);
  } catch (err) {
    setStatus(errorText(err));
    toast(`Engine problem: ${errorText(err)}`, "error", 10000);
  }
  if (inDesktopApp()) {
    void getCurrentWindow().onCloseRequested(async (event) => {
      if (dirty() && !(await confirmBox("Unsaved changes", "Quit without saving?", "Quit"))) event.preventDefault();
    });
  }
}

void start();

// For the command palette and tests.
Object.assign(window, { cadx: { commands, state: () => state, ICONS, viewport, sketcher, selection: () => selection } });
