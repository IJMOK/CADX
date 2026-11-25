import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
import type { Datum, MeshData, Pick, Placement, SketchData, Vec2, Vec3 } from "./types";

const BODY = new THREE.Color(0x9aa4b2);
const HOVER = new THREE.Color(0xffb347);
const SELECTED = new THREE.Color(0x4c9aff);
const FEATURE = new THREE.Color(0x7fd18b);
const EDGE = new THREE.Color(0x15181c);
const SKETCH_COLOUR = 0x5fc3ff;
const SKETCH_CONSTRUCTION = 0x8a7f5c;

export type PickFilter = "face" | "edge" | "any" | "none";
export type ViewName = "iso" | "top" | "bottom" | "front" | "back" | "right" | "left";
export type DisplayMode = "shaded-edges" | "shaded" | "wireframe";

/** A tool (the sketcher) can take over pointer input. Return true to consume. */
export interface InputHandler {
  down?(e: PointerEvent): boolean;
  move?(e: PointerEvent): boolean;
  up?(e: PointerEvent): boolean;
  dblclick?(e: MouseEvent): boolean;
}

export const v3 = (v: Vec3) => new THREE.Vector3(v[0], v[1], v[2]);

export function planeToWorld(p: Placement, x: number, y: number): THREE.Vector3 {
  return v3(p.origin).addScaledVector(v3(p.xDir), x).addScaledVector(v3(p.yDir), y);
}

/** Polyline points (local 2D) for an entity. */
export function entityPolyline(e: SketchData["entities"][number]): Vec2[] {
  if (e.type === "line" && e.a && e.b) return [e.a, e.b];
  if ((e.type === "circle" || e.type === "arc") && e.center && e.radius !== undefined) {
    let a0 = 0, a1 = 360;
    if (e.type === "arc") {
      a0 = e.start ?? 0;
      a1 = e.end ?? 90;
      while (a1 <= a0) a1 += 360;
    }
    const n = Math.max(12, Math.ceil((a1 - a0) / 4));
    const pts: Vec2[] = [];
    for (let i = 0; i <= n; i++) {
      const t = ((a0 + ((a1 - a0) * i) / n) * Math.PI) / 180;
      pts.push([e.center[0] + e.radius * Math.cos(t), e.center[1] + e.radius * Math.sin(t)]);
    }
    return pts;
  }
  return [];
}

export class Viewport {
  readonly renderer: THREE.WebGLRenderer;
  readonly scene = new THREE.Scene();
  private persp: THREE.PerspectiveCamera;
  private ortho: THREE.OrthographicCamera;
  camera: THREE.PerspectiveCamera | THREE.OrthographicCamera;
  readonly controls: OrbitControls;
  private raycaster = new THREE.Raycaster();
  private pointer = new THREE.Vector2();

  private body = new THREE.Group();
  private mesh: THREE.Mesh | null = null;
  private edgeLines: THREE.LineSegments | null = null;
  private faceIds: number[] = [];
  private edgeIds: number[] = [];
  private faceNames: string[] = [];
  private edgeNames: string[] = [];
  private sketchGroup = new THREE.Group();
  private datumGroup = new THREE.Group();
  private basePlanes = new THREE.Group();
  /** Free space for tools (the sketcher) to draw in. */
  readonly overlay = new THREE.Group();

  private hover: Pick | null = null;
  selected: Pick[] = [];
  private featureHighlight = "";
  filter: PickFilter = "any";
  display: DisplayMode = "shaded-edges";
  input: InputHandler | null = null;
  private frameHooks: (() => void)[] = [];
  private downAt: { x: number; y: number } | null = null;

  onHover: (p: Pick | null) => void = () => {};
  onClick: (p: Pick | null, e: PointerEvent) => void = () => {};
  onContext: (p: Pick | null, e: MouseEvent) => void = () => {};

