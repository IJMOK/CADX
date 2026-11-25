// Sketch mode: draw and constrain 2D geometry on the sketch plane. Geometry is
// solved by the engine (solveSketch); this file only edits the sketch JSON and
// draws it.

import * as THREE from "three";
import { clear, fmt, h, toast } from "./dom";
import type { Constraint, Entity, Num, Placement, SketchData, SketchStatus, Vec2 } from "./types";
import { entityPolyline, planeToWorld, v3, type InputHandler, type Viewport } from "./viewport";

type Tool = "select" | "line" | "rect" | "circle" | "arc" | "point" | "dim" | "dimX" | "dimY";
type Sel = { kind: "point"; ref: string } | { kind: "entity"; id: string } | { kind: "constraint"; id: string };

interface Snap {
  pos: Vec2;
  ref?: string;     // snapped to this point
  onCurve?: string; // snapped onto this curve
}

export interface SketcherHost {
  solve(sketch: SketchData): Promise<SketchStatus>;
  finish(sketch: SketchData): void;
  cancel(): void;
  status(text: string): void;
}

const COL = {
  normal: 0xe8eef7,
  constrained: 0x7fd18b,
  construction: 0xb59f62,
  selected: 0xffb347,
  hover: 0xfff07a,
  preview: 0x4c9aff,
  conflict: 0xff6b6b,
};

const DIMENSIONS = new Set(["distance", "distanceX", "distanceY", "radius", "diameter", "angle"]);
const GLYPH: Record<string, string> = {
  horizontal: "H", vertical: "V", parallel: "∥", perpendicular: "⊥", tangent: "T", equal: "=",
  midpoint: "M", pointOn: "◦", symmetric: "⇹", fix: "⚓", coincident: "",
};

const dist = (a: Vec2, b: Vec2) => Math.hypot(a[0] - b[0], a[1] - b[1]);
const deg = (r: number) => (r * 180) / Math.PI;
const rad = (d: number) => (d * Math.PI) / 180;
const clone = <T>(x: T): T => JSON.parse(JSON.stringify(x)) as T;

export class Sketcher implements InputHandler {
  private sk: SketchData = { id: "", entities: [], constraints: [] };
  private placement!: Placement;
  private status: SketchStatus | null = null;
  private tool: Tool = "select";
  private sel: Sel[] = [];
  private hover: Sel | null = null;
  private pending: Snap[] = [];          // clicks so far for the current drawing tool
  private chainFrom: Snap | null = null; // line tool: start of the next segment
  private cursorSnap: Snap | null = null;
  private dimPick: Sel[] = [];
  private undoStack: SketchData[] = [];
  private redoStack: SketchData[] = [];
  private drag: { sel: Sel; start: Vec2; orig: SketchData } | null = null;
  private solving = false;
  private dragQueued: Vec2 | null = null;
  gridSnap = true;
  private group = new THREE.Group();
  private labels: HTMLDivElement;
  private removeHook: (() => void) | null = null;
  private savedView: { ortho: boolean; pos: THREE.Vector3; up: THREE.Vector3; target: THREE.Vector3 } | null = null;
  active = false;
  onChange: () => void = () => {};

  constructor(private vp: Viewport, private host: SketcherHost) {
    this.labels = h("div", { class: "sketch-labels" });
    vp.canvas.parentElement!.append(this.labels);
  }

  get id(): string { return this.sk.id; }
  get dof(): number | null { return this.status?.dof ?? null; }
  get currentTool(): Tool { return this.tool; }

  start(sketch: SketchData, placement: Placement): void {
    this.sk = clone(sketch);
    this.placement = placement;
    this.active = true;
    this.sel = [];
    this.undoStack = [];
    this.redoStack = [];
    this.setTool("select");
    const c = this.vp.controls;
    this.savedView = { ortho: this.vp.orthographic, pos: this.vp.camera.position.clone(), up: this.vp.camera.up.clone(), target: c.target.clone() };
    this.vp.setOrthographic(true);
    c.target.copy(v3(placement.origin));
    c.enableRotate = false;
    this.vp.lookAlong(v3(placement.normal), v3(placement.yDir));
    this.vp.overlay.add(this.group);
    this.vp.input = this;
    this.vp.filter = "none";
    this.removeHook = this.vp.addFrameHook(() => this.placeLabels());
    this.labels.style.display = "";
    void this.resolve();
    this.redraw();
    if (this.sk.entities.length) this.vp.fit();
  }

  stop(): void {
    this.active = false;
    this.vp.overlay.remove(this.group);
    this.group.clear();
    this.vp.input = null;
    this.vp.filter = "any";
    this.vp.controls.enableRotate = true;
    this.removeHook?.();
    this.removeHook = null;
    clear(this.labels);
    this.labels.style.display = "none";
    if (this.savedView) {
      const s = this.savedView;
      this.vp.controls.target.copy(s.target);
      this.vp.camera.position.copy(s.pos);
      this.vp.camera.up.copy(s.up);
      this.vp.setOrthographic(s.ortho);
      this.vp.controls.update();
    }
  }

  data(): SketchData {
    return clone(this.sk);
  }

  // ------------------------------------------------------------ state

