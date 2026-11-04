"""Prepared materials in the order every host applies them (Houdini's viewport, husk, usdview)."""


class NeedsPreparation(ValueError):
    """A material is valid but has no prepared native material yet (a Substrate Builder graph, or a MaterialX graph
    hdUE cannot translate at render time): the host prepares it (live_materials.LivePreparation) or reports it.
    """


SETTINGS = (
    "nativeMaterialAssets",
    "nativeMaterialParameters",
    "surfaceEffects",
)


def sync(stage, time, materialx, substrate, view):
    from cooperative import run

    return run(sync_steps(stage, time, materialx, substrate, view))


def sync_steps(stage, time, materialx, substrate, view):
    """The MaterialX library first (MaterialXLibrary.sync writes its assets, parameters and wet bindings into view's
    renderer settings), else the wet bindings alone; then the Substrate Builder library over them. Returns the three
    settings for the host to apply. Raises ValueError with the library's message (substrate_library.NeedsPreparation
    for an unprepared or changed Substrate graph).
    """
    from surface_effects import resolve_steps

    entries = {}
    if materialx:
        if not (yield from materialx.sync_steps()):
            raise materialx.preparation_exception
        entries = materialx.bound_entries
    settings = {name: view.GetRendererSetting(name) or {} for name in SETTINGS}
    if not materialx:
        settings["surfaceEffects"], _ = yield from resolve_steps(
            stage, {}, time
        )
    yield from substrate.sync_steps(stage, time, entries, settings)
    return settings
