#!/usr/bin/env python3
"""Instala o hook pre-commit de higiene do repositório em .git/hooks/pre-commit."""

from pathlib import Path
import shutil
import stat

root = Path(__file__).resolve().parent.parent
src = root / "tools" / "hooks" / "pre-commit"
dst = root / ".git" / "hooks" / "pre-commit"

dst.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(src, dst)
dst.chmod(dst.stat().st_mode | stat.S_IEXEC)
print(f"Pre-commit hook instalado em: {dst}")
