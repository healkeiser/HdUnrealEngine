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
    """The MaterialX library first (MaterialXLibrary.sync writes its assets, parameters and wet bindings into view's
    renderer settings), else the wet bindings alone; then the Substrate Builder library over them. Returns the three
    settings for the host to apply. Raises ValueError with the library's message (substrate_library.NeedsPreparation
    for an unprepared or changed Substrate graph).
    """
    from surface_effects import resolve

    entries = {}
    if materialx:
        if not materialx.sync():
            raise materialx.preparation_exception
        entries = materialx.bound_entries
    settings = {name: view.GetRendererSetting(name) or {} for name in SETTINGS}
    if not materialx:
        settings["surfaceEffects"], _ = resolve(stage, {}, time)
    substrate.sync(stage, time, entries, settings)
    return settings
