// Files: .cadx documents (a zip written by the Rust shell), STEP/IGES/STL
// export and import (done by the engine), recent files.

import { invoke } from "@tauri-apps/api/core";
import { open, save } from "@tauri-apps/plugin-dialog";
import type { PartDocument } from "./types";

const RECENT_KEY = "cadx.recent";

export const CADX_FILTER = { name: "CADX part", extensions: ["cadx"] };
export const OPEN_FILTERS = [
  { name: "All supported", extensions: ["cadx", "json", "step", "stp", "iges", "igs", "brep"] },
  CADX_FILTER,
  { name: "STEP / IGES / BREP", extensions: ["step", "stp", "iges", "igs", "brep"] },
];
export const EXPORT_FILTERS: Record<string, { name: string; extensions: string[] }> = {
  step: { name: "STEP AP214", extensions: ["step", "stp"] },
  iges: { name: "IGES", extensions: ["iges", "igs"] },
  stl: { name: "STL (binary)", extensions: ["stl"] },
  brep: { name: "OpenCASCADE BREP", extensions: ["brep"] },
};

export const isNative = (path: string) => /\.(cadx|json)$/i.test(path);
export const baseName = (path: string) => path.split(/[\\/]/).pop() ?? path;

export async function pickOpen(): Promise<string | null> {
  const r = await open({ multiple: false, directory: false, filters: OPEN_FILTERS });
  return typeof r === "string" ? r : null;
}

export async function pickImport(): Promise<string | null> {
  const r = await open({ multiple: false, directory: false, filters: [OPEN_FILTERS[2]] });
  return typeof r === "string" ? r : null;
}

export async function pickSave(defaultName: string): Promise<string | null> {
  const r = await save({ filters: [CADX_FILTER], defaultPath: defaultName.endsWith(".cadx") ? defaultName : `${defaultName}.cadx` });
  if (!r) return null;
  return /\.cadx$/i.test(r) ? r : `${r}.cadx`;
}

export async function pickExport(format: string, defaultName: string): Promise<string | null> {
  const f = EXPORT_FILTERS[format];
  const r = await save({ filters: [f], defaultPath: `${defaultName}.${f.extensions[0]}` });
  if (!r) return null;
  return f.extensions.some((e) => r.toLowerCase().endsWith(`.${e}`)) ? r : `${r}.${f.extensions[0]}`;
}

export async function readDocument(path: string): Promise<PartDocument> {
  const text = await invoke<string>("open_cadx", { path });
  return JSON.parse(text) as PartDocument;
}

export async function writeDocument(path: string, doc: PartDocument): Promise<void> {
  await invoke("save_cadx", { path, document: JSON.stringify(doc) });
}

export async function startupFile(): Promise<string | null> {
  try {
    return await invoke<string | null>("startup_file");
  } catch {
    return null;
  }
}

export function recentFiles(): string[] {
  try {
    return (JSON.parse(localStorage.getItem(RECENT_KEY) ?? "[]") as string[]).slice(0, 10);
  } catch {
    return [];
  }
}

export function addRecent(path: string): void {
  const list = [path, ...recentFiles().filter((p) => p !== path)].slice(0, 10);
  localStorage.setItem(RECENT_KEY, JSON.stringify(list));
}

export function removeRecent(path: string): void {
  localStorage.setItem(RECENT_KEY, JSON.stringify(recentFiles().filter((p) => p !== path)));
}
