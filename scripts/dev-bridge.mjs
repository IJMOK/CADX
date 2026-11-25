#!/usr/bin/env node
// Development bridge: lets the UI run in a plain browser against a real engine.
// It starts cadx-engine and relays POST /rpc bodies (one JSON-RPC line) to it.
// Listens on 127.0.0.1 only. Not used by the desktop app.
//
//   node scripts/dev-bridge.mjs path/to/cadx-engine [port=5174]
//   pnpm --dir ui dev          # then open http://localhost:5173/?bridge

import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { createInterface } from "node:readline";

const enginePath = process.argv[2] ?? process.env.CADX_ENGINE;
const port = Number(process.argv[3] ?? 5174);
if (!enginePath) {
  console.error("usage: dev-bridge.mjs <cadx-engine> [port]");
  process.exit(2);
}

let engine, lines, waiting = [];
function start() {
  engine = spawn(enginePath, [], { stdio: ["pipe", "pipe", "inherit"] });
  lines = createInterface({ input: engine.stdout });
  lines.on("line", (l) => waiting.shift()?.resolve(l));
  engine.on("exit", (code) => {
    console.error(`engine exited (${code}); restarting on next request`);
    for (const w of waiting) w.reject(new Error("engine exited"));
    waiting = [];
    engine = null;
  });
}

// One request at a time, like the Rust shell.
let chain = Promise.resolve();
function call(line) {
  const run = () => new Promise((resolve, reject) => {
    if (!engine) start();
    waiting.push({ resolve, reject });
    engine.stdin.write(line.replace(/\n/g, " ") + "\n");
  });
  const p = chain.then(run, run);
  chain = p.catch(() => undefined);
  return p;
}

createServer((req, res) => {
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Access-Control-Allow-Headers", "content-type");
  if (req.method === "OPTIONS") { res.end(); return; }
  if (req.method !== "POST" || req.url !== "/rpc") { res.statusCode = 404; res.end(); return; }
  let body = "";
  req.on("data", (c) => (body += c));
  req.on("end", async () => {
    try {
      const reply = await call(body);
      res.setHeader("content-type", "application/json");
      res.end(reply);
    } catch (e) {
      res.statusCode = 502;
      res.end(String(e));
    }
  });
}).listen(port, "127.0.0.1", () => console.error(`dev bridge on http://127.0.0.1:${port}/rpc -> ${enginePath}`));
