// Feature dialogs: a task panel with fields, live preview, OK and Cancel.
// Each feature type is described by a FeatureDef; the panel is generic.

import { clear, debounce, h } from "./dom";
import type { Feature, FeatureStatus, Num, Pick, State } from "./types";

export type FieldKind =
  | "number" | "int" | "select" | "check" | "text" | "sketch" | "edges" | "faces" | "features"
  | "plane" | "direction" | "axis" | "position";

export interface FieldDef {
  key: string;
  label: string;
  kind: FieldKind;
  options?: [string, string][];
  show?: (f: Feature) => boolean;
  unit?: string;
  optional?: boolean;
  hint?: string;
}

export interface FeatureDef {
  type: string;
  title: string;
  icon: string;
  fields: FieldDef[];
  defaults: (ctx: DialogContext) => Record<string, unknown>;
}

export interface DialogContext {
  state: State;
  selection: Pick[];
  lastSketch?: string;
}

export interface DialogHost {
  state(): State;
  preview(feature: Feature, editing: boolean): Promise<State | null>;
  commit(feature: Feature, editing: boolean): Promise<boolean>;
  cancel(): void;
  setPickFilter(kind: "face" | "edge" | "any" | "none"): void;
  setSelection(picks: Pick[]): void;
  sketchLines(sketchId: string): string[];
}

const OPS: [string, string][] = [["add", "Unite (add)"], ["cut", "Subtract (cut)"], ["intersect", "Intersect"]];
const AXES: [string, string][] = [["Z", "Z axis"], ["X", "X axis"], ["Y", "Y axis"]];
const DIRS: [string, string][] = [["X", "+X"], ["Y", "+Y"], ["Z", "+Z"], ["-X", "−X"], ["-Y", "−Y"], ["-Z", "−Z"]];
const PLANES: [string, string][] = [["XY", "XY plane"], ["XZ", "XZ plane"], ["YZ", "YZ plane"]];

const sketchIds = (s: State) => s.document.features.filter((f) => f.type === "sketch").map((f) => f.id);
const patternable = new Set(["extrude", "revolve", "hole", "box", "cylinder", "sphere", "cone", "import"]);

function lastSketch(ctx: DialogContext): string {
  const ids = sketchIds(ctx.state);
  return ctx.lastSketch && ids.includes(ctx.lastSketch) ? ctx.lastSketch : ids[ids.length - 1] ?? "";
}

const picked = (ctx: DialogContext, kind: "face" | "edge") => ctx.selection.filter((p) => p.kind === kind).map((p) => p.name);
const bodyOp = (ctx: DialogContext) => (ctx.state.mesh ? "add" : "add");

