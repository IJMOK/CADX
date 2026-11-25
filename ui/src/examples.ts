// Example parts, built from the same document format the engine saves.

import type { PartDocument } from "./types";

type J = Record<string, unknown>;

const line = (id: string, a: [number, number], b: [number, number]): J => ({ id, type: "line", a, b });
const con = (id: string, type: string, refs: string[], value?: number | string): J =>
  value === undefined ? { id, type, refs } : { id, type, refs, value };

/** A fully constrained rectangle from the origin, w x h (numbers or expressions). */
function rectangle(w: number | string, h: number | string, guessW: number, guessH: number): { entities: J[]; constraints: J[] } {
  return {
    entities: [line("L1", [0, 0], [guessW, 0]), line("L2", [guessW, 0], [guessW, guessH]),
      line("L3", [guessW, guessH], [0, guessH]), line("L4", [0, guessH], [0, 0])],
    constraints: [
      con("c1", "coincident", ["L1.end", "L2.start"]), con("c2", "coincident", ["L2.end", "L3.start"]),
      con("c3", "coincident", ["L3.end", "L4.start"]), con("c4", "coincident", ["L4.end", "L1.start"]),
      con("c5", "horizontal", ["L1"]), con("c6", "horizontal", ["L3"]), con("c7", "vertical", ["L2"]), con("c8", "vertical", ["L4"]),
      con("c9", "coincident", ["L1.start", "origin"]), con("d1", "distance", ["L1"], w), con("d2", "distance", ["L2"], h),
    ],
  };
}

function points(list: [string, [number, number], string | number, string | number][]): { entities: J[]; constraints: J[] } {
  const entities: J[] = [], constraints: J[] = [];
  list.forEach(([id, p, x, y], i) => {
    entities.push({ id, type: "point", p });
    constraints.push(con(`dx${i + 1}`, "distanceX", [id], x), con(`dy${i + 1}`, "distanceY", [id], y));
  });
  return { entities, constraints };
}

export function bracketExample(): PartDocument {
  return {
    format: "cadx-part", version: 1, name: "Bracket", units: "mm",
    parameters: [
      { name: "length", expression: "80" }, { name: "width", expression: "40" }, { name: "height", expression: "45" },
      { name: "thick", expression: "6" }, { name: "holeD", expression: "6.6" }, { name: "inset", expression: "14" },
      { name: "blendR", expression: "thick" },
    ],
    features: [
      { id: "Sketch1", type: "sketch", name: "Base outline", plane: "XY", ...rectangle("length", "width", 80, 40) },
      { id: "Extrude1", type: "extrude", name: "Base plate", sketch: "Sketch1", distance: "thick", op: "add" },
      { id: "Sketch2", type: "sketch", name: "Upright outline", plane: { base: "XZ", offset: "-width" }, ...rectangle("length", "height", 80, 45) },
      { id: "Extrude2", type: "extrude", name: "Upright", sketch: "Sketch2", distance: "thick", op: "add" },
      { id: "Sketch3", type: "sketch", name: "Base holes", plane: "Extrude1/end",
        ...points([["P1", [14, 14], "inset", "inset"], ["P2", [66, 14], "length - inset", "inset"]]) },
      { id: "Hole1", type: "hole", sketch: "Sketch3", diameter: "holeD", through: true, form: "counterbore", cbDiameter: "holeD * 1.7", cbDepth: "thick / 2" },
      { id: "Sketch4", type: "sketch", name: "Upright holes", plane: "Extrude2/end",
        ...points([["P1", [20, 30], "length / 4", "height - inset"], ["P2", [60, 30], "length * 3 / 4", "height - inset"]]) },
      { id: "Hole2", type: "hole", sketch: "Sketch4", diameter: "holeD", through: true },
      { id: "Fillet1", type: "fillet", name: "Inside blend", radius: "blendR", edges: ["Extrude1/end|Extrude2/end"] },
      { id: "Chamfer1", type: "chamfer", name: "Top edge", distance: 1, edges: ["Extrude2/end|Extrude2/side(Sketch2/L3)"] },
    ] as PartDocument["features"],
  };
}

export function flangeExample(): PartDocument {
  return {
    format: "cadx-part", version: 1, name: "Flange", units: "mm",
    parameters: [
      { name: "od", expression: "100" }, { name: "bore", expression: "40" }, { name: "t", expression: "12" },
      { name: "hubD", expression: "60" }, { name: "hubH", expression: "20" }, { name: "pcd", expression: "(od + hubD) / 2" },
      { name: "bolts", expression: "6" },
    ],
    features: [
      { id: "Sketch1", type: "sketch", name: "Half section", plane: "XZ",
        entities: [line("L1", [20, 0], [50, 0]), line("L2", [50, 0], [50, 12]), line("L3", [50, 12], [30, 12]),
          line("L4", [30, 12], [30, 32]), line("L5", [30, 32], [20, 32]), line("L6", [20, 32], [20, 0])],
        constraints: [
          con("c1", "coincident", ["L1.end", "L2.start"]), con("c2", "coincident", ["L2.end", "L3.start"]),
          con("c3", "coincident", ["L3.end", "L4.start"]), con("c4", "coincident", ["L4.end", "L5.start"]),
          con("c5", "coincident", ["L5.end", "L6.start"]), con("c6", "coincident", ["L6.end", "L1.start"]),
          con("h1", "horizontal", ["L1"]), con("h2", "horizontal", ["L3"]), con("h3", "horizontal", ["L5"]),
          con("v1", "vertical", ["L2"]), con("v2", "vertical", ["L4"]), con("v3", "vertical", ["L6"]),
          con("p1", "pointOn", ["L1.start", "xaxis"]),
          con("d1", "distanceX", ["L1.start"], "bore / 2"), con("d2", "distanceX", ["L2.start"], "od / 2"),
          con("d3", "distance", ["L2"], "t"), con("d4", "distanceX", ["L4.start"], "hubD / 2"),
          con("d5", "distance", ["L6"], "t + hubH"),
        ] },
      { id: "Revolve1", type: "revolve", name: "Body", sketch: "Sketch1", axis: "V", angle: 360, op: "add" },
      { id: "Sketch2", type: "sketch", name: "Bolt hole", plane: "Revolve1/side(Sketch1/L3)",
        entities: [{ id: "P1", type: "point", p: [40, 0] }],
        constraints: [con("d1", "distanceX", ["P1"], "pcd / 2"), con("p1", "pointOn", ["P1", "xaxis"])] },
      { id: "Hole1", type: "hole", sketch: "Sketch2", diameter: 9, through: true },
      { id: "Pattern1", type: "circularPattern", name: "Bolt circle", features: ["Hole1"], axis: "Z", count: "bolts", angle: 360 },
      { id: "Fillet1", type: "fillet", name: "Hub root", radius: 3, edges: ["Revolve1/side(Sketch1/L3)|Revolve1/side(Sketch1/L4)"] },
    ] as PartDocument["features"],
  };
}
