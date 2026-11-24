// CADX desktop shell: owns the window, launches the C++ engine as a child
// process and relays JSON-RPC lines between the UI and the engine.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::Mutex;

struct Engine {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
}

impl Engine {
    fn spawn() -> Result<Self, String> {
        let path = engine_path();
        let mut child = Command::new(&path)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit()) // engine logs appear in the dev terminal
            .spawn()
            .map_err(|e| format!("could not start engine at {}: {e}", path.display()))?;
        let stdin = child.stdin.take().ok_or("engine stdin unavailable")?;
        let stdout = BufReader::new(child.stdout.take().ok_or("engine stdout unavailable")?);
        Ok(Self { child, stdin, stdout })
    }

    fn call(&mut self, request: &str) -> Result<String, String> {
        if request.contains('\n') {
            return Err("request must be a single line".into());
        }
        writeln!(self.stdin, "{request}").map_err(|e| e.to_string())?;
        self.stdin.flush().map_err(|e| e.to_string())?;
        let mut line = String::new();
        let n = self.stdout.read_line(&mut line).map_err(|e| e.to_string())?;
        if n == 0 {
            return Err("engine closed its output".into());
        }
        Ok(line.trim_end().to_owned())
    }

    fn alive(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(None))
    }
}

/// Engine binary: $CADX_ENGINE if set, else `cadx-engine` next to this executable.
fn engine_path() -> PathBuf {
    if let Ok(p) = std::env::var("CADX_ENGINE") {
        return PathBuf::from(p);
    }
    let name = if cfg!(windows) { "cadx-engine.exe" } else { "cadx-engine" };
    std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(|d| d.join(name)))
        .unwrap_or_else(|| PathBuf::from(name))
}

#[derive(Default)]
struct EngineState(Mutex<Option<Engine>>);

/// Send one JSON-RPC request line to the engine and return its response line.
/// Starts the engine on first use, and restarts it if it has died.
#[tauri::command]
fn engine_call(state: tauri::State<'_, EngineState>, request: String) -> Result<String, String> {
    let mut guard = state.0.lock().map_err(|_| "engine lock poisoned")?;
    let needs_start = match guard.as_mut() {
        Some(engine) => !engine.alive(),
        None => true,
    };
    if needs_start {
        *guard = Some(Engine::spawn()?);
    }
    let result = guard.as_mut().expect("engine started").call(&request);
    if result.is_err() {
        *guard = None; // next call restarts it
    }
    result
}

fn main() {
    tauri::Builder::default()
        .manage(EngineState::default())
        .invoke_handler(tauri::generate_handler![engine_call])
        .run(tauri::generate_context!())
        .expect("error while running CADX");
}
