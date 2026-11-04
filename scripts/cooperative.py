"""Stage walks as cooperative jobs, so Houdini's UI thread is never held by one.

A walk is a generator: it yields between units of work (a prim, a material) and returns its result. Batch hosts (husk,
usdview, tests) run it to completion with run(); Houdini's host advances a Job for a few milliseconds per timer tick
(hdue_houdini.Host), as a renderer keeps its own work off the UI thread and the viewport stays interactive. One
implementation serves every host: the walks are the same code either way.
"""

import time


def run(steps):
    """Run a walk to completion; its return value."""
    try:
        while True:
            next(steps)
    except StopIteration as done:
        return done.value


class Job:
    """A walk advanced in time slices. advance() runs it for at most budget seconds and returns True once it has
    finished; result then holds its return value. An exception from the walk propagates from advance(), and the job
    is finished.
    """

    def __init__(self, steps, label=""):
        self.steps = steps
        self.label = label
        self.done = False
        self.result = None
        self.started = time.perf_counter()
        self.units = 0  # yields so far: the host's progress message

    def advance(self, budget):
        if self.done:
            return True
        deadline = time.perf_counter() + budget
        try:
            while True:
                next(self.steps)
                self.units += 1
                if time.perf_counter() >= deadline:
                    return False
        except StopIteration as finished:
            self.done, self.result = True, finished.value
            return True
        except BaseException:
            self.done = True
            raise

    def elapsed(self):
        return time.perf_counter() - self.started

    def close(self):
        """Abandon the walk (a newer one replaces it)."""
        self.done = True
        self.steps.close()