export const FEATURES: Record<string, FeatureDef> = {
  extrude: {
    type: "extrude", title: "Extrude", icon: "⬆",
    fields: [
      { key: "sketch", label: "Sketch", kind: "sketch" },
      { key: "op", label: "Boolean", kind: "select", options: OPS },
      { key: "direction", label: "Direction", kind: "select", options: [["normal", "Along normal"], ["reverse", "Reversed"], ["symmetric", "Symmetric"], ["twoSided", "Two-sided"]] },
      { key: "through", label: "Through all", kind: "check" },
      { key: "flip", label: "Flip side", kind: "check", show: (f) => !!f.through && f.direction !== "symmetric" && f.direction !== "twoSided" },
      { key: "distance", label: "Distance", kind: "number", unit: "mm", show: (f) => !f.through },
      { key: "distance2", label: "Second distance", kind: "number", unit: "mm", show: (f) => !f.through && f.direction === "twoSided" },
    ],
    defaults: (ctx) => ({ sketch: lastSketch(ctx), op: bodyOp(ctx), direction: "normal", distance: 10, distance2: 5 }),
  },
  revolve: {
    type: "revolve", title: "Revolve", icon: "↻",
    fields: [
      { key: "sketch", label: "Sketch", kind: "sketch" },
      { key: "axis", label: "Axis", kind: "axis", hint: "H/V are the sketch's own axes; pick a sketch line by id or an edge" },
      { key: "angle", label: "Angle", kind: "number", unit: "°" },
      { key: "op", label: "Boolean", kind: "select", options: OPS },
    ],
    defaults: (ctx) => ({ sketch: lastSketch(ctx), axis: "V", angle: 360, op: "add" }),
  },
  hole: {
    type: "hole", title: "Hole", icon: "◎",
    fields: [
      { key: "sketch", label: "Points sketch", kind: "sketch", hint: "Every point in the sketch becomes a hole, drilled against the sketch normal" },
      { key: "form", label: "Form", kind: "select", options: [["simple", "Simple"], ["counterbore", "Counterbore"], ["countersink", "Countersink"]] },
      { key: "diameter", label: "Diameter", kind: "number", unit: "mm" },
      { key: "through", label: "Through all", kind: "check" },
      { key: "depth", label: "Depth", kind: "number", unit: "mm", show: (f) => !f.through },
      { key: "tipAngle", label: "Tip angle (0 = flat)", kind: "number", unit: "°", show: (f) => !f.through },
      { key: "cbDiameter", label: "Counterbore ⌀", kind: "number", unit: "mm", show: (f) => f.form === "counterbore" },
      { key: "cbDepth", label: "Counterbore depth", kind: "number", unit: "mm", show: (f) => f.form === "counterbore" },
      { key: "csDiameter", label: "Countersink ⌀", kind: "number", unit: "mm", show: (f) => f.form === "countersink" },
      { key: "csAngle", label: "Countersink angle", kind: "number", unit: "°", show: (f) => f.form === "countersink" },
    ],
    defaults: (ctx) => ({ sketch: lastSketch(ctx), form: "simple", diameter: 6, through: true, depth: 10, tipAngle: 118,
      cbDiameter: 11, cbDepth: 6, csDiameter: 12, csAngle: 90 }),
  },
  fillet: {
    type: "fillet", title: "Edge Blend", icon: "◜",
    fields: [{ key: "edges", label: "Edges", kind: "edges" }, { key: "radius", label: "Radius", kind: "number", unit: "mm" }],
    defaults: (ctx) => ({ edges: picked(ctx, "edge"), radius: 2 }),
  },
  chamfer: {
    type: "chamfer", title: "Chamfer", icon: "◸",
    fields: [{ key: "edges", label: "Edges", kind: "edges" }, { key: "distance", label: "Distance", kind: "number", unit: "mm" }],
    defaults: (ctx) => ({ edges: picked(ctx, "edge"), distance: 1 }),
  },
  shell: {
    type: "shell", title: "Shell", icon: "▢",
    fields: [{ key: "faces", label: "Faces to remove", kind: "faces" }, { key: "thickness", label: "Thickness", kind: "number", unit: "mm" }],
    defaults: (ctx) => ({ faces: picked(ctx, "face"), thickness: 2 }),
  },
  linearPattern: {
    type: "linearPattern", title: "Linear Pattern", icon: "⋯",
    fields: [
      { key: "body", label: "Pattern the whole body", kind: "check" },
      { key: "features", label: "Features", kind: "features", show: (f) => !f.body },
      { key: "direction", label: "Direction", kind: "direction" },
      { key: "count", label: "Count", kind: "int" },
      { key: "spacing", label: "Spacing", kind: "number", unit: "mm" },
      { key: "direction2", label: "Second direction", kind: "direction", optional: true },
      { key: "count2", label: "Count 2", kind: "int", show: (f) => !!f.direction2 },
      { key: "spacing2", label: "Spacing 2", kind: "number", unit: "mm", show: (f) => !!f.direction2 },
    ],
    defaults: () => ({ body: false, features: [], direction: "X", count: 3, spacing: 20, direction2: "", count2: 2, spacing2: 20 }),
  },
  circularPattern: {
    type: "circularPattern", title: "Circular Pattern", icon: "✣",
    fields: [
      { key: "body", label: "Pattern the whole body", kind: "check" },
      { key: "features", label: "Features", kind: "features", show: (f) => !f.body },
      { key: "axis", label: "Axis", kind: "axis" },
      { key: "count", label: "Count", kind: "int" },
      { key: "angle", label: "Total angle", kind: "number", unit: "°", hint: "360 spaces the copies evenly round a full circle" },
    ],
    defaults: () => ({ body: false, features: [], axis: "Z", count: 6, angle: 360 }),
  },
  mirror: {
    type: "mirror", title: "Mirror", icon: "⇋",
    fields: [
      { key: "body", label: "Mirror the whole body", kind: "check" },
      { key: "features", label: "Features", kind: "features", show: (f) => !f.body },
      { key: "plane", label: "Mirror plane", kind: "plane" },
    ],
    defaults: (ctx) => ({ body: true, features: [], plane: picked(ctx, "face")[0] ?? "YZ" }),
  },
  datumPlane: {
    type: "datumPlane", title: "Datum Plane", icon: "▱",
    fields: [
      { key: "base", label: "Base plane or face", kind: "plane" },
      { key: "offset", label: "Offset", kind: "number", unit: "mm" },
      { key: "flip", label: "Flip normal", kind: "check" },
    ],
    defaults: (ctx) => ({ base: picked(ctx, "face")[0] ?? "XY", offset: 20, flip: false }),
  },
  box: {
    type: "box", title: "Block", icon: "■",
    fields: [
      { key: "length", label: "Length (X)", kind: "number", unit: "mm" },
      { key: "width", label: "Width (Y)", kind: "number", unit: "mm" },
      { key: "height", label: "Height (Z)", kind: "number", unit: "mm" },
      { key: "position", label: "Corner at", kind: "position" },
      { key: "op", label: "Boolean", kind: "select", options: OPS },
    ],
    defaults: () => ({ length: 50, width: 30, height: 20, position: [0, 0, 0], op: "add" }),
  },
  cylinder: {
    type: "cylinder", title: "Cylinder", icon: "⬤",
    fields: [
      { key: "radius", label: "Radius", kind: "number", unit: "mm" },
      { key: "height", label: "Height", kind: "number", unit: "mm" },
      { key: "axis", label: "Axis", kind: "select", options: AXES },
      { key: "position", label: "Base centre", kind: "position" },
      { key: "op", label: "Boolean", kind: "select", options: OPS },
    ],
    defaults: () => ({ radius: 10, height: 30, axis: "Z", position: [0, 0, 0], op: "add" }),
  },
  sphere: {
    type: "sphere", title: "Sphere", icon: "●",
    fields: [
      { key: "radius", label: "Radius", kind: "number", unit: "mm" },
      { key: "position", label: "Centre", kind: "position" },
      { key: "op", label: "Boolean", kind: "select", options: OPS },
    ],
    defaults: () => ({ radius: 15, position: [0, 0, 0], op: "add" }),
  },
  cone: {
    type: "cone", title: "Cone", icon: "▲",
    fields: [
      { key: "r1", label: "Base radius", kind: "number", unit: "mm" },
      { key: "r2", label: "Top radius", kind: "number", unit: "mm" },
      { key: "height", label: "Height", kind: "number", unit: "mm" },
      { key: "axis", label: "Axis", kind: "select", options: AXES },
      { key: "position", label: "Base centre", kind: "position" },
      { key: "op", label: "Boolean", kind: "select", options: OPS },
    ],
    defaults: () => ({ r1: 15, r2: 5, height: 25, axis: "Z", position: [0, 0, 0], op: "add" }),
  },
  sketchPlane: {
    type: "sketch", title: "Sketch Plane", icon: "✎",
    fields: [{ key: "plane", label: "Plane", kind: "plane", hint: "Pick a planar face or a datum plane, or choose a base plane" }],
    defaults: (ctx) => ({ plane: picked(ctx, "face")[0] ?? "XY" }),
  },
  import: {
    type: "import", title: "Import", icon: "⇩",
    fields: [{ key: "op", label: "Boolean", kind: "select", options: OPS }],
    defaults: () => ({ op: "add" }),
  },
};

