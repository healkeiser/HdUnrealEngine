"""In-Houdini test entry: Houdini runs every pythonrc.py on HOUDINI_PATH, and tests/houdini is on it for test runs
only (Launch-Houdini.ps1 -TestKind). Starts the hdUE host and then the test named by HDUE_HOUDINI_TEST.
"""

import os

if os.environ.get("HDUE_HOUDINI_TEST"):
    import hdefereval
    import hdue_tests

    hdefereval.execute_deferred(hdue_tests.run)
