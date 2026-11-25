// Shapes of the engine's JSON (see engine/src/rpc.hpp and part.hpp).

export type Vec2 = [number, number];
export type Vec3 = [number, number, number];
export type Num = number | string; // a number or an expression

export interface MeshData {
  positions: number[];
  indices: number[];
  faceIds: number[];
  edges: number[];
  edgeIds: number[];
}

export interface Placement {
  origin: Vec3;
  xDir: Vec3;
  yDir: Vec3;
  normal: Vec3;
}

export interface Entity {
  id: string;
  type: "point" | "line" | "circle" | "arc";
  construction?: boolean;
  p?: Vec2;
  a?: Vec2;
  b?: Vec2;
  center?: Vec2;
  radius?: number;
  start?: number;
  end?: number;
}

export interface Constraint {
  id: string;
  type: string;
  refs: string[];
  value?: Num;
}

export interface SketchData {
  id: string;
  entities: Entity[];
  constraints: Constraint[];
}

export interface SketchStatus {
  state: "solved" | "conflicting" | "failed" | "invalid";
  dof: number;
  conflicting: string[];
  redundant: string[];
  message: string;
  sketch: SketchData;
  placement?: Placement;
}

export interface Feature {
  id: string;
  type: string;
  name?: string;
  suppressed?: boolean;
  [key: string]: unknown;
}

export interface Parameter {
  name: string;
  expression: string;
}

export interface PartDocument {
  format: "cadx-part";
  version: number;
  name: string;
  units: string;
  parameters: Parameter[];
  features: Feature[];
}

export interface FeatureStatus {
  id: string;
  state: "ok" | "failed" | "suppressed";
  message: string;
}

export interface Datum extends Placement {
  id: string;
}

export interface State {
  revision: number;
  name: string;
  document: PartDocument;
  canUndo: boolean;
  undoLabel: string;
  canRedo: boolean;
  redoLabel: string;
  rollback: string | null;
  features: FeatureStatus[];
  sketches: SketchStatus[];
  datums: Datum[];
  parameterValues: Record<string, number>;
  expressionErrors: string[];
  mesh?: MeshData;
  faceNames?: string[];
  edgeNames?: string[];
  id?: string; // set by addFeature / importFile
}

export interface Measure {
  empty?: boolean;
  volume: number;
  area: number;
  centreOfMass: Vec3;
  bboxMin: Vec3;
  bboxMax: Vec3;
  solids: number;
  faces: number;
  edges: number;
}

export type Pick = { kind: "face" | "edge"; name: string };