export const ICONS: Record<string, string> = {
  sketch: "✎", ...Object.fromEntries(Object.values(FEATURES).filter((d) => d.type !== "sketch").map((d) => [d.type, d.icon])),
};

export const parseNum = (text: string): Num => {
  const t = text.trim();
  return t !== "" && /^-?(\d+\.?\d*|\.\d+)(e-?\d+)?$/i.test(t) ? Number(t) : t;
};

/** The task panel for one feature. */
export class FeatureDialog {
  private f: Feature;
  private activePick: string | null = null;
  private body: HTMLElement;
  private msg: HTMLElement;
  private schedulePreview: () => void;
  private closed = false;

  constructor(private panel: HTMLElement, private def: FeatureDef, feature: Feature, private editing: boolean,
              private host: DialogHost, private onClose: (committed: boolean) => void) {
    this.f = JSON.parse(JSON.stringify(feature)) as Feature;
    this.body = h("div", { class: "task-fields" });
    this.msg = h("div", { class: "task-msg" });
    this.schedulePreview = debounce(() => void this.preview(), 180);
    clear(panel);
    panel.append(
      h("div", { class: "task-head" }, h("span", { class: "task-icon" }, def.icon),
        h("h3", null, `${editing ? "Edit" : "New"} ${def.title}`), h("span", { class: "task-id" }, feature.id ?? "")),
      this.body, this.msg,
      h("div", { class: "task-buttons" },
        h("button", { onclick: () => this.cancel() }, "Cancel"),
        h("button", { class: "primary", onclick: () => void this.ok() }, "OK")));
    panel.classList.add("open");
    // First selection field is active.
    const firstPick = def.fields.find((fd) => ["edges", "faces", "plane", "axis", "direction"].includes(fd.kind));
    if (firstPick && (firstPick.kind === "edges" || firstPick.kind === "faces")) this.activate(firstPick);
    this.render();
    void this.preview();
  }