  setTool(t: Tool): void {
    this.tool = t;
    this.cursorSnap = null;
    this.pending = [];
    this.chainFrom = null;
    this.dimPick = [];
    this.vp.canvas.style.cursor = t === "select" ? "default" : "crosshair";
    this.redraw();
    this.onChange();
    const hints: Record<Tool, string> = {
      select: "Click to select, Shift+click to add, drag points to move. Delete removes.",
      line: "Click points to draw lines. Esc or right-click ends the chain.",
      rect: "Click two opposite corners.",
      circle: "Click the centre, then a point on the circle.",
      arc: "Click the centre, the start point, then the end point (counter-clockwise).",
      point: "Click to place points (hole centres, references).",
      dim: "Click a line, circle or arc, or two points / point and line / two lines.",
      dimX: "Click two points (or one, measured from the origin) for a horizontal distance.",
      dimY: "Click two points (or one, measured from the origin) for a vertical distance.",
    };
    this.host.status(hints[t]);
  }

  private checkpoint(): void {
    this.undoStack.push(clone(this.sk));
    if (this.undoStack.length > 100) this.undoStack.shift();
    this.redoStack = [];
  }

  undo(): void {
    const prev = this.undoStack.pop();
    if (!prev) return;
    this.redoStack.push(clone(this.sk));
    this.sk = prev;
    this.sel = [];
    void this.resolve();
  }

  redo(): void {
    const next = this.redoStack.pop();
    if (!next) return;
    this.undoStack.push(clone(this.sk));
    this.sk = next;
    this.sel = [];
    void this.resolve();
  }

  private nextId(prefix: string, list: { id: string }[]): string {
    const used = new Set(list.map((x) => x.id));
    for (let n = 1; ; n++) if (!used.has(prefix + n)) return prefix + n;
  }

  private addEntity(e: Omit<Entity, "id">, prefix: string): string {
    const id = this.nextId(prefix, this.sk.entities);
    this.sk.entities.push({ id, ...e } as Entity);
    return id;
  }

  private addConstraint(type: string, refs: string[], value?: Num): string {
    const id = this.nextId(DIMENSIONS.has(type) ? "d" : "c", this.sk.constraints);
    const c: Constraint = { id, type, refs };
    if (value !== undefined) c.value = value;
    this.sk.constraints.push(c);
    return id;
  }

  /** Solve and adopt the solved geometry. Returns the status. */
  private async resolve(sketch: SketchData = this.sk, adopt = true): Promise<SketchStatus | null> {
    try {
      const st = await this.host.solve(sketch);
      if (adopt && this.active) {
        this.status = st;
        if (st.state === "solved") {
          // Take the solved positions, keep our own constraint list (values may be expressions).
          const solved = new Map(st.sketch.entities.map((e) => [e.id, e]));
          this.sk.entities = this.sk.entities.map((e) => ({ ...e, ...(solved.get(e.id) ?? {}), construction: e.construction }));
          for (const e of this.sk.entities) if (!e.construction) delete e.construction;
        }
        this.redraw();
        this.onChange();
        this.report();
      }
      return st;
    } catch (err) {
      toast(String(err instanceof Error ? err.message : err), "error");
      return null;
    }
  }

  private report(): void {
    const st = this.status;
    if (!st) return;
    let text = "";
    if (st.state === "solved") text = st.dof === 0 ? "Fully constrained" : `${st.dof} degree${st.dof === 1 ? "" : "s"} of freedom left`;
    else if (st.state === "conflicting") text = `Conflicting constraints: ${st.conflicting.join(", ")}`;
    else text = `Sketch ${st.state}: ${st.message}`;
    if (st.redundant.length && st.state === "solved") text += ` · redundant: ${st.redundant.join(", ")}`;
    this.host.status(text);
  }

  // -------------------------------------------------------- geometry

  private entity(id: string): Entity | undefined {
    return this.sk.entities.find((e) => e.id === id);
  }

  pointPos(ref: string): Vec2 | null {
    if (ref === "origin") return [0, 0];
    const [id, part] = ref.split(".");
    const e = this.entity(id);
    if (!e) return null;
    if (e.type === "point") return e.p ?? null;
    if (e.type === "line") return part === "start" ? e.a! : part === "end" ? e.b! : null;
    if (part === "center") return e.center!;
    if (e.type === "arc" && (part === "start" || part === "end")) {
      const a = rad(part === "start" ? e.start! : e.end!);
      return [e.center![0] + e.radius! * Math.cos(a), e.center![1] + e.radius! * Math.sin(a)];
    }
    return null;
  }

  private setPoint(sk: SketchData, ref: string, p: Vec2): void {
    const [id, part] = ref.split(".");
    const e = sk.entities.find((x) => x.id === id);
    if (!e) return;
    if (e.type === "point") e.p = p;
    else if (e.type === "line") { if (part === "start") e.a = p; else e.b = p; }
    else if (part === "center") e.center = p;
    else if (e.type === "arc") {
      const a = deg(Math.atan2(p[1] - e.center![1], p[0] - e.center![0]));
      if (part === "start") e.start = a; else e.end = a;
      e.radius = dist(p, e.center!);
    }
  }

  private pointRefs(): string[] {
    const out: string[] = [];
    for (const e of this.sk.entities) {
      if (e.type === "point") out.push(e.id);
      else if (e.type === "line") out.push(`${e.id}.start`, `${e.id}.end`);
      else if (e.type === "circle") out.push(`${e.id}.center`);
      else out.push(`${e.id}.center`, `${e.id}.start`, `${e.id}.end`);
    }
    return out;
  }