  constructor(readonly canvas: HTMLCanvasElement) {
    this.renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
    this.renderer.setPixelRatio(window.devicePixelRatio);
    this.scene.background = new THREE.Color(0x2b3038);

    this.persp = new THREE.PerspectiveCamera(35, 1, 0.1, 100000);
    this.ortho = new THREE.OrthographicCamera(-1, 1, 1, -1, -100000, 100000);
    for (const c of [this.persp, this.ortho]) c.up.set(0, 0, 1); // Z up, like NX
    this.persp.position.set(120, -160, 110);
    this.camera = this.persp;

    this.controls = new OrbitControls(this.camera, canvas);
    this.controls.enableDamping = true;
    this.controls.dampingFactor = 0.2;
    this.controls.zoomToCursor = true;
    // NX-style mouse: middle rotates, right pans, left selects. Alt+left rotates (trackpads).
    this.controls.mouseButtons = { LEFT: -1 as unknown as THREE.MOUSE, MIDDLE: THREE.MOUSE.ROTATE, RIGHT: THREE.MOUSE.PAN };
    canvas.addEventListener("pointerdown", (e) => {
      this.controls.mouseButtons.LEFT = (e.altKey ? THREE.MOUSE.ROTATE : -1) as THREE.MOUSE;
      if (e.shiftKey && e.button === 1) this.controls.mouseButtons.MIDDLE = THREE.MOUSE.PAN;
      else this.controls.mouseButtons.MIDDLE = THREE.MOUSE.ROTATE;
    }, { capture: true });

    this.scene.add(new THREE.HemisphereLight(0xffffff, 0x404050, 1.3));
    const key = new THREE.DirectionalLight(0xffffff, 1.3);
    key.position.set(1, -1, 2);
    this.camera.add(key); // light follows the view
    this.scene.add(this.persp, this.ortho);
    const key2 = key.clone();
    this.ortho.add(key2);
    this.scene.add(new THREE.AxesHelper(20));
    this.buildBasePlanes();
    this.scene.add(this.body, this.sketchGroup, this.datumGroup, this.basePlanes, this.overlay);

    canvas.addEventListener("pointermove", (e) => this.onMove(e));
    canvas.addEventListener("pointerdown", (e) => this.onDown(e));
    canvas.addEventListener("pointerup", (e) => this.onUp(e));
    canvas.addEventListener("dblclick", (e) => this.input?.dblclick?.(e));
    canvas.addEventListener("contextmenu", (e) => e.preventDefault());
    new ResizeObserver(() => this.resize()).observe(canvas);
    this.resize();
    this.renderer.setAnimationLoop(() => {
      this.controls.update();
      this.renderer.render(this.scene, this.camera);
      for (const f of this.frameHooks) f();
    });
  }

  addFrameHook(f: () => void): () => void {
    this.frameHooks.push(f);
    return () => (this.frameHooks = this.frameHooks.filter((x) => x !== f));
  }

  // ------------------------------------------------------------- body

  show(data: MeshData | undefined, faceNames: string[], edgeNames: string[], refit = false): void {
    this.body.clear();
    this.mesh?.geometry.dispose();
    this.edgeLines?.geometry.dispose();
    this.mesh = null;
    this.edgeLines = null;
    this.faceNames = faceNames;
    this.edgeNames = edgeNames;
    if (!data) return;

    const geom = new THREE.BufferGeometry();
    geom.setAttribute("position", new THREE.Float32BufferAttribute(data.positions, 3));
    geom.setIndex(data.indices);
    geom.computeVertexNormals();
    geom.setAttribute("color", new THREE.BufferAttribute(new Float32Array(data.positions.length), 3));
    this.mesh = new THREE.Mesh(geom, new THREE.MeshStandardMaterial({
      vertexColors: true, metalness: 0.1, roughness: 0.6, side: THREE.DoubleSide,
      polygonOffset: true, polygonOffsetFactor: 1, polygonOffsetUnits: 1,
    }));
    this.faceIds = data.faceIds;

    const edgeGeom = new THREE.BufferGeometry();
    edgeGeom.setAttribute("position", new THREE.Float32BufferAttribute(data.edges, 3));
    edgeGeom.setAttribute("color", new THREE.BufferAttribute(new Float32Array(data.edges.length), 3));
    this.edgeLines = new THREE.LineSegments(edgeGeom, new THREE.LineBasicMaterial({ vertexColors: true }));
    this.edgeIds = data.edgeIds ?? [];

    this.body.add(this.mesh, this.edgeLines);
    // Keep hover/selection only if the names still exist.
    if (this.hover && !this.exists(this.hover)) {
      this.hover = null;
      this.onHover(null);
    }
    this.selected = this.selected.filter((p) => this.exists(p));
    this.applyDisplay();
    this.paint();
    if (refit) this.fit();
  }

