// CADX desktop shell: owns the window, launches the C++ engine as a child
// process and relays JSON-RPC lines between the UI and the engine. It also
// reads and writes .cadx files (a zip holding the part JSON).

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::fs::File;
use std::io::{BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::Mutex;

#[cfg(windows)]
use std::os::windows::process::CommandExt;

const CADX_FORMAT_VERSION: u32 = 1;

struct Engine {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
}

impl Engine {
    fn spawn() -> Result<Self, String> {
        let path = engine_path();
        let mut cmd = Command::new(&path);
        cmd.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(engine_log());
        if let Some(dir) = path.parent() {
            // The engine's DLLs (OpenCASCADE, TBB, ...) sit next to it.
            cmd.current_dir(dir);
        }
        #[cfg(windows)]
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no console flashing up
        let mut child = cmd
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
            return Err("the engine stopped (see cadx-engine.log in the temp folder)".into());
        }
        Ok(line.trim_end().to_owned())
    }

    fn alive(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(None))
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        let _ = self.child.kill();
    }
}

/// Engine logs: the dev terminal in debug builds, a temp file in release.
fn engine_log() -> Stdio {
    if cfg!(debug_assertions) {
        return Stdio::inherit();
    }
    match File::create(std::env::temp_dir().join("cadx-engine.log")) {
        Ok(f) => Stdio::from(f),
        Err(_) => Stdio::null(),
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
async fn engine_call(state: tauri::State<'_, EngineState>, request: String) -> Result<String, String> {
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

/// Write a .cadx file: a zip with manifest.json and part.json.
/// Written to a temporary file first so a failed save never truncates the old file.
#[tauri::command]
fn save_cadx(path: String, document: String) -> Result<(), String> {
    let doc: serde_json::Value = serde_json::from_str(&document).map_err(|e| format!("bad document: {e}"))?;
    let manifest = serde_json::json!({
        "format": "cadx",
        "version": CADX_FORMAT_VERSION,
        "generator": format!("CADX {}", env!("CARGO_PKG_VERSION")),
        "parts": [{ "name": doc.get("name").cloned().unwrap_or(serde_json::json!("Part1")), "file": "part.json" }],
        "root": "part.json"
    });
    let target = Path::new(&path);
    let tmp = target.with_extension("cadx.tmp");
    {
        let file = File::create(&tmp).map_err(|e| format!("could not write {}: {e}", tmp.display()))?;
        let mut zip = zip::ZipWriter::new(file);
        let opts = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
        zip.start_file("manifest.json", opts).map_err(|e| e.to_string())?;
        zip.write_all(serde_json::to_string_pretty(&manifest).unwrap().as_bytes()).map_err(|e| e.to_string())?;
        zip.start_file("part.json", opts).map_err(|e| e.to_string())?;
        zip.write_all(serde_json::to_string_pretty(&doc).unwrap().as_bytes()).map_err(|e| e.to_string())?;
        zip.finish().map_err(|e| e.to_string())?;
    }
    std::fs::rename(&tmp, target).map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        format!("could not replace {}: {e}", target.display())
    })
}

/// Read a .cadx file (or a bare part .json) and return the part document JSON.
#[tauri::command]
fn open_cadx(path: String) -> Result<String, String> {
    let mut file = File::open(&path).map_err(|e| format!("could not open {path}: {e}"))?;
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes).map_err(|e| e.to_string())?;
    if !bytes.starts_with(b"PK") {
        // Plain JSON part.
        let text = String::from_utf8(bytes).map_err(|_| "not a CADX file".to_string())?;
        serde_json::from_str::<serde_json::Value>(&text).map_err(|e| format!("not a CADX file: {e}"))?;
        return Ok(text);
    }
    let mut zip = zip::ZipArchive::new(std::io::Cursor::new(bytes)).map_err(|e| format!("not a CADX file: {e}"))?;
    let root = {
        let mut name = "part.json".to_string();
        if let Ok(mut m) = zip.by_name("manifest.json") {
            let mut s = String::new();
            m.read_to_string(&mut s).map_err(|e| e.to_string())?;
            let manifest: serde_json::Value = serde_json::from_str(&s).map_err(|e| format!("bad manifest: {e}"))?;
            if manifest.get("version").and_then(|v| v.as_u64()).unwrap_or(1) > CADX_FORMAT_VERSION as u64 {
                return Err("this file was saved by a newer CADX".into());
            }
            if let Some(r) = manifest.get("root").and_then(|v| v.as_str()) {
                name = r.to_string();
            }
        }
        name
    };
    let mut part = zip.by_name(&root).map_err(|_| format!("{root} is missing from the file"))?;
    let mut text = String::new();
    part.read_to_string(&mut text).map_err(|e| e.to_string())?;
    Ok(text)
}

/// A file passed on the command line (double-clicking a .cadx file).
#[tauri::command]
fn startup_file() -> Option<String> {
    std::env::args().skip(1).find(|a| !a.starts_with('-') && Path::new(a).exists())
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(EngineState::default())
        .invoke_handler(tauri::generate_handler![engine_call, save_cadx, open_cadx, startup_file])
        .run(tauri::generate_context!())
        .expect("error while running CADX");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cadx_round_trip() {
        let dir = std::env::temp_dir().join(format!("cadx-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("t.cadx");
        let doc = r#"{"format":"cadx-part","version":1,"name":"T","features":[]}"#;
        save_cadx(path.to_string_lossy().into(), doc.into()).unwrap();
        let back: serde_json::Value = serde_json::from_str(&open_cadx(path.to_string_lossy().into()).unwrap()).unwrap();
        assert_eq!(back["name"], "T");
        let json_path = dir.join("t.json");
        std::fs::write(&json_path, doc).unwrap();
        assert!(open_cadx(json_path.to_string_lossy().into()).is_ok());
        std::fs::remove_dir_all(&dir).ok();
    }
}
