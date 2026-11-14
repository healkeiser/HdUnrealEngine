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

    def installed(variable, relative):
        root = os.environ.get(variable, "")
        # A relative path would resolve against the current directory.
        return [Path(root) / relative] if Path(root).is_absolute() else []

    found = shutil.which("pwsh")
    on_path = (
        [Path(found)]
        if found
        and Path(found).is_absolute()
        and Path(found).resolve().parent != Path.cwd().resolve()
        else []
    )
    candidates = (
        installed("ProgramFiles", "PowerShell/7/pwsh.exe")
        + on_path
        + installed(
            "SystemRoot", "System32/WindowsPowerShell/v1.0/powershell.exe"
        )
    )
    return next((str(c) for c in candidates if c.is_file()), None)
