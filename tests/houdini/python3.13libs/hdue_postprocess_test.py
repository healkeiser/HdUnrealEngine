"""hdUE Render Settings post-process parameters in the real Solaris viewport: a setting set back to its default
(Revert to Defaults, or typed) renders as it did before it was ever changed.
"""

import hdue_houdini as integration
import numpy as np
from hdue_houdini_test import Test as ViewportTest

_test = None
TEMP = "xn__hdUEpostProcessWhiteTemp_3fbl"


class Test(ViewportTest):
    def run(self):
        import hdefereval

        viewer = integration.scene_viewer()
        for _ in range(24):
            yield
        hdefereval.execute_deferred(viewer.curViewport().frameAll)
        for _ in range(8):
            yield
        integration.set_render_setting("xn__hdUEtonemapper", 1)
        node = viewer.pwd().displayNode()
        while node.type().name() != integration.RENDER_SETTINGS_TYPES[1]:
            node = node.inputs()[0]
        temp = node.parm(TEMP)

        def render(label):
            previous = self.frames[-1]["generation"] if self.frames else 0
            rgb = yield from self.image(label, 0, previous)
            mean = rgb.reshape(-1, 3).mean(axis=0)
            self.frames[-1]["mean_rgb"] = mean.tolist()
            return mean

        base = yield from render("01-default")
        temp.set(3000)
        warm = yield from render("02-temp-3000")
        if np.abs(warm - base).max() < 0.005:
            raise AssertionError(
                f"WhiteTemp 3000 did not change the image: {base} {warm}"
            )
        temp.revertToDefaults()
        reverted = yield from render("03-reverted")
        temp.set(3000)
        yield from render("04-temp-3000-again")
        temp.set(6500)
        typed = yield from render("05-typed-6500")
        for label, mean in (("reverted", reverted), ("typed 6500", typed)):
            if np.abs(mean - base).max() > 0.005:
                raise AssertionError(
                    f"WhiteTemp {label} differs from the default: {base} {mean}"
                )


def start(host):
    global _test
    _test = Test(host)
