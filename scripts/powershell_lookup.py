"""Finds the PowerShell that child scripts run with (no USD needed)."""

import os
import shutil
from pathlib import Path


def powershell_executable():
    """Returns the PowerShell to run child scripts with, or None.

    Only known install folders and PATH entries outside the current directory
    count: shutil.which on Windows searches the current directory first, and
    husk's current directory is often the scene's folder.
    """
    candidates = [
        Path(os.environ.get("ProgramFiles", "")) / "PowerShell/7/pwsh.exe"
    ]
    found = shutil.which("pwsh")
    if found and Path(found).resolve().parent != Path.cwd().resolve():
        candidates.append(Path(found))
    candidates.append(
        Path(os.environ.get("SystemRoot", ""))
        / "System32/WindowsPowerShell/v1.0/powershell.exe"
    )
    return next((str(c) for c in candidates if c.is_file()), None)
