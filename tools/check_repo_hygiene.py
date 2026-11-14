#!/usr/bin/env python3
"""Verificação de higiene do repositório (Seção 5 do plano).

Garante que nenhum arquivo do jogo (.xbe, .iso, .bin, assets/), código gerado
pela recompilação (generated/), binários compilados (.exe, .dll, .pdb) ou chaves/segredos
entre no controle de versão.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import PurePosixPath

FORBIDDEN_EXTENSIONS = {
    ".xbe",
    ".iso",
    ".bin",
    ".exe",
    ".dll",
    ".lib",
    ".pdb",
    ".dmp",
    ".wasm",
    ".key",
    ".pem",
}

FORBIDDEN_PREFIXES = (
    "generated/",
    "out/",
    "build/",
    "hdd/",
    "CrashDumps/",
    "logs/",
)

ALLOWED_IN_ASSETS = {
    "assets/.gitkeep",
    "assets/README.md",
}


def git_tracked_and_staged_files() -> list[str]:
    out_cached = subprocess.check_output(
        ["git", "ls-files", "--cached"], text=True, encoding="utf-8"
    )
    files = {line.strip() for line in out_cached.splitlines() if line.strip()}
    return sorted(files)


def check_file(path_str: str) -> str | None:
    p = PurePosixPath(path_str.replace("\\", "/"))
    norm = p.as_posix()

    if norm.startswith("assets/") and norm not in ALLOWED_IN_ASSETS:
        return f"arquivo proibido dentro de assets/: {norm}"

    for prefix in FORBIDDEN_PREFIXES:
        if norm.startswith(prefix):
            return f"caminho em diretório proibido ({prefix}): {norm}"

    ext = p.suffix.lower()
    if ext in FORBIDDEN_EXTENSIONS:
        return f"extensão proibida ({ext}): {norm}"

    if p.name.lower() in {"coop-online.toml", ".env"}:
        return f"arquivo de configuração sensível/local: {norm}"

    return None


def main() -> int:
    try:
        files = git_tracked_and_staged_files()
    except Exception as exc:
        print(f"[hygiene] erro ao listar arquivos do git: {exc}", file=sys.stderr)
        return 2

    violations: list[str] = []
    for f in files:
        reason = check_file(f)
        if reason:
            violations.append(reason)

    if violations:
        print("[hygiene] FALHA — arquivos proibidos detectados no repositório:", file=sys.stderr)
        for v in violations:
            print(f"  - {v}", file=sys.stderr)
        return 1

    print(f"[hygiene] OK ({len(files)} arquivos rastreados verificados; zero assets, binários ou código gerado).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
