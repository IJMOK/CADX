// Thin client for the C++ engine. In the desktop app the Rust shell relays
// JSON-RPC lines to the engine sidecar; in a plain browser there is no engine.

import { invoke, isTauri } from "@tauri-apps/api/core";

interface RpcResponse<T> {
  jsonrpc: "2.0";
  id: number | null;
  result?: T;
  error?: { code: number; message: string };
}

let nextId = 1;
// Requests are serialised: the engine answers one line per request, in order.
let queue: Promise<unknown> = Promise.resolve();

// Development only: with ?bridge in the URL, a plain browser talks to the
// engine through scripts/dev-bridge.mjs on localhost.
const bridge = import.meta.env.DEV && new URLSearchParams(location.search).has("bridge")
  ? `http://127.0.0.1:${new URLSearchParams(location.search).get("bridge") || "5174"}/rpc`
  : null;

export const engineAvailable = (): boolean => isTauri() || bridge !== null;
export const inDesktopApp = (): boolean => isTauri();

export class EngineError extends Error {
  constructor(message: string, readonly code: number) {
    super(message);
  }
}

export function call<T>(method: string, params: Record<string, unknown> = {}): Promise<T> {
  if (!engineAvailable()) {
    return Promise.reject(new Error("No engine: run inside the CADX desktop app."));
  }
  const run = async () => {
    const request = JSON.stringify({ jsonrpc: "2.0", id: nextId++, method, params });
    const line = bridge
      ? await (await fetch(bridge, { method: "POST", body: request })).text()
      : await invoke<string>("engine_call", { request });
    const response = JSON.parse(line) as RpcResponse<T>;
    if (response.error) throw new EngineError(response.error.message, response.error.code);
    return response.result as T;
  };
  const p = queue.then(run, run);
  queue = p.catch(() => undefined);
  return p;
}

export const errorText = (err: unknown): string => (err instanceof Error ? err.message : String(err));
