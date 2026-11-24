// Thin client for the C++ engine. In the desktop app the Rust shell relays
// JSON-RPC lines to the engine sidecar; in a plain browser there is no engine yet.

import { invoke, isTauri } from "@tauri-apps/api/core";

export interface MeshData {
  positions: number[];
  indices: number[];
  faceIds: number[];
  edges: number[];
}

interface RpcResponse<T> {
  jsonrpc: "2.0";
  id: number | null;
  result?: T;
  error?: { code: number; message: string };
}

let nextId = 1;

export const engineAvailable = (): boolean => isTauri();

export async function call<T>(method: string, params: Record<string, unknown> = {}): Promise<T> {
  if (!engineAvailable()) {
    throw new Error("No engine: run inside the CADX desktop app (cargo tauri dev).");
  }
  const request = JSON.stringify({ jsonrpc: "2.0", id: nextId++, method, params });
  const line = await invoke<string>("engine_call", { request });
  const response = JSON.parse(line) as RpcResponse<T>;
  if (response.error) throw new Error(`${response.error.message} (${response.error.code})`);
  return response.result as T;
}
