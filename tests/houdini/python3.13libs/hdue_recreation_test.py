"""Houdini recreates its Hydra renderer while hdUE's material walk runs, as it does after the first frame of a large
stage (NVIDIA's Da Vinci Workshop): the walk for the old session is dropped instead of applying to a session that is
gone ("viewport materials failed (-4)"), no material error is reported, and the new session gets its materials (its
image matches the one before).
"""

import time

import hdue_houdini as integration
import numpy as np
from hdue_houdini_test import Test as ViewportTest

_test = None


class Test(ViewportTest):
    def run(self):
        import hdefereval

        viewer = integration.scene_viewer()
        host = self.host
        for _ in range(24):
            yield
        hdefereval.execute_deferred(viewer.curViewport().frameAll)
        for _ in range(8):
            yield
        while host.material_job is not None or not host.last_materials:
            yield
        before = yield from self.image("01-before", 0)
        old = self.frames[-1]["session"]
        walk = host.material_steps

        def slow(stage, session):
            # Two seconds of units ahead of the walk: it spans many host ticks (MATERIAL_BUDGET each), as a large
            # stage's does, so the restart lands inside it.
            end = time.monotonic() + 2
            while time.monotonic() < end:
                time.sleep(0.001)
                yield
            return (yield from walk(stage, session))

        host.material_steps = slow
        # Every problem reported, not only the last: the new session's walk clears material_error when it succeeds.
        problems = []
        report = host.material_problem

        def record(message, session):
            problems.append(message)
            report(message, session)

        host.material_problem = record
        # As the first walk of a stage, which has applied nothing yet (a walk that changes nothing applies nothing).
        host.prepared_materials.applied = None
        host.materials_dirty = True
        while not (host.material_job and host.material_job_session == old):
            yield
        viewer.restartRenderer()
        while self.api.status()["session"] in (0, old):
            yield
        new = self.api.status()["session"]
        # The old walk is dropped once the host sees the new session; the next walk is the new session's.
        while host.material_job and host.material_job_session == old:
            yield
        host.material_steps = walk
        while host.material_job is not None or host.last_materials[0] != new:
            yield
        viewer.curViewport().draw()
        after = yield from self.image("02-recreated", 0)
        if self.frames[-1]["session"] != new:
            raise AssertionError(
                f"The image is not the new session's: {self.frames[-1]['session']} {new}"
            )
        host.material_problem = report
        if problems or host.material_error:
            raise AssertionError(
                f"Material errors while the renderer was recreated: {problems} {host.material_error}"
            )
        difference = float(np.abs(after - before).mean())
        self.frames[-1]["mean_difference"] = difference
        if difference > 0.01:
            raise AssertionError(
                f"The new session's image differs from the old one's: {difference}"
            )


def start(host):
    global _test
    _test = Test(host)
