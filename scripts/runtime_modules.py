"""Print the scripts/ files the shipped Houdini integration uses: the modules it imports and the PowerShell or Python
scripts they run by name, followed transitively. Package-Houdini.ps1 copies only these."""
from modulefinder import ModuleFinder
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'scripts'
finder = ModuleFinder(path=[str(ROOT / 'integrations/houdini/python3.13libs'), str(SCRIPTS)])
pending, used = sorted((ROOT / 'integrations/houdini').rglob('*.py')), set()
while pending:
    file = pending.pop()
    if file.suffix == '.py':
        finder.run_script(str(file))
    # Run by path: self.root/'scripts/X.ps1' in Python, (Join-Path $PSScriptRoot 'X.py') in PowerShell.
    named = {SCRIPTS / name for name in re.findall(r'(?:PSScriptRoot\W{1,4}|scripts/)([\w-]+\.(?:ps1|py))\b',
                                                   file.read_text(encoding='utf-8-sig'))}
    imported = {Path(module.__file__) for module in finder.modules.values() if module.__file__}
    for path in sorted(named | imported):
        if path.parent == SCRIPTS and path.is_file() and path not in used:
            used.add(path); pending.append(path)
print('\n'.join(sorted(path.name for path in used)))