  get feature(): Feature { return this.f; }

  /** A viewport pick while the dialog is open. Returns true if used. */
  pick(p: Pick | null): boolean {
    if (!this.activePick || !p) return false;
    const fd = this.def.fields.find((x) => x.key === this.activePick);
    if (!fd) return false;
    if (fd.kind === "edges" || fd.kind === "faces") {
      if (p.kind !== (fd.kind === "edges" ? "edge" : "face")) return true;
      const list = [...((this.f[fd.key] as string[]) ?? [])];
      const i = list.indexOf(p.name);
      if (i >= 0) list.splice(i, 1); else list.push(p.name);
      this.f[fd.key] = list;
    } else {
      this.f[fd.key] = p.name;
      this.activePick = null;
      this.host.setPickFilter("any");
    }
    this.changed();
    return true;
  }

  private activate(fd: FieldDef): void {
    this.activePick = fd.key;
    const k = fd.kind === "edges" ? "edge" : fd.kind === "faces" || fd.kind === "plane" ? "face" : "any";
    this.host.setPickFilter(k);
    this.syncSelection();
  }

  private syncSelection(): void {
    const picks: Pick[] = [];
    for (const fd of this.def.fields) {
      if (fd.kind === "edges") for (const n of (this.f[fd.key] as string[]) ?? []) picks.push({ kind: "edge", name: n });
      if (fd.kind === "faces") for (const n of (this.f[fd.key] as string[]) ?? []) picks.push({ kind: "face", name: n });
    }
    this.host.setSelection(picks);
  }

  private changed(): void {
    this.render();
    this.syncSelection();
    this.schedulePreview();
  }

  private render(): void {
    clear(this.body);
    const state = this.host.state();
    for (const fd of this.def.fields) {
      if (fd.show && !fd.show(this.f)) continue;
      this.body.append(this.field(fd, state));
    }
  }