  private curveDistance(e: Entity, p: Vec2): { d: number; proj: Vec2 } {
    if (e.type === "line") {
      const [ax, ay] = e.a!, [bx, by] = e.b!;
      const dx = bx - ax, dy = by - ay;
      const len2 = dx * dx + dy * dy || 1;
      const t = Math.max(0, Math.min(1, ((p[0] - ax) * dx + (p[1] - ay) * dy) / len2));
      const proj: Vec2 = [ax + t * dx, ay + t * dy];
      return { d: dist(p, proj), proj };
    }
    if (e.type === "circle" || e.type === "arc") {
      const c = e.center!, r = e.radius!;
      const ang = Math.atan2(p[1] - c[1], p[0] - c[0]);
      const proj: Vec2 = [c[0] + r * Math.cos(ang), c[1] + r * Math.sin(ang)];
      if (e.type === "arc") {
        let a = deg(ang), a0 = e.start!, a1 = e.end!;
        while (a1 <= a0) a1 += 360;
        while (a < a0) a += 360;
        if (a > a1) {
          const s = this.pointPos(`${e.id}.start`)!, en = this.pointPos(`${e.id}.end`)!;
          const ds = dist(p, s), de = dist(p, en);
          return ds < de ? { d: ds, proj: s } : { d: de, proj: en };
        }
      }
      return { d: Math.abs(dist(p, c) - r), proj };
    }
    return { d: Infinity, proj: p };
  }

  private px(): number {
    return this.vp.pixelSize(v3(this.placement.origin));
  }

  private gridStep(): number {
    const raw = this.px() * 12;
    const p = Math.pow(10, Math.floor(Math.log10(raw)));
    for (const m of [1, 2, 5, 10]) if (p * m >= raw) return p * m;
    return p * 10;
  }

  /** What is under the cursor: a point, then a curve. */
  private hitTest(p: Vec2): Sel | null {
    const px = this.px();
    let best: Sel | null = null, bestD = 9 * px;
    for (const ref of [...this.pointRefs(), "origin"]) {
      const q = this.pointPos(ref);
      if (q && dist(p, q) < bestD) { bestD = dist(p, q); best = { kind: "point", ref }; }
    }
    if (best) return best;
    bestD = 6 * px;
    for (const e of this.sk.entities) {
      if (e.type === "point") continue;
      const { d } = this.curveDistance(e, p);
      if (d < bestD) { bestD = d; best = { kind: "entity", id: e.id }; }
    }
    return best;
  }

  private snap(p: Vec2, exclude: string[] = []): Snap {
    const hit = this.hitTest(p);
    if (hit?.kind === "point" && !exclude.some((x) => hit.ref.startsWith(x))) return { pos: this.pointPos(hit.ref)!, ref: hit.ref };
    if (hit?.kind === "entity" && !exclude.includes(hit.id)) {
      const e = this.entity(hit.id)!;
      return { pos: this.curveDistance(e, p).proj, onCurve: hit.id };
    }
    if (this.gridSnap) {
      const g = this.gridStep();
      return { pos: [Math.round(p[0] / g) * g, Math.round(p[1] / g) * g] };
    }
    return { pos: p };
  }

  // Horizontal/vertical inference for a new segment.
  private inferHV(from: Vec2, to: Snap): { pos: Vec2; hv?: "horizontal" | "vertical" } {
    if (to.ref) return { pos: to.pos };
    const dx = to.pos[0] - from[0], dy = to.pos[1] - from[1];
    const a = Math.abs(deg(Math.atan2(dy, dx)));
    if (Math.hypot(dx, dy) < 1e-9) return { pos: to.pos };
    if (a < 4 || a > 176) return { pos: [to.pos[0], from[1]], hv: "horizontal" };
    if (Math.abs(a - 90) < 4) return { pos: [from[0], to.pos[1]], hv: "vertical" };
    return { pos: to.pos };
  }

  private attach(pointRef: string, s: Snap): void {
    if (s.ref) this.addConstraint("coincident", [pointRef, s.ref]);
    else if (s.onCurve) this.addConstraint("pointOn", [pointRef, s.onCurve]);
  }

  // ------------------------------------------------------------ input

  down(e: PointerEvent): boolean {
    const p = this.vp.planeHit(e, this.placement);
    if (!p) return true;
    if (this.tool === "select") {
      const hit = this.hitTest(p);
      if (hit && !(hit.kind === "point" && hit.ref === "origin") && (hit.kind === "point" || hit.kind === "entity")) {
        this.drag = { sel: hit, start: p, orig: clone(this.sk) };
      }
    }
    return true;
  }

  move(e: PointerEvent): boolean {
    const p = this.vp.planeHit(e, this.placement);
    if (!p) return true;
    if (this.drag && e.buttons === 1) {
      this.dragTo(p);
      return true;
    }
    if (e.buttons !== 0) return false; // let the camera pan/zoom
    if (this.tool === "select" || this.tool === "dim" || this.tool === "dimX" || this.tool === "dimY") {
      const hit = this.hitTest(p);
      if (JSON.stringify(hit) !== JSON.stringify(this.hover)) {
        this.hover = hit;
        this.redraw();
      }
    } else {
      this.cursorSnap = this.snap(p);
      this.redraw();
    }
    return true;
  }

  up(e: PointerEvent): boolean {
    const p = this.vp.planeHit(e, this.placement);
    if (e.button === 2) {
      // Right click ends a line chain / drawing; otherwise it pans.
      if (this.pending.length || this.chainFrom) { this.pending = []; this.chainFrom = null; this.redraw(); return true; }
      return false;
    }
    if (e.button !== 0 || !p) return false;
    if (this.drag) {
      const moved = dist(p, this.drag.start) > this.px() * 3;
      const orig = this.drag.orig;
      const sel = this.drag.sel;
      this.drag = null;
      if (moved) {
        this.undoStack.push(orig);
        this.redoStack = [];
        return true;
      }
      this.clickSelect(sel, e.shiftKey || e.ctrlKey);
      return true;
    }
    switch (this.tool) {
      case "select": this.clickSelect(this.hitTest(p), e.shiftKey || e.ctrlKey); break;
      case "line": this.clickLine(p); break;
      case "rect": this.clickRect(p); break;
      case "circle": this.clickCircle(p); break;
      case "arc": this.clickArc(p); break;
      case "point": this.clickPoint(p); break;
      case "dim": case "dimX": case "dimY": void this.clickDim(p, e); break;
    }
    return true;
  }

