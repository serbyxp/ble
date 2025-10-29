"""Ensure intelhex is available for PlatformIO builds."""
from __future__ import annotations

import subprocess
import sys


def ensure_intelhex() -> None:
    try:
        import intelhex  # noqa: F401
    except ImportError:
        subprocess.check_call([sys.executable, "-m", "pip", "install", "intelhex"])


ensure_intelhex()
