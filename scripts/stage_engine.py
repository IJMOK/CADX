#!/usr/bin/env python3
"""Stage the engine for the Tauri bundle.

Copies cadx-engine(.exe) to app/src-tauri/binaries/cadx-engine-<target-triple>(.exe)
and, on Windows, every DLL it needs (found by walking PE import tables) into
app/src-tauri/engine-dlls/. System DLLs are left out; the MSVC runtime is
included so the app runs on machines without the VC++ redistributable.

usage: stage_engine.py <engine exe> <triple> <dll search dir>...
"""

import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TAURI = ROOT / "app" / "src-tauri"


def pe_imports(path: Path) -> list[str]:
    """Names of the DLLs a PE file imports (normal and delay-load)."""
    data = path.read_bytes()
    if data[:2] != b"MZ":
        return []
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return []
    coff = pe + 4
    nsections, = struct.unpack_from("<H", data, coff + 2)
    opt_size, = struct.unpack_from("<H", data, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from("<H", data, opt)
    dirs = opt + (112 if magic == 0x20B else 96)
    sections = []
    sec = opt + opt_size
    for i in range(nsections):
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + i * 40 + 8)
        sections.append((vaddr, max(vsize, rawsize), rawptr))

    def off(rva: int) -> int | None:
        for vaddr, size, rawptr in sections:
            if vaddr <= rva < vaddr + size:
                return rva - vaddr + rawptr
        return None

    def cstr(rva: int) -> str:
        o = off(rva)
        if o is None:
            return ""
        end = data.index(b"\0", o)
        return data[o:end].decode("ascii", "replace")

    names = []
    # Import directory (index 1): 20-byte descriptors, Name at +12.
    rva, size = struct.unpack_from("<II", data, dirs + 8 * 1)
    o = off(rva) if rva else None
    while o is not None:
        desc = struct.unpack_from("<IIIII", data, o)
        if not any(desc):
            break
        names.append(cstr(desc[3]))
        o += 20
    # Delay-load directory (index 13): 32-byte descriptors, Name at +4.
    rva, size = struct.unpack_from("<II", data, dirs + 8 * 13)
    o = off(rva) if rva else None
    while o is not None:
        desc = struct.unpack_from("<IIIIIIII", data, o)
        if not any(desc):
            break
        names.append(cstr(desc[1]))
        o += 32
    return [n for n in names if n]


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe = Path(sys.argv[1]).resolve()
    triple = sys.argv[2]
    search = [Path(p) for p in sys.argv[3:]]
    windows = exe.suffix.lower() == ".exe"

    bins = TAURI / "binaries"
    bins.mkdir(exist_ok=True)
    target = bins / f"cadx-engine-{triple}{'.exe' if windows else ''}"
    shutil.copy2(exe, target)
    print(f"engine -> {target.relative_to(ROOT)}")

    dlls = TAURI / "engine-dlls"
    if dlls.exists():
        shutil.rmtree(dlls)
    dlls.mkdir()
    (dlls / ".gitkeep").write_text("")
    if not windows:
        return 0

    index: dict[str, Path] = {}
    for d in search:
        if d.is_dir():
            for f in d.glob("*.dll"):
                index.setdefault(f.name.lower(), f)

    found: dict[str, Path] = {}
    missing: set[str] = set()
    todo = [exe]
    while todo:
        for name in pe_imports(todo.pop()):
            key = name.lower()
            if key in found or key in missing:
                continue
            if key in index:
                found[key] = index[key]
                todo.append(index[key])
            else:
                missing.add(key)  # a system DLL (kernel32, api-ms-win-*, ...)

    for key, src in sorted(found.items()):
        shutil.copy2(src, dlls / src.name)
    total = sum(p.stat().st_size for p in found.values())
    print(f"{len(found)} DLLs ({total / 1e6:.1f} MB) -> {dlls.relative_to(ROOT)}")
    for key in sorted(found):
        print(f"  {found[key].name}")
    unexpected = sorted(m for m in missing if not (m.startswith("api-ms-") or m.startswith("ext-ms-")))
    print("left to the system:", ", ".join(unexpected))
    return 0


if __name__ == "__main__":
    sys.exit(main())