  private exists(p: Pick): boolean {
    return (p.kind === "face" ? this.faceNames : this.edgeNames).includes(p.name);
  }

  hasBody(): boolean {
    return this.mesh !== null;
  }

  private ghosted = false;
  /** See-through body, for sketching. */
  setGhost(on: boolean): void {
    this.ghosted = on;
    this.applyDisplay();
  }

  setSketchesVisible(on: boolean): void {
    this.sketchGroup.visible = on;
  }

  setDatumsVisible(on: boolean): void {
    this.datumGroup.visible = on;
  }

  setDisplay(mode: DisplayMode): void {
    this.display = mode;
    this.applyDisplay();
  }

  private applyDisplay(): void {
    if (this.mesh) {
      const m = this.mesh.material as THREE.MeshStandardMaterial;
      m.wireframe = false;
      m.visible = this.display !== "wireframe";
      m.transparent = this.ghosted;
      m.opacity = this.ghosted ? 0.25 : 1;
      m.depthWrite = !this.ghosted;
      m.needsUpdate = true;
    }
    if (this.edgeLines) this.edgeLines.visible = this.display !== "shaded";
    if (this.display === "wireframe" && this.edgeLines) this.edgeLines.visible = true;
  }

  setSelection(picks: Pick[]): void {
    this.selected = picks;
    this.paint();
  }

  highlightFeature(id: string): void {
    this.featureHighlight = id;
    this.paint();
  }

  private faceColour(name: string): THREE.Color {
    if (this.hover?.kind === "face" && this.hover.name === name) return HOVER;
    if (this.selected.some((s) => s.kind === "face" && s.name === name)) return SELECTED;
    if (this.featureHighlight && name.startsWith(this.featureHighlight + "/")) return FEATURE;
    return BODY;
  }

  private edgeColour(name: string): THREE.Color {
    if (this.hover?.kind === "edge" && this.hover.name === name) return HOVER;
    if (this.selected.some((s) => s.kind === "edge" && s.name === name)) return SELECTED;
    return this.display === "wireframe" ? BODY : EDGE;
  }

  private paint(): void {
    if (this.mesh) {
      const colour = this.mesh.geometry.getAttribute("color") as THREE.BufferAttribute;
      const index = this.mesh.geometry.getIndex()!;
      const cache = new Map<number, THREE.Color>();
      for (let t = 0; t < this.faceIds.length; t++) {
        const f = this.faceIds[t];
        let c = cache.get(f);
        if (!c) { c = this.faceColour(this.faceNames[f] ?? ""); cache.set(f, c); }
        for (let k = 0; k < 3; k++) colour.setXYZ(index.getX(t * 3 + k), c.r, c.g, c.b);
      }
      colour.needsUpdate = true;
    }
    if (this.edgeLines) {
      const colour = this.edgeLines.geometry.getAttribute("color") as THREE.BufferAttribute;
      for (let s = 0; s < this.edgeIds.length; s++) {
        const c = this.edgeColour(this.edgeNames[this.edgeIds[s]] ?? "");
        colour.setXYZ(s * 2, c.r, c.g, c.b);
        colour.setXYZ(s * 2 + 1, c.r, c.g, c.b);
      }
      colour.needsUpdate = true;
    }
  }

