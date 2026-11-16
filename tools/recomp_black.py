#!/usr/bin/env python3
"""Pipeline de recompilação real do BLACK (Xbox NTSC-U) usando TGP482/XboxXBERecomp (sdk/).

Etapas executadas:
  1. Garante dependências de compilação do recompilador (Zydis v4.1.1 + toml++ v3.4.0) em build/deps/.
  2. Compila `build/compiler/xbe_recomp.exe` e `build/compiler/xbe_symbols.exe` a partir de `sdk/tools/`.
  3. Se `assets/default.xbe` não existir (ou for menor que 1 MB), procura uma `.iso` em `assets/` e
     extrai automaticamente via `tools/xiso_extract.py`.
  4. Executa `xbe_recomp.exe config/game.toml --xdk sdk/xdk` para gerar os fontes C++ recompilados em
     `generated/game/` (`x86_recomp.0.cpp`..`63.cpp`, `x86_func_mapping.cpp`, `image_symbols.h`, `hook_exports.h`).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xiso_extract

REAL_XBE_MIN_BYTES = 1_000_000
EXPECTED_TITLE_ID = 0x45410083
EXPECTED_RETAIL_SHA256 = "DF2739C372D254A90AEECCF5971D12097F22D89FBB90DB021FDA96DD4DEB68F6"


def _find_zig_python(repo_root: Path) -> list[str] | None:
    """Localiza python + ziglang ou clang++ no sistema."""
    try:
        res = subprocess.run([sys.executable, "-m", "ziglang", "version"], capture_output=True, text=True)
        if res.returncode == 0:
            return [sys.executable, "-m", "ziglang"]
    except OSError:
        pass

    local_appdata = Path(os.environ.get("LOCALAPPDATA", ""))
    gemini_zig = Path(
        r"C:\Users\Vi\.gemini\antigravity\brain\88a5c853-e71e-4983-802f-2ebf2e0b90e9\scratch\venv\Scripts\python.exe"
    )
    for cand in [
        repo_root / ".venv" / "Scripts" / "python.exe",
        gemini_zig,
        local_appdata / "Programs" / "zig" / "python.exe",
    ]:
        if cand.is_file():
            res = subprocess.run([str(cand), "-m", "ziglang", "version"], capture_output=True, text=True)
            if res.returncode == 0:
                return [str(cand), "-m", "ziglang"]
    return None


def ensure_deps(repo_root: Path) -> tuple[Path, Path]:
    deps_dir = repo_root / "build" / "deps"
    deps_dir.mkdir(parents=True, exist_ok=True)
    zydis_dir = deps_dir / "zydis"
    toml_dir = deps_dir / "tomlplusplus"

    if not (zydis_dir / "include" / "Zydis" / "Zydis.h").is_file():
        print("[recomp_black] Clonando Zydis v4.1.1 (+ zycore)...")
        subprocess.run(
            [
                "git",
                "clone",
                "--depth",
                "1",
                "--branch",
                "v4.1.1",
                "--recurse-submodules",
                "--shallow-submodules",
                "https://github.com/zyantific/zydis.git",
                str(zydis_dir),
            ],
            check=True,
        )

    if not (toml_dir / "include" / "toml++" / "toml.hpp").is_file():
        print("[recomp_black] Clonando tomlplusplus v3.4.0...")
        subprocess.run(
            [
                "git",
                "clone",
                "--depth",
                "1",
                "--branch",
                "v3.4.0",
                "https://github.com/marzer/tomlplusplus.git",
                str(toml_dir),
            ],
            check=True,
        )

    return zydis_dir, toml_dir


def build_toolchain(repo_root: Path, force: bool = False) -> tuple[Path, Path]:
    compiler_dir = repo_root / "build" / "compiler"
    compiler_dir.mkdir(parents=True, exist_ok=True)
    recomp_exe = compiler_dir / "xbe_recomp.exe"
    symbols_exe = compiler_dir / "xbe_symbols.exe"

    if not force and recomp_exe.is_file() and symbols_exe.is_file():
        return recomp_exe, symbols_exe

    zydis_dir, toml_dir = ensure_deps(repo_root)
    zig_cmd = _find_zig_python(repo_root)
    clang = shutil.which("clang")
    clangxx = shutil.which("clang++")

    if zig_cmd:
        cc_prefix = [*zig_cmd, "cc"]
        cxx_prefix = [*zig_cmd, "c++"]
    elif clang and clangxx:
        cc_prefix = [clang]
        cxx_prefix = [clangxx]
    else:
        raise RuntimeError(
            "Nenhum compilador C/C++ (ziglang ou clang++) encontrado para compilar xbe_recomp/xbe_symbols."
        )

    obj_dir = repo_root / "build" / "deps" / "zydis_objs"
    obj_dir.mkdir(parents=True, exist_ok=True)

    c_flags = [
        "-std=c11",
        "-O2",
        "-DZYDIS_STATIC_BUILD",
        "-DZYCORE_STATIC_BUILD",
        "-D_CRT_SECURE_NO_WARNINGS",
        f"-I{zydis_dir / 'include'}",
        f"-I{zydis_dir / 'src'}",
        f"-I{zydis_dir / 'dependencies' / 'zycore' / 'include'}",
    ]
    c_sources = sorted((zydis_dir / "src").rglob("*.c")) + sorted(
        (zydis_dir / "dependencies" / "zycore" / "src").rglob("*.c")
    )

    objs: list[str] = []
    for idx, src in enumerate(c_sources):
        obj = obj_dir / f"z_{idx}_{src.stem}.o"
        objs.append(str(obj))
        if force or not obj.is_file() or obj.stat().st_mtime < src.stat().st_mtime:
            subprocess.run([*cc_prefix, *c_flags, "-c", str(src), "-o", str(obj)], check=True)

    sdk_dir = repo_root / "sdk"
    cxx_flags = [
        "-std=c++20",
        "-O2",
        "-DZYDIS_STATIC_BUILD",
        "-DZYCORE_STATIC_BUILD",
        "-D_CRT_SECURE_NO_WARNINGS",
        f"-I{zydis_dir / 'include'}",
        f"-I{zydis_dir / 'dependencies' / 'zycore' / 'include'}",
        f"-I{toml_dir / 'include'}",
        f"-I{sdk_dir / 'runtime'}",
        f"-I{sdk_dir / 'tools' / 'recomp'}",
    ]

    recomp_srcs = [str(p) for p in sorted((sdk_dir / "tools" / "recomp").glob("*.cpp"))]
    if force or not recomp_exe.is_file():
        print("[recomp_black] Compilando build/compiler/xbe_recomp.exe...")
        subprocess.run([*cxx_prefix, *cxx_flags, *recomp_srcs, *objs, "-o", str(recomp_exe)], check=True)

    symbols_src = str(sdk_dir / "tools" / "symbols" / "main.cpp")
    if force or not symbols_exe.is_file():
        print("[recomp_black] Compilando build/compiler/xbe_symbols.exe...")
        subprocess.run([*cxx_prefix, *cxx_flags, symbols_src, *objs, "-o", str(symbols_exe)], check=True)

    return recomp_exe, symbols_exe


def _inspect_xbe(path: Path) -> tuple[bool, int, str]:
    with open(path, "rb") as fp:
        hdr = fp.read(0x200)
    if len(hdr) < 0x180 or hdr[:4] != b"XBEH":
        return False, 0, "Cabeçalho XBEH ausente"
    base_addr = struct.unpack_from("<I", hdr, 0x104)[0]
    cert_addr = struct.unpack_from("<I", hdr, 0x118)[0]
    cert_off = cert_addr - base_addr
    title_id = 0
    if 0 <= cert_off and cert_off + 0x0C <= len(hdr):
        title_id = struct.unpack_from("<I", hdr, cert_off + 0x08)[0]
    else:
        with open(path, "rb") as fp:
            if cert_off >= 0:
                fp.seek(cert_off + 0x08)
                raw = fp.read(4)
                if len(raw) == 4:
                    title_id = struct.unpack("<I", raw)[0]
    sz = path.stat().st_size
    digest = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1024 * 1024), b""):
            digest.update(chunk)
    sha256 = digest.hexdigest().upper()
    sha_note = (
        "SHA-256 retail NTSC-U exato"
        if sha256 == EXPECTED_RETAIL_SHA256
        else f"SHA-256 difere da referência NTSC-U ({sha256[:16]}...)"
    )
    ok = (title_id == EXPECTED_TITLE_ID) and (sz >= REAL_XBE_MIN_BYTES)
    return ok, title_id, f"size={sz} bytes, TitleID=0x{title_id:08X}, {sha_note}"


def check_toolchain(repo_root: Path) -> int:
    recomp_exe, symbols_exe = build_toolchain(repo_root)
    print(f"[recomp_black] Toolchain OK: {recomp_exe.name} ({recomp_exe.stat().st_size} B), {symbols_exe.name} ({symbols_exe.stat().st_size} B)")

    sym_proc = subprocess.run([str(symbols_exe)], cwd=repo_root, capture_output=True, text=True)
    if "xbe_symbols" not in (sym_proc.stdout + sym_proc.stderr):
        print("[recomp_black] ERRO: saída inesperada de xbe_symbols.exe", file=sys.stderr)
        return 1

    cfg_path = repo_root / "config" / "game.toml"
    xdk_dir = repo_root / "sdk" / "xdk"
    rec_proc = subprocess.run(
        [str(recomp_exe), str(cfg_path), "--xdk", str(xdk_dir)],
        cwd=repo_root,
        capture_output=True,
        text=True,
    )
    combined = (rec_proc.stdout + rec_proc.stderr).strip()
    xbe_path = repo_root / "assets" / "default.xbe"
    if xbe_path.is_file():
        print(f"[recomp_black] xbe_recomp exit={rec_proc.returncode}: {combined}")
        return rec_proc.returncode

    if "cannot open" in combined and "default.xbe" in combined:
        print(
            "[recomp_black] OK: xbe_recomp.exe validou config/game.toml + sdk/xdk/5849/layout.toml "
            "e aguarda assets/default.xbe real."
        )
        return 0

    print(f"[recomp_black] Falha na validação da config (exit={rec_proc.returncode}):\n{combined}", file=sys.stderr)
    return 1


def _ensure_real_xbe(repo_root: Path) -> Path | None:
    assets_dir = repo_root / "assets"
    xbe_path = assets_dir / "default.xbe"

    if not xbe_path.is_file() or xbe_path.stat().st_size < REAL_XBE_MIN_BYTES:
        isos = sorted(p for p in assets_dir.iterdir() if p.is_file() and p.suffix.lower() == ".iso") if assets_dir.is_dir() else []
        if isos:
            print(f"[recomp_black] Extraindo {isos[0].name} para {assets_dir}...")
            xiso_extract.extract_iso(isos[0], assets_dir)

    if not xbe_path.is_file():
        print(
            "[recomp_black] ERRO: assets/default.xbe não encontrado.\n"
            "Coloque a ISO de BLACK (Xbox NTSC-U) ou os arquivos extraídos (default.xbe, GlobData.bin, levels/, chars/, etc.) em assets/.",
            file=sys.stderr,
        )
        return None

    ok, _, info = _inspect_xbe(xbe_path)
    print(f"[recomp_black] Encontrado default.xbe: {info}")
    if not ok:
        print(
            f"[recomp_black] ERRO: default.xbe inválido para BLACK NTSC-U (esperado TitleID=0x{EXPECTED_TITLE_ID:08X}, >= 1 MB).",
            file=sys.stderr,
        )
        return None

    return xbe_path


def run_symbols(repo_root: Path) -> int:
    """Executa xbe_symbols.exe sobre assets/default.xbe para gerar config/game.toml limpo (Fase 0)."""
    xbe_path = _ensure_real_xbe(repo_root)
    if xbe_path is None:
        return 1

    _, symbols_exe = build_toolchain(repo_root)
    cmd = [
        str(symbols_exe),
        str(xbe_path),
        str(repo_root / "sdk" / "xdk"),
        "--library",
        "BlackEnhancedOnline.dll",
        "--out",
        str(repo_root / "config" / "game.toml"),
    ]
    print(f"[recomp_black] Executando: {' '.join(cmd)}")
    proc = subprocess.run(cmd, cwd=repo_root)
    return proc.returncode


def export_pe(repo_root: Path) -> int:
    """Exporta um PE plano com símbolos (sub_XXXXXXXX) via sdk/tools/symbols/xbe2pe.py para uso no Ghidra (Fase 1)."""
    xbe_path = _ensure_real_xbe(repo_root)
    if xbe_path is None:
        return 1

    out_pe = repo_root / "build" / "black_flat.pe.exe"
    out_pe.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable,
        str(repo_root / "sdk" / "tools" / "symbols" / "xbe2pe.py"),
        str(repo_root / "config" / "game.toml"),
        str(out_pe),
    ]
    print(f"[recomp_black] Exportando PE plano para Ghidra: {out_pe}")
    proc = subprocess.run(cmd, cwd=repo_root)
    return proc.returncode


def run_recomp(repo_root: Path) -> int:
    xbe_path = _ensure_real_xbe(repo_root)
    if xbe_path is None:
        return 1

    recomp_exe, _ = build_toolchain(repo_root)
    cmd = [str(recomp_exe), str(repo_root / "config" / "game.toml"), "--xdk", str(repo_root / "sdk" / "xdk")]
    print(f"[recomp_black] Executando: {' '.join(cmd)}")
    proc = subprocess.run(cmd, cwd=repo_root)
    return proc.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepara e executa o recompilador XboxXBE-Recomp para BLACK.")
    parser.add_argument(
        "--check-toolchain",
        action="store_true",
        help="Compila/verifica xbe_recomp.exe + xbe_symbols.exe e valida config/game.toml + sdk/xdk/5849/layout.toml.",
    )
    parser.add_argument(
        "--run-symbols",
        action="store_true",
        help="Executa xbe_symbols.exe sobre assets/default.xbe para gerar config/game.toml (Fase 0).",
    )
    parser.add_argument(
        "--export-pe",
        action="store_true",
        help="Exporta build/black_flat.pe.exe via sdk/tools/symbols/xbe2pe.py para inspeção no Ghidra (Fase 1).",
    )
    parser.add_argument("--rebuild-tools", action="store_true", help="Força recompilação de xbe_recomp e xbe_symbols.")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    try:
        if args.rebuild_tools:
            build_toolchain(repo_root, force=True)
        if args.check_toolchain:
            return check_toolchain(repo_root)
        if args.run_symbols:
            return run_symbols(repo_root)
        if args.export_pe:
            return export_pe(repo_root)
        return run_recomp(repo_root)
    except Exception as exc:
        print(f"[recomp_black] ERRO: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