  private field(fd: FieldDef, state: State): HTMLElement {
    const value = this.f[fd.key];
    const set = (v: unknown) => { this.f[fd.key] = v; this.changed(); };
    const row = (control: Node, extra?: Node) =>
      h("label", { class: "field", title: fd.hint ?? "" }, h("span", null, fd.label), control, fd.unit ? h("em", null, fd.unit) : null, extra ?? null);

    switch (fd.kind) {
      case "number": case "int": {
        const input = h("input", { type: "text", value: value === undefined ? "" : String(value), spellcheck: false,
          onchange: () => set(parseNum(input.value)),
          onkeydown: (e: KeyboardEvent) => { if (e.key === "Enter") set(parseNum(input.value)); },
        });
        if (typeof value === "string") input.classList.add("expr");
        return row(input);
      }
      case "text": {
        const input = h("input", { type: "text", value: String(value ?? ""), onchange: () => set(input.value) });
        return row(input);
      }
      case "check": {
        const input = h("input", { type: "checkbox", checked: !!value, onchange: () => set(input.checked) });
        return h("label", { class: "field check" }, input, h("span", null, fd.label));
      }
      case "select": {
        const sel = h("select", { onchange: () => set(sel.value) },
          (fd.options ?? []).map(([v, l]) => h("option", { value: v, selected: v === value }, l)));
        return row(sel);
      }
      case "sketch": {
        const ids = sketchIds(state);
        const sel = h("select", { onchange: () => set(sel.value) },
          ids.length ? ids.map((id) => h("option", { value: id, selected: id === value }, id)) : h("option", { value: "" }, "(no sketches yet)"));
        return row(sel);
      }
      case "position": {
        const v = (Array.isArray(value) ? value : [0, 0, 0]) as Num[];
        const inputs = [0, 1, 2].map((i) => {
          const inp = h("input", { type: "text", class: "xyz", value: String(v[i] ?? 0),
            onchange: () => { const nv = [...v]; nv[i] = parseNum(inp.value); set(nv); } });
          return inp;
        });
        return row(h("span", { class: "xyz-row" }, inputs));
      }
      case "features": {
        const chosen = new Set((value as string[]) ?? []);
        const editingIndex = this.editing ? state.document.features.findIndex((f) => f.id === this.f.id) : state.document.features.length;
        const eligible = state.document.features.slice(0, editingIndex < 0 ? undefined : editingIndex).filter((f) => patternable.has(f.type));
        const list = h("div", { class: "checklist" },
          eligible.length ? eligible.map((f) => {
            const cb = h("input", { type: "checkbox", checked: chosen.has(f.id), onchange: () => {
              if (cb.checked) chosen.add(f.id); else chosen.delete(f.id);
              set(eligible.map((x) => x.id).filter((id) => chosen.has(id)));
            } });
            return h("label", null, cb, ` ${ICONS[f.type] ?? ""} ${f.name ?? f.id}`);
          }) : h("em", null, "No extrudes, revolves, holes or primitives to repeat"));
        return h("div", { class: "field block" }, h("span", null, fd.label), list);
      }
      case "edges": case "faces": {
        const items = (value as string[]) ?? [];
        const active = this.activePick === fd.key;
        const box = h("div", { class: `picklist${active ? " active" : ""}`, onclick: () => { this.activate(fd); this.render(); } },
          items.length ? items.map((n) => h("div", { class: "chip", title: n },
            h("span", null, short(n)),
            h("button", { onclick: (e: MouseEvent) => { e.stopPropagation(); set(items.filter((x) => x !== n)); } }, "×")))
            : h("em", null, active ? `Click ${fd.kind} in the view` : `Click here, then pick ${fd.kind}`));
        return h("div", { class: "field block" }, h("span", null, `${fd.label} (${items.length})`), box);
      }
      case "plane": case "direction": case "axis": {
        let opts: [string, string][] = fd.kind === "plane" ? [...PLANES, ...state.datums.map((d) => [d.id, d.id] as [string, string])]
          : fd.kind === "direction" ? [...DIRS] : [...AXES];
        if (fd.kind === "axis" && this.def.type === "revolve") {
          opts = [["V", "Sketch vertical (V)"], ["H", "Sketch horizontal (H)"],
            ...this.host.sketchLines(String(this.f.sketch ?? "")).map((l) => [l, `Sketch line ${l}`] as [string, string]), ...AXES];
        }
        if (fd.optional) opts = [["", "(none)"], ...opts];
        const v = String(value ?? "");
        const isPicked = v !== "" && !opts.some(([o]) => o === v);
        if (isPicked) opts = [...opts, [v, `Picked: ${short(v)}`]];
        const sel = h("select", { onchange: () => set(sel.value) }, opts.map(([o, l]) => h("option", { value: o, selected: o === v }, l)));
        const active = this.activePick === fd.key;
        const pickBtn = h("button", { class: `pick-btn${active ? " active" : ""}`, title: fd.kind === "plane" ? "Pick a planar face" : "Pick an edge or face",
          onclick: (e: MouseEvent) => { e.preventDefault(); if (active) { this.activePick = null; this.host.setPickFilter("any"); } else this.activate(fd); this.render(); } }, "⌖");
        return row(sel, pickBtn);
      }
    }
  }

  private async preview(): Promise<void> {
    if (this.closed) return;
    const st = await this.host.preview(this.f, this.editing);
    if (!st || this.closed) return;
    const id = this.editing ? this.f.id : st.id;
    const status: FeatureStatus | undefined = st.features.find((s) => s.id === id);
    this.msg.textContent = status && status.state === "failed" ? `⚠ ${status.message}` : "";
    this.msg.className = `task-msg${status?.state === "failed" ? " error" : ""}`;
  }

  async ok(): Promise<void> {
    const done = await this.host.commit(this.f, this.editing);
    if (done) this.close(true);
  }

  cancel(): void {
    this.host.cancel();
    this.close(false);
  }

  private close(committed: boolean): void {
    this.closed = true;
    this.panel.classList.remove("open");
    clear(this.panel);
    this.host.setPickFilter("any");
    this.onClose(committed);
  }
}

/** Shorten a persistent name for display. */
export function short(name: string): string {
  return name.length > 48 ? name.slice(0, 22) + "…" + name.slice(-22) : name;
}
