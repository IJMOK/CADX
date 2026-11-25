import { defineConfig } from "vite";

// Tauri expects a fixed dev port and no screen clearing.
export default defineConfig({
  clearScreen: false,
  server: { port: 5173, strictPort: true },
  build: { target: "es2022", chunkSizeWarningLimit: 1500 },
});
