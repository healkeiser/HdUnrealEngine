"""Every hdUE dialog script (soho/parameters/*.ds) parses as Houdini parses it when a LOP adds the renderer's tab
(loputils.addDialogScriptFolder): a script that does not parse raises a dialog on every new Light LOP and leaves it
without the Unreal tab.
"""

import sys
from pathlib import Path

import hou

failed = []
for path in sorted(
    (Path(__file__).parents[1] / "integrations/houdini/soho/parameters").glob(
        "*.ds"
    )
):
    group = hou.ParmTemplateGroup()
    try:
        group.setToDialogScript(path.read_text(encoding="utf-8"))
    except hou.OperationFailed as error:
        failed.append(f"{path.name}: {error}")
    else:
        if not group.parmTemplates():
            failed.append(f"{path.name}: no parameters")
if failed:
    sys.exit("\n".join(failed))
print("dialog scripts parse")
