"""Reuse the checked cooked MaterialX registry in a Solaris viewport."""

import hashlib
from pathlib import Path

import hou
from hdue_op_textures import opdef_stamp
from materialx_controls import MaterialXLibrary, OfflineView


class PreparedMaterials(OfflineView):
    def __init__(self, live=None):
        super().__init__()
        self.file = ()  # no library yet: the first sync makes one, with or without a file
        self.library = None
        self.embedded = {}
        self.checked = {}
        self.applied = None
        self.substrate = None
        self.substrate_file = None
        self.published_error = None
        self.live = live
        self.substrate_base = []

    def publish_error(self, session, api, message):
        self.applied = None
        key = (session, message)
        if key != self.published_error and api.surfaces(
            session, {}, error=message
        ):
            self.published_error = key

    def sync(self, stage, time, file, session, api, substrate_file=None):
        from cooperative import run

        return run(
            self.sync_steps(stage, time, file, session, api, substrate_file)
        )

    def sync_steps(self, stage, time, file, session, api, substrate_file=None):
        """sync() as a cooperative walk (cooperative.Job): the host advances it a few milliseconds per timer tick, so
        a large stage (thousands of materials) never holds Houdini's UI thread. Returns whether it applied new
        settings.
        """
        identity = None
        if file:
            path = Path(hou.expandString(file)).resolve()
            identity = (str(path), path.stat().st_mtime_ns)
        if identity != self.file:
            self.file = None
            self.library = None
            self.embedded = {}
            self.checked = {}
            # The host's wet-mask preview toggle is not the library's: it survives a library change.
            self.settings = {
                k: v for k, v in self.settings.items() if k == "wetMaskPreview"
            }
            # Also without a file: it binds the MaterialX materials live preparation made.
            self.library = MaterialXLibrary(path if identity else None, self)
            if identity:
                # Original embedded resources must still match the cooked images.
                import json

                data = json.loads(path.read_text(encoding="utf-8-sig"))
                self.embedded = data.get("embedded_textures", {})
            self.file = identity
        for uri, entry in self.embedded.items():
            # Hashed again only when the HDA holding it changes: this runs on every frame change.
            stamp = opdef_stamp(uri)
            if self.checked.get(uri) == stamp:
                continue
            if (
                hashlib.sha256(hou.readBinaryFile(uri)).hexdigest()
                != entry["sha256"]
            ):
                raise ValueError(
                    "Embedded texture changed; prepare/import/cook again: "
                    + uri
                )
            self.checked[uri] = stamp
        self._dataModel.stage = stage
        self._dataModel.currentFrame = time
        self._renderer = session
        from prepared_materials import sync_steps
        from substrate_library import SubstrateLibrary

        substrate_identity = None
        if substrate_file:
            path = Path(hou.expandString(substrate_file)).resolve()
            substrate_identity = (str(path), path.stat().st_mtime_ns)
        if self.substrate is None or substrate_identity != self.substrate_file:
            previous = self.substrate.owned if self.substrate else set()
            self.substrate = SubstrateLibrary(
                path if substrate_identity else None
            )
            self.substrate_base = list(self.substrate.materials)
            self.substrate.owned = previous
            self.substrate_file = substrate_identity
            self.applied = None
        if self.live:
            self.live.tick()
            self.substrate.materials = (
                self.substrate_base + self.live.substrate_entries()
            )
            self.library.set_prepared(self.live.materialx_entries())
        self.library.invalidate()

        def known():
            return self.substrate.materials + self.live.materialx_entries()

        try:
            self.settings.update(
                (
                    yield from sync_steps(
                        stage, time, self.library, self.substrate, self
                    )
                )
            )
            if self.live:
                # MaterialX graphs hdUE cannot translate at render time: they render translated while UE's importer
                # prepares them in the background.
                from live_materials import materialx_records_steps

                records = yield from materialx_records_steps(
                    self.library.missing, time, self.library.missing_graphs
                )
                if records:
                    self.live.request(records, known())
                else:
                    self.live.wanted = None
        except ValueError as error:
            from substrate_library import NeedsPreparation

            message = str(error)
            if self.live and isinstance(error, NeedsPreparation):
                from live_materials import materialx_records_steps
                from prepare_substrate import collect_steps

                records = (
                    yield from collect_steps(
                        stage, time, self.library.bound_entries
                    )
                ) + (
                    yield from materialx_records_steps(
                        self.library.missing,
                        time,
                        self.library.missing_graphs,
                    )
                )
                message = self.live.request(records, known()) or message
            self.publish_error(session, api, message)
            raise ValueError(message) from error
        assets = self.settings.get("nativeMaterialAssets", {})
        overrides = self.settings.get("nativeMaterialParameters", {})
        effects = self.settings.get("surfaceEffects", {})
        preview = bool(self.settings.get("wetMaskPreview", False))
        current = (session, assets, overrides, effects, preview)
        if (
            current != self.applied
            and api.materials(session, assets, overrides)
            and api.surfaces(session, effects, preview)
        ):
            self.applied = current
            self.published_error = None
            return True
        return False
