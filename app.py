#!/usr/bin/env python3
"""Start the ESP32 relay after loading local env files."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent
SERVER_DIR = ROOT / "server"


def parse_env_line(line: str) -> tuple[str, str] | None:
    line = line.strip()
    if not line or line.startswith("#") or "=" not in line:
        return None
    key, value = line.split("=", 1)
    key = key.strip()
    value = value.strip().strip('"').strip("'")
    if not key:
        return None
    return key, value


def load_env_file(path: Path) -> None:
    if not path.exists():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        parsed = parse_env_line(line)
        if not parsed:
            continue
        key, value = parsed
        os.environ.setdefault(key, value)


def load_env() -> None:
    load_env_file(ROOT / ".env")
    load_env_file(SERVER_DIR / ".env")


def main() -> int:
    load_env()
    port = os.environ.get("PORT", "8080")
    python_bin = os.environ.get("PYTHON_BIN") or sys.executable
    os.environ["PYTHON_BIN"] = python_bin

    print(f"Krit AI env loaded. Starting ESP32 relay on port {port}.")
    proc = subprocess.Popen(["node", "server.js"], cwd=SERVER_DIR, env=os.environ.copy())

    def stop_child(_signum, _frame):
        proc.terminate()

    signal.signal(signal.SIGINT, stop_child)
    signal.signal(signal.SIGTERM, stop_child)
    return proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
