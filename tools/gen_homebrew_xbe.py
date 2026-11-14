#!/usr/bin/env python3
"""Gera localmente na pasta assets/ (ignorada pelo Git) um pacote homebrew/aberto de BLACK
para execução imediata em 1 clique no navegador (estilo nfsu2.xyz).

Gera:
  - assets/default.xbe (cabeçalho XBEH válido, Title ID 0x45410083, validável pelo OPFS Loader)
  - assets/GlobData.bin (tabela sintética de parâmetros da campanha)
  - assets/levels/treneska/mission.json (dados completos dos 3 setores da Missão 01: Treneska)

Nenhum byte protegido da EA/Criterion é distribuído ou versionado neste repositório.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

XBE_MAGIC = 0x48454258  # "XBEH"
BASE_ADDR = 0x00010000
CERT_OFFSET = 0x0180
CERT_ADDR = BASE_ADDR + CERT_OFFSET
TITLE_ID = 0x45410083  # BLACK (NTSC-U)


def build_homebrew_xbe() -> bytes:
    buf = bytearray(16384)
    # Assinatura XBEH
    struct.pack_into("<I", buf, 0x000, XBE_MAGIC)
    # BaseAddress (0x104), HeadersSize (0x108), ImageSize (0x10C)
    struct.pack_into("<III", buf, 0x104, BASE_ADDR, 0x0400, 0x4000)
    # CertAddress (0x118), SectionCount (0x11C), SectionHeaders (0x120)
    struct.pack_into("<III", buf, 0x118, CERT_ADDR, 1, BASE_ADDR + 0x02B0)
    # InitFlags (0x124), EntryPoint (0x128)
    struct.pack_into("<II", buf, 0x124, 0x00000001, 0x00011000)

    # XBE Certificate em +0x180
    struct.pack_into("<III", buf, CERT_OFFSET, 0x00DC, 0x20060224, TITLE_ID)
    title_utf16 = "BLACK Enhanced Open Homebrew".encode("utf-16-le")
    buf[CERT_OFFSET + 0x0C : CERT_OFFSET + 0x0C + len(title_utf16)] = title_utf16

    # Marker na área de código +0x1000 (ret / nop stub)
    buf[0x1000 : 0x1004] = b"\x90\x90\xC3\x90"
    return bytes(buf)


def build_glob_data() -> bytes:
    # Cabeçalho sintético: magic "GLBD", versão 1, 3 armas base
    header = struct.pack("<4sIIII", b"GLBD", 1, 3, 30, 100)
    payload = header + b"BLACK_ENHANCED_OPEN_CAMPAIGN_DATA\x00"
    return payload.ljust(1024, b"\x00")


def build_treneska_mission() -> dict:
    # Arena 32x32 em coordenadas de grade (1 unidade = 1 metro aproximado)
    # Paredes: '#' = concreto, 'B' = bunker reforçado, 'C' = container metálico
    grid_rows = [
        "################################",
        "#......B.......#...............#",
        "#..............#...CCC....B....#",
        "#..CC..................CC......#",
        "#..........BB..................#",
        "#..............#.......BB......#",
        "#......##......#...............#",
        "####..#########B#####....#######",
        "#..............................#",
        "#...CC.........BB........CC....#",
        "#..............................#",
        "#.......B..............B.......#",
        "#..............##..............#",
        "########....#######...##########",
        "#..............................#",
        "#....BB......CCCCCC......BB....#",
        "#..............................#",
        "#.......#..............#.......#",
        "#.......#......B.......#.......#",
        "#..............................#",
        "################################",
    ]
    return {
        "id": "treneska",
        "title": "Missao 01: Treneska Border Crossing",
        "subtitle": "Setores Alpha / Bravo / Charlie",
        "spawn": {"x": 2.5, "y": 2.5, "yaw": 0.35},
        "mapWidth": 32,
        "mapHeight": len(grid_rows),
        "grid": grid_rows,
        "sectors": [
            {
                "id": 1,
                "name": "SETOR ALPHA // POSTO DA FRONTEIRA",
                "objective": "Elimine a patrulha e destrua o bloqueio do portao",
                "barrels": [
                    {"x": 8.5, "y": 3.5},
                    {"x": 12.2, "y": 5.2},
                    {"x": 21.5, "y": 4.5},
                ],
                "crates": [
                    {"x": 5.5, "y": 2.5, "ammo": True},
                    {"x": 17.5, "y": 2.2, "ammo": False},
                    {"x": 26.5, "y": 5.2, "ammo": True},
                ],
                "enemies": [
                    {"x": 9.5, "y": 2.5, "hp": 80, "kind": "rifle", "patrol": [[8.5, 2.5], [12.5, 2.5]]},
                    {"x": 13.5, "y": 4.8, "hp": 90, "kind": "rifle", "patrol": [[11.5, 4.8], [14.0, 2.5]]},
                    {"x": 20.5, "y": 3.5, "hp": 110, "kind": "shotgun", "patrol": [[18.5, 3.5], [23.5, 5.5]]},
                    {"x": 27.5, "y": 3.2, "hp": 100, "kind": "sniper", "patrol": [[26.5, 2.2], [28.5, 5.5]]},
                ],
            },
            {
                "id": 2,
                "name": "SETOR BRAVO // PATIO DE CONTEINERES",
                "objective": "Avance pelo patio de carga e neutraliza os atiradores",
                "barrels": [
                    {"x": 7.5, "y": 9.5},
                    {"x": 14.5, "y": 10.5},
                    {"x": 22.5, "y": 9.2},
                ],
                "crates": [
                    {"x": 4.5, "y": 11.5, "ammo": True},
                    {"x": 18.5, "y": 11.2, "ammo": False},
                    {"x": 27.5, "y": 11.0, "ammo": True},
                ],
                "enemies": [
                    {"x": 6.5, "y": 10.2, "hp": 95, "kind": "rifle", "patrol": [[4.5, 9.5], [10.5, 11.5]]},
                    {"x": 13.5, "y": 8.8, "hp": 120, "kind": "shotgun", "patrol": [[11.5, 8.8], [16.5, 11.2]]},
                    {"x": 20.5, "y": 10.5, "hp": 100, "kind": "rifle", "patrol": [[18.5, 9.0], [23.5, 11.5]]},
                    {"x": 26.5, "y": 8.8, "hp": 130, "kind": "heavy", "patrol": [[24.5, 8.8], [28.5, 11.5]]},
                    {"x": 29.0, "y": 10.2, "hp": 90, "kind": "sniper", "patrol": [[28.5, 8.5], [29.5, 12.0]]},
                ],
            },
            {
                "id": 3,
                "name": "SETOR CHARLIE // BUNKER DE COMANDO",
                "objective": "Limpe o bunker central para concluir a operacao",
                "barrels": [
                    {"x": 10.5, "y": 15.5},
                    {"x": 16.0, "y": 16.5},
                    {"x": 21.5, "y": 15.5},
                ],
                "crates": [
                    {"x": 3.5, "y": 16.5, "ammo": True},
                    {"x": 15.5, "y": 19.2, "ammo": True},
                    {"x": 28.5, "y": 16.5, "ammo": True},
                ],
                "enemies": [
                    {"x": 6.5, "y": 15.5, "hp": 110, "kind": "rifle", "patrol": [[4.5, 14.5], [8.5, 18.5]]},
                    {"x": 12.5, "y": 17.5, "hp": 135, "kind": "heavy", "patrol": [[10.5, 16.5], [14.5, 19.2]]},
                    {"x": 19.5, "y": 17.5, "hp": 135, "kind": "heavy", "patrol": [[17.5, 16.5], [21.5, 19.2]]},
                    {"x": 25.5, "y": 15.5, "hp": 120, "kind": "shotgun", "patrol": [[23.5, 14.5], [27.5, 18.5]]},
                    {"x": 16.0, "y": 19.0, "hp": 180, "kind": "commander", "patrol": [[13.5, 19.0], [18.5, 19.0]]},
                ],
            },
        ],
    }


def generate_assets(repo_root: Path) -> Path:
    assets_dir = repo_root / "assets"
    assets_dir.mkdir(parents=True, exist_ok=True)
    (assets_dir / "levels" / "treneska").mkdir(parents=True, exist_ok=True)

    xbe_path = assets_dir / "default.xbe"
    xbe_path.write_bytes(build_homebrew_xbe())

    glob_path = assets_dir / "GlobData.bin"
    glob_path.write_bytes(build_glob_data())

    mission_path = assets_dir / "levels" / "treneska" / "mission.json"
    mission_path.write_text(json.dumps(build_treneska_mission(), indent=2), encoding="utf-8")

    return assets_dir


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    assets_dir = generate_assets(repo_root)
    print(f"[gen_homebrew_xbe] Assets abertos gerados com sucesso em: {assets_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
