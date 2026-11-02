"""hdUE Render Settings post-process parameters in the real Solaris viewport: a setting set back to its default
(Revert to Defaults, or typed) renders as it did before it was ever changed; an hdUE Post Process Volume setting turned
on renders as the same render setting, and turned off again as before.
"""

import hdue_houdini as integration
import hou
import numpy as np
from hdue_houdini_test import Test as ViewportTest

_test = None
TEMP = "xn__unrealpostProcessWhiteTemp_fjbl"


class Test(ViewportTest):
    def run(self):
        import hdefereval

        viewer = integration.scene_viewer()
        for _ in range(24):
            yield
        hdefereval.execute_deferred(viewer.curViewport().frameAll)
        for _ in range(8):
            yield
        integration.set_render_setting("xn__unrealtonemapper", 1)
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
        # An hdUE Post Process Volume (unbound: wherever the viewport camera is) after the displayed node. Its settings
        # start at Do Nothing; turning one on adds the attribute to the existing prim, which must reach the viewport,
        # and turning it off again removes it.
        display = viewer.pwd().displayNode()
        volume = display.parent().createNode(
            "hdue::hduepostprocessvolume::1.0", "post_process_volume"
        )
        volume.setInput(0, display)
        volume.parm(hou.text.encode("unreal:postProcessVolume:bUnbound")).set(1)
        volume.setDisplayFlag(True)
        empty = yield from render("06-volume-no-settings")
        volume.parm(
            hou.text.encode("unreal:postProcess:WhiteTemp_control")
        ).set("set")
        volume.parm(hou.text.encode("unreal:postProcess:WhiteTemp")).set(3000)
        volume_warm = yield from render("07-volume-temp-3000")
        volume.parm(
            hou.text.encode("unreal:postProcess:WhiteTemp_control")
        ).set("none")
        volume_off = yield from render("08-volume-setting-off")
        if (
            np.abs(empty - base).max() > 0.005
            or np.abs(volume_off - base).max() > 0.005
        ):
            raise AssertionError(
                f"A volume without settings changed the image: {base} {empty} {volume_off}"
            )
        if np.abs(volume_warm - warm).max() > 0.01:
            raise AssertionError(
                f"The volume's WhiteTemp 3000 is not the render setting's: {warm} {volume_warm}"
            )


def start(host):
    global _test
    _test = Test(host)
