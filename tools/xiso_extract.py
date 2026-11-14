#!/usr/bin/env python3
"""Extrator de imagens de disco Xbox Original (XISO / Redump ISO).

Implementa o mesmo formato lido por `sdk/runtime/kernel/disc.cpp`:
  - Partição de jogo em 0x0 (XISO pura) ou 0x18300000 (Redump DVD completa)
  - Volume descriptor no setor 32 (offset +0x10000) com assinatura `MICROSOFT*XBOX*MEDIA`
  - Árvore binária de entradas XDVDFS para `default.xbe`, `GlobData.bin`, `chars/`, `levels/`, etc.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path
from typing import BinaryIO

SECTOR_SIZE = 2048
VOLUME_OFFSET = 32 * SECTOR_SIZE
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITION_OFFSETS = (0, 0x18300000, 0x0FDA0000)
ATTR_DIR = 0x10
MAX_TABLE_BYTES = 16 * 1024 * 1024


def _find_partition_base(fp: BinaryIO, file_size: int) -> tuple[int, int, int] | None:
    for base in PARTITION_OFFSETS:
        vol_pos = base + VOLUME_OFFSET
        if vol_pos + SECTOR_SIZE > file_size:
            continue
        fp.seek(vol_pos)
        vol = fp.read(SECTOR_SIZE)
        if len(vol) < SECTOR_SIZE:
            continue
        if vol[: len(MAGIC)] == MAGIC and vol[0x7EC : 0x7EC + len(MAGIC)] == MAGIC:
            root_sector, root_size = struct.unpack_from("<II", vol, 0x14)
            return base, root_sector, root_size
    return None


def _safe_name(raw_name: bytes) -> str | None:
    name = raw_name.decode("latin-1", errors="replace").strip("\x00")
    if not name or name in (".", "..") or "/" in name or "\\" in name or ":" in name:
        return None
    return name


def _extract_dir(
    fp: BinaryIO,
    base: int,
    dir_sector: int,
    dir_size: int,
    out_dir: Path,
    depth: int = 0,
    counters: list[int] | None = None,
) -> None:
    if dir_size == 0 or depth > 32 or dir_size > MAX_TABLE_BYTES:
        return
    if counters is None:
        counters = [0, 0]

    fp.seek(base + dir_sector * SECTOR_SIZE)
    table = fp.read(dir_size)
    if len(table) < dir_size:
        raise ValueError(f"Tabela de diretório truncada no setor {dir_sector}")

    out_dir.mkdir(parents=True, exist_ok=True)
    seen: set[int] = set()
    stack: list[int] = [0]

    while stack:
        off = stack.pop()
        if off in seen or off + 14 > dir_size:
            continue
        seen.add(off)

        left, right, sector, size, attr, name_len = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF:
            continue
        if off + 14 + name_len > dir_size:
            continue

        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)

        name = _safe_name(table[off + 14 : off + 14 + name_len])
        if not name:
            continue

        target = out_dir / name
        if attr & ATTR_DIR:
            _extract_dir(fp, base, sector, size, target, depth + 1, counters)
        else:
            fp.seek(base + sector * SECTOR_SIZE)
            remaining = size
            with open(target, "wb") as out_f:
                while remaining > 0:
                    chunk = fp.read(min(1024 * 1024, remaining))
                    if not chunk:
                        break
                    out_f.write(chunk)
                    remaining -= len(chunk)
            counters[0] += 1
            counters[1] += size


def extract_iso(iso_path: Path, out_dir: Path) -> int:
    if not iso_path.is_file():
        raise FileNotFoundError(f"ISO não encontrada: {iso_path}")

    file_size = iso_path.stat().st_size
    with open(iso_path, "rb") as fp:
        found = _find_partition_base(fp, file_size)
        if not found:
            raise ValueError(
                f"{iso_path.name} não possui assinatura Xbox XISO (MICROSOFT*XBOX*MEDIA) nos offsets 0x0 ou 0x18300000."
            )
        base, root_sector, root_size = found
        counters = [0, 0]
        _extract_dir(fp, base, root_sector, root_size, out_dir, 0, counters)

    xbe = out_dir / "default.xbe"
    if xbe.is_file():
        mb = counters[1] / (1024 * 1024)
        print(f"[xiso_extract] Extraídos {counters[0]} arquivo(s) ({mb:.1f} MB) de {iso_path.name} para {out_dir}")
    return counters[0]


def main() -> int:
    parser = argparse.ArgumentParser(description="Extrai arquivos de uma ISO do Xbox Original para assets/.")
    parser.add_argument("iso", type=Path, help="Caminho do arquivo .iso do jogo BLACK (Xbox)")
    parser.add_argument(
        "out",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "assets",
        help="Pasta de destino (padrão: assets/)",
    )
    args = parser.parse_args()

    try:
        extract_iso(args.iso, args.out)
    except Exception as exc:
        print(f"[xiso_extract] ERRO: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