  // ---------------------------------------------------- sketches, datums

  setSketches(list: { sketch: SketchData; placement: Placement; dim: boolean }[]): void {
    this.sketchGroup.clear();
    for (const { sketch, placement, dim } of list) {
      for (const e of sketch.entities) {
        if (e.type === "point" && e.p) {
          if (dim) continue; // a used sketch's points (hole centres) would sit inside the holes
          const g = new THREE.BufferGeometry().setFromPoints([planeToWorld(placement, e.p[0], e.p[1])]);
          this.sketchGroup.add(new THREE.Points(g, new THREE.PointsMaterial({ color: SKETCH_COLOUR, size: 6, sizeAttenuation: false })));
          continue;
        }
        const pts = entityPolyline(e).map(([x, y]) => planeToWorld(placement, x, y));
        if (pts.length < 2) continue;
        const mat = e.construction
          ? new THREE.LineDashedMaterial({ color: SKETCH_CONSTRUCTION, dashSize: 2, gapSize: 1.5, transparent: true, opacity: dim ? 0.4 : 0.9 })
          : new THREE.LineBasicMaterial({ color: SKETCH_COLOUR, transparent: true, opacity: dim ? 0.45 : 1 });
        const line = new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), mat);
        line.computeLineDistances();
        this.sketchGroup.add(line);
      }
    }
  }

  setDatums(datums: Datum[], size: number): void {
    this.datumGroup.clear();
    for (const d of datums) this.datumGroup.add(this.planeQuad(d, size, 0xd8b04c));
  }

  private planeQuad(p: Placement, size: number, colour: number): THREE.Group {
    const g = new THREE.Group();
    const s = size / 2;
    const corners = [[-s, -s], [s, -s], [s, s], [-s, s]].map(([x, y]) => planeToWorld(p, x, y));
    const geom = new THREE.BufferGeometry().setFromPoints([corners[0], corners[1], corners[2], corners[0], corners[2], corners[3]]);
    geom.computeVertexNormals();
    g.add(new THREE.Mesh(geom, new THREE.MeshBasicMaterial({ color: colour, transparent: true, opacity: 0.12, side: THREE.DoubleSide, depthWrite: false })));
    g.add(new THREE.LineLoop(new THREE.BufferGeometry().setFromPoints(corners), new THREE.LineBasicMaterial({ color: colour })));
    return g;
  }

  private buildBasePlanes(): void {
    const planes: [string, Placement][] = [
      ["XY", { origin: [0, 0, 0], xDir: [1, 0, 0], yDir: [0, 1, 0], normal: [0, 0, 1] }],
      ["XZ", { origin: [0, 0, 0], xDir: [1, 0, 0], yDir: [0, 0, 1], normal: [0, -1, 0] }],
      ["YZ", { origin: [0, 0, 0], xDir: [0, 1, 0], yDir: [0, 0, 1], normal: [1, 0, 0] }],
    ];
    for (const [name, p] of planes) {
      const q = this.planeQuad(p, 60, 0x6c8fb3);
      q.userData.plane = name;
      this.basePlanes.add(q);
    }
    this.basePlanes.visible = false;
  }

  showBasePlanes(on: boolean): void {
    this.basePlanes.visible = on;
  }

  /** Which base plane (XY/XZ/YZ) is under the pointer, if they are shown. */
  pickBasePlane(e: MouseEvent): string | null {
    if (!this.basePlanes.visible) return null;
    this.setPointer(e);
    const hits = this.raycaster.intersectObjects(this.basePlanes.children, true);
    for (const h of hits) {
      let o: THREE.Object3D | null = h.object;
      while (o && !o.userData.plane) o = o.parent;
      if (o) return o.userData.plane as string;
    }
    return null;
  }

  // ----------------------------------------------------------- picking

  private setPointer(e: MouseEvent): void {
    const r = this.canvas.getBoundingClientRect();
    this.pointer.set(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
    this.raycaster.setFromCamera(this.pointer, this.camera);
  }

  /** World size of one screen pixel at a point. */
  pixelSize(at: THREE.Vector3): number {
    const h = this.canvas.clientHeight || 1;
    if (this.camera instanceof THREE.OrthographicCamera) {
      return (this.camera.top - this.camera.bottom) / this.camera.zoom / h;
    }
    const d = this.camera.position.distanceTo(at);
    return (2 * d * Math.tan((this.persp.fov * Math.PI) / 360)) / h;
  }

  pickAt(e: MouseEvent, filter: PickFilter = this.filter): Pick | null {
    if (!this.mesh || filter === "none") return null;
    this.setPointer(e);
    const faceHit = this.raycaster.intersectObject(this.mesh)[0];
    if ((filter === "edge" || filter === "any") && this.edgeLines && this.edgeLines.visible !== false) {
      const ref = faceHit?.point ?? this.controls.target;
      this.raycaster.params.Line = { threshold: this.pixelSize(ref) * 6 };
      const edgeHit = this.raycaster.intersectObject(this.edgeLines)[0];
      if (edgeHit && edgeHit.index !== undefined && (!faceHit || edgeHit.distance <= faceHit.distance + this.pixelSize(ref) * 8)) {
        const name = this.edgeNames[this.edgeIds[Math.floor(edgeHit.index / 2)]];
        if (name) return { kind: "edge", name };
      }
      if (filter === "edge") return null;
    }
    if (faceHit?.faceIndex != null) {
      const name = this.faceNames[this.faceIds[faceHit.faceIndex]];
      if (name) return { kind: "face", name };
    }
    return null;
  }

  /** Pointer position on a plane, in its local 2D coordinates. */
  planeHit(e: MouseEvent, p: Placement): Vec2 | null {
    this.setPointer(e);
    const n = v3(p.normal);
    const plane = new THREE.Plane().setFromNormalAndCoplanarPoint(n, v3(p.origin));
    const hit = new THREE.Vector3();
    if (!this.raycaster.ray.intersectPlane(plane, hit)) return null;
    const d = hit.sub(v3(p.origin));
    return [d.dot(v3(p.xDir)), d.dot(v3(p.yDir))];
  }

  /** Screen position (CSS px, relative to the canvas) of a world point. */
  project(p: THREE.Vector3): { x: number; y: number; visible: boolean } {
    const v = p.clone().project(this.camera);
    return {
      x: ((v.x + 1) / 2) * this.canvas.clientWidth,
      y: ((1 - v.y) / 2) * this.canvas.clientHeight,
      visible: v.z > -1 && v.z < 1,
    };
  }

  private onMove(e: PointerEvent): void {
    if (this.input?.move?.(e)) return;
    if (e.buttons !== 0) return; // orbiting
    const p = this.pickAt(e);
    if (p?.kind !== this.hover?.kind || p?.name !== this.hover?.name) {
      this.hover = p;
      this.paint();
      this.onHover(p);
    }
  }

  private onDown(e: PointerEvent): void {
    this.downAt = { x: e.clientX, y: e.clientY };
    if (e.button === 0 && !e.altKey) this.input?.down?.(e);
  }

  private onUp(e: PointerEvent): void {
    const moved = this.downAt ? Math.hypot(e.clientX - this.downAt.x, e.clientY - this.downAt.y) > 4 : true;
    this.downAt = null;
    if (this.input?.up?.(e)) return;
    if (moved) return;
    if (e.button === 0 && !e.altKey) this.onClick(this.pickAt(e), e);
    if (e.button === 2) this.onContext(this.pickAt(e), e);
  }

  // ------------------------------------------------------------- views

  fit(): void {
    const box = new THREE.Box3();
    if (this.mesh) box.expandByObject(this.body);
    if (this.sketchGroup.children.length) box.expandByObject(this.sketchGroup);
    if (this.overlay.children.length) box.expandByObject(this.overlay);
    if (box.isEmpty()) box.set(new THREE.Vector3(-30, -30, -30), new THREE.Vector3(30, 30, 30));
    const centre = box.getCenter(new THREE.Vector3());
    const size = box.getSize(new THREE.Vector3()).length() || 1;
    const dir = this.camera.position.clone().sub(this.controls.target).normalize();
    this.controls.target.copy(centre);
    this.persp.position.copy(centre).addScaledVector(dir, size * 1.7);
    this.ortho.position.copy(centre).addScaledVector(dir, size * 4);
    this.ortho.zoom = 1;
    this.fitOrtho(size);
    this.controls.update();
  }

  private orthoSize = 100;
  private fitOrtho(size = this.orthoSize): void {
    this.orthoSize = size;
    const aspect = this.canvas.clientWidth / Math.max(1, this.canvas.clientHeight);
    const h = size * 0.65;
    this.ortho.left = -h * aspect;
    this.ortho.right = h * aspect;
    this.ortho.top = h;
    this.ortho.bottom = -h;
    this.ortho.updateProjectionMatrix();
  }

  setView(name: ViewName): void {
    const dirs: Record<ViewName, [Vec3, Vec3]> = {
      iso: [[1, -1, 0.8], [0, 0, 1]],
      top: [[0, 0, 1], [0, 1, 0]],
      bottom: [[0, 0, -1], [0, -1, 0]],
      front: [[0, -1, 0], [0, 0, 1]],
      back: [[0, 1, 0], [0, 0, 1]],
      right: [[1, 0, 0], [0, 0, 1]],
      left: [[-1, 0, 0], [0, 0, 1]],
    };
    const [d, up] = dirs[name];
    this.lookAlong(v3(d).normalize(), v3(up));
  }

  /** Look at the target from direction `dir` (towards the viewer), with `up`. */
  lookAlong(dir: THREE.Vector3, up: THREE.Vector3): void {
    const dist = Math.max(this.camera.position.distanceTo(this.controls.target), 10);
    for (const c of [this.persp, this.ortho]) {
      c.up.copy(up);
      c.position.copy(this.controls.target).addScaledVector(dir, c === this.ortho ? dist * 3 : dist);
      c.lookAt(this.controls.target);
    }
    this.controls.update();
    this.fit();
  }

  get orthographic(): boolean {
    return this.camera === this.ortho;
  }

  setOrthographic(on: boolean): void {
    const from = this.camera;
    const to = on ? this.ortho : this.persp;
    if (from === to) return;
    const dir = from.position.clone().sub(this.controls.target);
    to.up.copy(from.up);
    if (on) {
      // Match the perspective view's visible size.
      const visible = 2 * dir.length() * Math.tan((this.persp.fov * Math.PI) / 360);
      this.ortho.zoom = 1;
      this.fitOrtho(visible / 1.3);
      to.position.copy(this.controls.target).addScaledVector(dir.normalize(), Math.max(dir.length(), 1000));
    } else {
      const visible = (this.ortho.top - this.ortho.bottom) / this.ortho.zoom;
      const dist = visible / (2 * Math.tan((this.persp.fov * Math.PI) / 360));
      to.position.copy(this.controls.target).addScaledVector(dir.normalize(), dist);
    }
    this.camera = to;
    this.controls.object = to;
    this.controls.update();
  }

  private resize(): void {
    const { clientWidth: w, clientHeight: h } = this.canvas;
    if (w === 0 || h === 0) return;
    this.renderer.setSize(w, h, false);
    this.persp.aspect = w / h;
    this.persp.updateProjectionMatrix();
    this.fitOrtho();
  }
}
