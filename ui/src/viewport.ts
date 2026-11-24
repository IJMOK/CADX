import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
import type { MeshData } from "./engine";

const BODY_COLOUR = 0x9aa4b2;
const HOVER_COLOUR = 0xffb347;

export class Viewport {
  private renderer: THREE.WebGLRenderer;
  private scene = new THREE.Scene();
  private camera: THREE.PerspectiveCamera;
  private controls: OrbitControls;
  private raycaster = new THREE.Raycaster();
  private pointer = new THREE.Vector2();
  private body: THREE.Group | null = null;
  private mesh: THREE.Mesh | null = null;
  private faceIds: number[] = [];
  private hoveredFace = -1;

  /** Called with the B-rep face index under the cursor, or -1. */
  onHover: (faceId: number) => void = () => {};

  constructor(private canvas: HTMLCanvasElement) {
    this.renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
    this.renderer.setPixelRatio(window.devicePixelRatio);
    this.scene.background = new THREE.Color(0x2b3038);

    this.camera = new THREE.PerspectiveCamera(40, 1, 0.1, 10000);
    this.camera.up.set(0, 0, 1); // Z up, like NX
    this.camera.position.set(120, -160, 110);

    this.controls = new OrbitControls(this.camera, canvas);
    this.controls.enableDamping = true;

    this.scene.add(new THREE.HemisphereLight(0xffffff, 0x404050, 1.2));
    const key = new THREE.DirectionalLight(0xffffff, 1.4);
    key.position.set(1, -1, 2);
    this.scene.add(key);
    this.scene.add(new THREE.AxesHelper(20));

    canvas.addEventListener("pointermove", (e) => this.pick(e));
    new ResizeObserver(() => this.resize()).observe(canvas);
    this.resize();
    this.renderer.setAnimationLoop(() => {
      this.controls.update();
      this.renderer.render(this.scene, this.camera);
    });
  }

  show(data: MeshData): void {
    if (this.body) this.scene.remove(this.body);

    const geom = new THREE.BufferGeometry();
    geom.setAttribute("position", new THREE.Float32BufferAttribute(data.positions, 3));
    geom.setIndex(data.indices);
    geom.computeVertexNormals();
    // Per-vertex colour so a hovered face can be highlighted without new materials.
    const colours = new Float32Array(data.positions.length);
    geom.setAttribute("color", new THREE.BufferAttribute(colours, 3));

    this.mesh = new THREE.Mesh(
      geom,
      new THREE.MeshStandardMaterial({
        vertexColors: true, metalness: 0.1, roughness: 0.6,
        polygonOffset: true, polygonOffsetFactor: 1, polygonOffsetUnits: 1,
      }),
    );
    this.faceIds = data.faceIds;
    this.hoveredFace = -1;
    this.paint();

    const edgeGeom = new THREE.BufferGeometry();
    edgeGeom.setAttribute("position", new THREE.Float32BufferAttribute(data.edges, 3));
    const edges = new THREE.LineSegments(edgeGeom, new THREE.LineBasicMaterial({ color: 0x111418 }));

    this.body = new THREE.Group();
    this.body.add(this.mesh, edges);
    this.scene.add(this.body);
    this.fit();
  }

  fit(): void {
    if (!this.body) return;
    const box = new THREE.Box3().setFromObject(this.body);
    const centre = box.getCenter(new THREE.Vector3());
    const size = box.getSize(new THREE.Vector3()).length() || 1;
    const dir = this.camera.position.clone().sub(this.controls.target).normalize();
    this.controls.target.copy(centre);
    this.camera.position.copy(centre).addScaledVector(dir, size * 1.6);
  }

  private pick(e: PointerEvent): void {
    if (!this.mesh) return;
    const r = this.canvas.getBoundingClientRect();
    this.pointer.set(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
    this.raycaster.setFromCamera(this.pointer, this.camera);
    const hit = this.raycaster.intersectObject(this.mesh)[0];
    const face = hit?.faceIndex != null ? (this.faceIds[hit.faceIndex] ?? -1) : -1;
    if (face !== this.hoveredFace) {
      this.hoveredFace = face;
      this.paint();
      this.onHover(face);
    }
  }

  private paint(): void {
    if (!this.mesh) return;
    const geom = this.mesh.geometry;
    const colour = geom.getAttribute("color") as THREE.BufferAttribute;
    const index = geom.getIndex()!;
    const base = new THREE.Color(BODY_COLOUR);
    const hover = new THREE.Color(HOVER_COLOUR);
    for (let t = 0; t < this.faceIds.length; t++) {
      const c = this.faceIds[t] === this.hoveredFace ? hover : base;
      for (let k = 0; k < 3; k++) colour.setXYZ(index.getX(t * 3 + k), c.r, c.g, c.b);
    }
    colour.needsUpdate = true;
  }

  private resize(): void {
    const { clientWidth: w, clientHeight: h } = this.canvas;
    if (w === 0 || h === 0) return;
    this.renderer.setSize(w, h, false);
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
  }
}