  dblclick(e: MouseEvent): boolean {
    const p = this.vp.planeHit(e, this.placement);
    if (!p || this.tool !== "select") return true;
    return true;
  }

  private clickSelect(hit: Sel | null, add: boolean): void {
    if (!hit || (hit.kind === "point" && hit.ref === "origin" && !add)) {
      if (!add) this.sel = [];
    } else {
      const key = JSON.stringify(hit);
      const i = this.sel.findIndex((s) => JSON.stringify(s) === key);
      if (add) { if (i >= 0) this.sel.splice(i, 1); else this.sel.push(hit); }
      else this.sel = [hit];
    }
    this.redraw();
    this.onChange();
  }

  selectAll(): void {
    this.sel = this.sk.entities.map((e) => ({ kind: "entity", id: e.id }) as Sel);
    this.redraw();
    this.onChange();
  }

  private dragTo(p: Vec2): void {
    if (!this.drag) return;
    if (this.solving) { this.dragQueued = p; return; }
    const sk = clone(this.drag.orig);
    const temp: Constraint[] = [];
    const d: Vec2 = [p[0] - this.drag.start[0], p[1] - this.drag.start[1]];
    const sel = this.drag.sel;
    if (sel.kind === "point") {
      this.setPoint(sk, sel.ref, p);
      temp.push({ id: "_drag", type: "fix", refs: [sel.ref] });
    } else if (sel.kind === "entity") {
      const e = sk.entities.find((x) => x.id === sel.id)!;
      if (e.type === "line") {
        e.a = [e.a![0] + d[0], e.a![1] + d[1]];
        e.b = [e.b![0] + d[0], e.b![1] + d[1]];
        temp.push({ id: "_drag", type: "fix", refs: [`${e.id}.start`] }, { id: "_drag2", type: "fix", refs: [`${e.id}.end`] });
      } else if (e.type === "circle" || e.type === "arc") {
        e.radius = Math.max(1e-3, dist(p, e.center!));
        temp.push({ id: "_drag", type: "radius", refs: [e.id], value: e.radius });
      } else return;
    }
    sk.constraints.push(...temp);
    this.solving = true;
    void this.host.solve(sk).then((st) => {
      this.solving = false;
      if (st.state === "solved" && this.drag) {
        const solved = new Map(st.sketch.entities.map((x) => [x.id, x]));
        this.sk.entities = this.sk.entities.map((x) => ({ ...x, ...(solved.get(x.id) ?? {}), construction: x.construction }));
        for (const x of this.sk.entities) if (!x.construction) delete x.construction;
        this.redraw();
      }
      const q = this.dragQueued;
      this.dragQueued = null;
      if (q && this.drag) this.dragTo(q);
    }, () => { this.solving = false; });
  }

  private clickLine(p: Vec2): void {
    const s = this.snap(p);
    if (!this.chainFrom) { this.chainFrom = s; this.redraw(); return; }
    const from = this.chainFrom;
    const { pos, hv } = this.inferHV(from.pos, s);
    if (dist(from.pos, pos) < this.px() * 2) return;
    this.checkpoint();
    const id = this.addEntity({ type: "line", a: from.pos, b: pos }, "L");
    this.attach(`${id}.start`, from);
    this.attach(`${id}.end`, { ...s, pos });
    if (hv) this.addConstraint(hv, [id]);
    // Closing onto an existing point ends the chain.
    this.chainFrom = s.ref ? null : { pos, ref: `${id}.end` };
    void this.resolve();
  }

  private clickRect(p: Vec2): void {
    const s = this.snap(p);
    if (!this.pending.length) { this.pending = [s]; this.redraw(); return; }
    const a = this.pending[0], b = s;
    this.pending = [];
    if (Math.abs(a.pos[0] - b.pos[0]) < 1e-6 || Math.abs(a.pos[1] - b.pos[1]) < 1e-6) return;
    this.checkpoint();
    const [x0, y0] = a.pos, [x1, y1] = b.pos;
    const c: Vec2[] = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]];
    const ids = [0, 1, 2, 3].map((i) => this.addEntity({ type: "line", a: c[i], b: c[(i + 1) % 4] }, "L"));
    for (let i = 0; i < 4; i++) this.addConstraint("coincident", [`${ids[i]}.end`, `${ids[(i + 1) % 4]}.start`]);
    this.addConstraint("horizontal", [ids[0]]);
    this.addConstraint("horizontal", [ids[2]]);
    this.addConstraint("vertical", [ids[1]]);
    this.addConstraint("vertical", [ids[3]]);
    this.attach(`${ids[0]}.start`, a);
    this.attach(`${ids[2]}.start`, b);
    void this.resolve();
  }

  private clickCircle(p: Vec2): void {
    const s = this.snap(p);
    if (!this.pending.length) { this.pending = [s]; this.redraw(); return; }
    const c = this.pending[0];
    this.pending = [];
    const r = dist(c.pos, s.pos);
    if (r < this.px() * 2) return;
    this.checkpoint();
    const id = this.addEntity({ type: "circle", center: c.pos, radius: r }, "C");
    this.attach(`${id}.center`, c);
    void this.resolve();
  }

  private clickArc(p: Vec2): void {
    const s = this.snap(p);
    this.pending.push(s);
    if (this.pending.length < 3) { this.redraw(); return; }
    const [c, a, b] = this.pending;
    this.pending = [];
    const r = dist(c.pos, a.pos);
    if (r < this.px() * 2) return;
    this.checkpoint();
    const start = deg(Math.atan2(a.pos[1] - c.pos[1], a.pos[0] - c.pos[0]));
    const end = deg(Math.atan2(b.pos[1] - c.pos[1], b.pos[0] - c.pos[0]));
    const id = this.addEntity({ type: "arc", center: c.pos, radius: r, start, end }, "A");
    this.attach(`${id}.center`, c);
    this.attach(`${id}.start`, a);
    if (b.ref) this.attach(`${id}.end`, b);
    void this.resolve();
  }

  private clickPoint(p: Vec2): void {
    const s = this.snap(p);
    if (s.ref) return; // already a point there
    this.checkpoint();
    const id = this.addEntity({ type: "point", p: s.pos }, "P");
    this.attach(id, s);
    void this.resolve();
  }

  // ------------------------------------------------------- dimensions

  private measure(type: string, refs: string[]): number {
    const pt = (r: string) => this.pointPos(r)!;
    if (type === "distance" && refs.length === 1) {
      const e = this.entity(refs[0])!;
      return dist(e.a!, e.b!);
    }
    if (type === "distance") {
      const e2 = this.entity(refs[1]);
      if (e2?.type === "line" && !refs[1].includes(".")) return this.curveDistanceInfinite(e2, pt(refs[0]));
      return dist(pt(refs[0]), pt(refs[1]));
    }
    if (type === "distanceX" || type === "distanceY") {
      const i = type === "distanceX" ? 0 : 1;
      return refs.length === 1 ? pt(refs[0])[i] : pt(refs[1])[i] - pt(refs[0])[i];
    }
    if (type === "radius") return this.entity(refs[0])!.radius!;
    if (type === "diameter") return this.entity(refs[0])!.radius! * 2;
    if (type === "angle") {
      const l1 = this.entity(refs[0])!, l2 = this.entity(refs[1])!;
      const a1 = Math.atan2(l1.b![1] - l1.a![1], l1.b![0] - l1.a![0]);
      const a2 = Math.atan2(l2.b![1] - l2.a![1], l2.b![0] - l2.a![0]);
      let a = deg(a2 - a1);
      while (a < 0) a += 360;
      while (a >= 360) a -= 360;
      return a;
    }
    return 0;
  }

  private curveDistanceInfinite(line: Entity, p: Vec2): number {
    const [ax, ay] = line.a!, [bx, by] = line.b!;
    const len = Math.hypot(bx - ax, by - ay) || 1;
    return Math.abs((bx - ax) * (ay - p[1]) - (ax - p[0]) * (by - ay)) / len;
  }

  private async clickDim(p: Vec2, e: PointerEvent): Promise<void> {
    const hit = this.hitTest(p);
    const picks = this.dimPick;
    const kindOf = (s: Sel) => (s.kind === "entity" ? this.entity(s.id)?.type : "point");
    if (this.tool === "dimX" || this.tool === "dimY") {
      const type = this.tool === "dimX" ? "distanceX" : "distanceY";
      if (hit?.kind === "point") {
        picks.push(hit);
        if (picks.length === 2) {
          this.dimPick = [];
          const refs = picks.map((s) => (s as { ref: string }).ref);
          await this.promptDimension(type, refs, e);
        }
      } else if (!hit && picks.length === 1) {
        this.dimPick = [];
        const ref = (picks[0] as { ref: string }).ref;
        if (ref !== "origin") await this.promptDimension(type, [ref], e);
      }
      this.redraw();
      return;
    }
    if (!hit) {
      // Empty click: a lone line gets its length.
      if (picks.length === 1 && kindOf(picks[0]) === "line") {
        this.dimPick = [];
        await this.promptDimension("distance", [(picks[0] as { id: string }).id], e);
      } else this.dimPick = [];
      this.redraw();
      return;
    }
    if (hit.kind === "entity") {
      const t = kindOf(hit);
      if ((t === "circle" || t === "arc") && picks.length === 0) {
        await this.promptDimension(t === "circle" ? "diameter" : "radius", [hit.id], e);
        return;
      }
    }
    picks.push(hit);
    if (picks.length === 2) {
      this.dimPick = [];
      const [a, b] = picks;
      const ka = kindOf(a), kb = kindOf(b);
      const ref = (s: Sel) => (s.kind === "point" ? s.ref : (s as { id: string }).id);
      if (ka === "point" && kb === "point") await this.promptDimension("distance", [ref(a), ref(b)], e);
      else if (ka === "point" && kb === "line") await this.promptDimension("distance", [ref(a), ref(b)], e);
      else if (ka === "line" && kb === "point") await this.promptDimension("distance", [ref(b), ref(a)], e);
      else if (ka === "line" && kb === "line") await this.promptDimension("angle", [ref(a), ref(b)], e);
      else toast("No dimension between those: pick a line, circle, arc, or two points.", "info");
    }
    this.redraw();
  }

  private promptValue(initial: string, e: { clientX: number; clientY: number }): Promise<string | null> {
    return new Promise((resolve) => {
      const r = this.vp.canvas.getBoundingClientRect();
      const input = h("input", { class: "dim-input", type: "text", value: initial, title: "A number or an expression (parameters allowed)" });
      Object.assign(input.style, { left: `${e.clientX - r.left + 8}px`, top: `${e.clientY - r.top - 12}px` });
      let done = false;
      const finish = (v: string | null) => {
        if (done) return;
        done = true;
        input.remove();
        resolve(v);
      };
      input.addEventListener("keydown", (k) => {
        k.stopPropagation();
        if (k.key === "Enter") finish(input.value.trim());
        if (k.key === "Escape") finish(null);
      });
      input.addEventListener("blur", () => finish(input.value.trim()));
      this.labels.parentElement!.append(input);
      input.focus();
      input.select();
    });
  }

  private async promptDimension(type: string, refs: string[], e: { clientX: number; clientY: number }): Promise<void> {
    const current = this.measure(type, refs);
    const text = await this.promptValue(fmt(current, 3), e);
    if (text === null || text === "") return;
    const value: Num = /^-?\d+(\.\d+)?$/.test(text) ? Number(text) : text;
    if (typeof value === "number" && value <= 0 && type !== "distanceX" && type !== "distanceY") {
      toast("A dimension must be positive.", "error");
      return;
    }
    this.checkpoint();
    const id = this.addConstraint(type, refs, value);
    const st = await this.resolve();
    if (st && st.state === "conflicting" && st.conflicting.includes(id)) {
      this.sk = this.undoStack.pop()!;
      toast(`That dimension conflicts with ${st.conflicting.filter((c) => c !== id).join(", ") || "existing constraints"}.`, "error");
      await this.resolve();
    }
  }

  private async editDimension(c: Constraint, e: MouseEvent): Promise<void> {
    const text = await this.promptValue(String(c.value ?? ""), e);
    if (text === null || text === "" || text === String(c.value)) return;
    this.checkpoint();
    c.value = /^-?\d+(\.\d+)?$/.test(text) ? Number(text) : text;
    const st = await this.resolve();
    if (st && st.state !== "solved") {
      this.sk = this.undoStack.pop()!;
      toast("That value cannot be solved; it was not applied.", "error");
      await this.resolve();
    }
  }

  // ------------------------------------------------------ constraints

  /** The constraints that can be applied to the current selection. */
  applicable(): string[] {
    const pts = this.sel.filter((s) => s.kind === "point").length;
    const ents = this.sel.filter((s) => s.kind === "entity").map((s) => this.entity((s as { id: string }).id)!);
    const lines = ents.filter((e) => e.type === "line").length;
    const curves = ents.filter((e) => e.type === "circle" || e.type === "arc").length;
    const pointEnts = ents.filter((e) => e.type === "point").length;
    const P = pts + pointEnts;
    const out: string[] = [];
    if (P === 2 && ents.length === pointEnts) out.push("coincident");
    if ((lines >= 1 && ents.length === lines && P === 0) || (P === 2 && lines === 0 && curves === 0)) out.push("horizontal", "vertical");
    if (lines === 2 && ents.length === 2 && P === 0) out.push("parallel", "perpendicular");
    if ((lines >= 2 && ents.length === lines) || (curves >= 2 && ents.length === curves)) out.push("equal");
    if (ents.length === 2 && P === 0 && curves >= 1) out.push("tangent");
    if (P === 1 && lines === 1 && ents.length - pointEnts === 1) out.push("midpoint");
    if (P === 1 && ents.length - pointEnts === 1) out.push("pointOn");
    if (P === 2 && lines === 1 && ents.length - pointEnts === 1) out.push("symmetric");
    if (this.sel.length && this.sel.every((s) => s.kind !== "constraint")) out.push("fix");
    return out;
  }

  applyConstraint(type: string): void {
    if (!this.applicable().includes(type)) {
      toast(`Select suitable geometry for ${type} first.`, "info");
      return;
    }
    const pts = this.sel.flatMap((s) => (s.kind === "point" ? [s.ref] : s.kind === "entity" && this.entity(s.id)?.type === "point" ? [s.id] : []));
    const ents = this.sel.flatMap((s) => (s.kind === "entity" && this.entity(s.id)?.type !== "point" ? [s.id] : []));
    const lines = ents.filter((id) => this.entity(id)!.type === "line");
    this.checkpoint();
    switch (type) {
      case "coincident": this.addConstraint(type, pts); break;
      case "horizontal": case "vertical":
        if (lines.length) for (const l of lines) this.addConstraint(type, [l]);
        else this.addConstraint(type, pts);
        break;
      case "parallel": case "perpendicular": this.addConstraint(type, lines); break;
      case "equal": for (let i = 1; i < ents.length; i++) this.addConstraint(type, [ents[0], ents[i]]); break;
      case "tangent": {
        // Line first, if there is one.
        const sorted = [...ents].sort((a, b) => (this.entity(a)!.type === "line" ? -1 : 0) - (this.entity(b)!.type === "line" ? -1 : 0));
        this.addConstraint(type, sorted);
        break;
      }
      case "midpoint": case "pointOn": this.addConstraint(type, [pts[0], ents[0]]); break;
      case "symmetric": this.addConstraint(type, [pts[0], pts[1], ents[0]]); break;
      case "fix": for (const r of [...pts, ...ents]) this.addConstraint(type, [r]); break;
    }
    this.sel = [];
    void this.resolve().then((st) => {
      if (st?.state === "conflicting") toast(`Conflicting constraints: ${st.conflicting.join(", ")}. Undo (Ctrl+Z) or delete one.`, "error", 5000);
    });
  }

  toggleConstruction(): void {
    const ids = this.sel.flatMap((s) => (s.kind === "entity" ? [s.id] : []));
    if (!ids.length) { toast("Select lines, arcs or circles first.", "info"); return; }
    this.checkpoint();
    for (const id of ids) {
      const e = this.entity(id)!;
      if (e.construction) delete e.construction; else e.construction = true;
    }
    this.redraw();
    this.onChange();
  }

  deleteSelection(): void {
    if (!this.sel.length) return;
    this.checkpoint();
    const ents = new Set(this.sel.flatMap((s) => (s.kind === "entity" ? [s.id] : [])));
    for (const s of this.sel) if (s.kind === "point" && !s.ref.includes(".") && s.ref !== "origin") ents.add(s.ref);
    const cons = new Set(this.sel.flatMap((s) => (s.kind === "constraint" ? [s.id] : [])));
    this.sk.entities = this.sk.entities.filter((e) => !ents.has(e.id));
    this.sk.constraints = this.sk.constraints.filter(
      (c) => !cons.has(c.id) && !c.refs.some((r) => ents.has(r.split(".")[0])));
    this.sel = [];
    void this.resolve();
  }

  escape(): void {
    if (this.pending.length || this.chainFrom || this.dimPick.length) {
      this.pending = [];
      this.chainFrom = null;
      this.dimPick = [];
      this.redraw();
    } else if (this.tool !== "select") this.setTool("select");
    else { this.sel = []; this.redraw(); this.onChange(); }
  }

  // ---------------------------------------------------------- drawing

  private isSel(s: Sel): boolean {
    const k = JSON.stringify(s);
    return this.sel.some((x) => JSON.stringify(x) === k) || this.dimPick.some((x) => JSON.stringify(x) === k);
  }

  private redraw(): void {
    if (!this.active) return;
    this.group.clear();
    const P = this.placement;
    const w = (x: number, y: number) => planeToWorld(P, x, y);
    const full = this.status?.state === "solved" && this.status.dof === 0;
    const conflict = this.status?.state === "conflicting";

    // Grid
    const g = this.gridStep() * 5;
    const ext = Math.max(200, g * 30);
    const gridPts: THREE.Vector3[] = [];
    for (let i = -30; i <= 30; i++) {
      gridPts.push(w(i * g, -30 * g), w(i * g, 30 * g), w(-30 * g, i * g), w(30 * g, i * g));
    }
    const grid = new THREE.LineSegments(new THREE.BufferGeometry().setFromPoints(gridPts),
      new THREE.LineBasicMaterial({ color: 0x3a4250, transparent: true, opacity: 0.6, depthTest: false }));
    grid.renderOrder = -1;
    this.group.add(grid);
    const axes = new THREE.LineSegments(new THREE.BufferGeometry().setFromPoints([w(-ext, 0), w(ext, 0), w(0, -ext), w(0, ext)]),
      new THREE.LineBasicMaterial({ color: 0x5a6578, depthTest: false }));
    this.group.add(axes);

    for (const e of this.sk.entities) {
      const sel = this.isSel({ kind: "entity", id: e.id });
      const hov = this.hover?.kind === "entity" && this.hover.id === e.id;
      const colour = sel ? COL.selected : hov ? COL.hover : e.construction ? COL.construction : conflict ? COL.conflict : full ? COL.constrained : COL.normal;
      if (e.type === "point") continue;
      const pts = entityPolyline(e).map(([x, y]) => w(x, y));
      const mat = e.construction
        ? new THREE.LineDashedMaterial({ color: colour, dashSize: this.px() * 8, gapSize: this.px() * 5, depthTest: false })
        : new THREE.LineBasicMaterial({ color: colour, depthTest: false });
      const line = new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), mat);
      line.computeLineDistances();
      line.renderOrder = 2;
      this.group.add(line);
    }

    // Points
    const ptPos: THREE.Vector3[] = [];
    const ptCol: number[] = [];
    for (const ref of [...this.pointRefs(), "origin"]) {
      const q = this.pointPos(ref);
      if (!q) continue;
      const sel = this.isSel({ kind: "point", ref }) || (!ref.includes(".") && this.isSel({ kind: "entity", id: ref }));
      const hov = this.hover?.kind === "point" && this.hover.ref === ref;
      const c = new THREE.Color(sel ? COL.selected : hov ? COL.hover : ref === "origin" ? 0xff8080 : full ? COL.constrained : COL.normal);
      ptPos.push(w(q[0], q[1]));
      ptCol.push(c.r, c.g, c.b);
    }
    const pg = new THREE.BufferGeometry().setFromPoints(ptPos);
    pg.setAttribute("color", new THREE.Float32BufferAttribute(ptCol, 3));
    const points = new THREE.Points(pg, new THREE.PointsMaterial({ size: 7, sizeAttenuation: false, vertexColors: true, depthTest: false }));
    points.renderOrder = 3;
    this.group.add(points);

    this.drawPreview(w);
    this.buildLabels();
  }

  private drawPreview(w: (x: number, y: number) => THREE.Vector3): void {
    const cur = this.cursorSnap;
    if (!cur || !["line", "rect", "circle", "arc", "point"].includes(this.tool)) return;
    const mat = new THREE.LineBasicMaterial({ color: COL.preview, depthTest: false });
    const add = (pts: Vec2[]) => {
      const l = new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts.map(([x, y]) => w(x, y))), mat);
      l.renderOrder = 4;
      this.group.add(l);
    };
    if (this.tool === "line" && this.chainFrom) add([this.chainFrom.pos, this.inferHV(this.chainFrom.pos, cur).pos]);
    if (this.tool === "rect" && this.pending.length) {
      const [x0, y0] = this.pending[0].pos, [x1, y1] = cur.pos;
      add([[x0, y0], [x1, y0], [x1, y1], [x0, y1], [x0, y0]]);
    }
    if (this.tool === "circle" && this.pending.length) {
      const c = this.pending[0].pos;
      add(entityPolyline({ id: "", type: "circle", center: c, radius: dist(c, cur.pos) }));
    }
    if (this.tool === "arc" && this.pending.length === 1) add([this.pending[0].pos, cur.pos]);
    if (this.tool === "arc" && this.pending.length === 2) {
      const [c, a] = this.pending.map((s) => s.pos);
      add(entityPolyline({ id: "", type: "arc", center: c, radius: dist(c, a),
        start: deg(Math.atan2(a[1] - c[1], a[0] - c[0])), end: deg(Math.atan2(cur.pos[1] - c[1], cur.pos[0] - c[0])) }));
    }
    // Snap marker
    const marker = new THREE.Points(new THREE.BufferGeometry().setFromPoints([w(cur.pos[0], cur.pos[1])]),
      new THREE.PointsMaterial({ size: cur.ref ? 11 : 6, sizeAttenuation: false, color: cur.ref ? 0xfff07a : cur.onCurve ? 0x7fd18b : COL.preview, depthTest: false }));
    marker.renderOrder = 5;
    this.group.add(marker);
  }

  // Labels are DOM elements placed over the canvas every frame.
  private labelEls: { el: HTMLElement; at: () => Vec2 | null }[] = [];

  private buildLabels(): void {
    clear(this.labels);
    this.labelEls = [];
    const conflicting = new Set(this.status?.conflicting ?? []);
    const glyphCount = new Map<string, number>();
    for (const c of this.sk.constraints) {
      const sel = this.isSel({ kind: "constraint", id: c.id });
      const isDim = DIMENSIONS.has(c.type);
      let text = isDim ? this.dimText(c) : GLYPH[c.type] ?? c.type;
      if (!text) continue;
      const at = this.labelAnchor(c);
      if (!at) continue;
      // Spread glyphs that share an anchor.
      const key = c.refs[0];
      const n = glyphCount.get(key) ?? 0;
      if (!isDim) glyphCount.set(key, n + 1);
      if (isDim && typeof c.value === "string") text = `${c.value} = ${text}`;
      const el = h("div", {
        class: `sk-label ${isDim ? "dim" : "glyph"}${sel ? " selected" : ""}${conflicting.has(c.id) ? " conflict" : ""}`,
        title: `${c.id}: ${c.type}${isDim ? " (double-click to edit)" : ""}`,
        onmousedown: (e: MouseEvent) => e.stopPropagation(),
        onclick: (e: MouseEvent) => { e.stopPropagation(); this.clickSelect({ kind: "constraint", id: c.id }, e.shiftKey || e.ctrlKey); },
        ondblclick: (e: MouseEvent) => { e.stopPropagation(); if (isDim) void this.editDimension(c, e); },
      }, text);
      if (!isDim) el.dataset.offset = String(n);
      this.labels.append(el);
      this.labelEls.push({ el, at });
    }
    this.placeLabels();
  }

  private dimText(c: Constraint): string {
    const v = this.status?.state === "solved" ? this.measure(c.type, c.refs) : typeof c.value === "number" ? c.value : NaN;
    const s = Number.isFinite(v) ? fmt(Math.abs(v), 2) : String(c.value);
    if (c.type === "radius") return `R${s}`;
    if (c.type === "diameter") return `⌀${s}`;
    if (c.type === "angle") return `${s}°`;
    return s;
  }

  private labelAnchor(c: Constraint): (() => Vec2 | null) | null {
    const mid = (a: Vec2, b: Vec2): Vec2 => [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];
    return () => {
      const r0 = c.refs[0];
      const e0 = r0 && !r0.includes(".") ? this.entity(r0) : undefined;
      if (c.type === "radius" || c.type === "diameter") {
        const e = this.entity(r0);
        if (!e?.center) return null;
        const a = e.type === "arc" ? rad((e.start! + e.end! + (e.end! < e.start! ? 360 : 0)) / 2) : rad(45);
        return [e.center[0] + e.radius! * Math.cos(a), e.center[1] + e.radius! * Math.sin(a)];
      }
      if (c.type === "angle") {
        const l = this.entity(c.refs[1]);
        return l?.a && l.b ? mid(l.a, l.b) : null;
      }
      if (e0?.type === "line" && c.refs.length === 1) return mid(e0.a!, e0.b!);
      if (e0?.type === "line" && c.refs.length === 2 && !c.refs[1].includes(".") && this.entity(c.refs[1])?.type === "line") {
        return mid(e0.a!, e0.b!);
      }
      const pts = c.refs.map((r) => this.pointPos(r)).filter((p): p is Vec2 => !!p);
      if (pts.length >= 2) return mid(pts[0], pts[1]);
      if (pts.length === 1) {
        if ((c.type === "distanceX" || c.type === "distanceY") && c.refs.length === 1) return mid([0, 0], pts[0]);
        return pts[0];
      }
      if (e0?.center) return e0.center;
      return null;
    };
  }

  private placeLabels(): void {
    for (const { el, at } of this.labelEls) {
      const p = at();
      if (!p) { el.style.display = "none"; continue; }
      const s = this.vp.project(planeToWorld(this.placement, p[0], p[1]));
      const off = Number(el.dataset.offset ?? -1);
      const dx = off >= 0 ? 10 + off * 16 : 0, dy = off >= 0 ? -16 : 0;
      el.style.display = "";
      el.style.transform = `translate(${s.x + dx}px, ${s.y + dy}px) translate(-50%, -50%)`;
    }
  }

  hasSelection(): boolean {
    return this.sel.length > 0;
  }
}
