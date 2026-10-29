"""Reuse the checked cooked MaterialX registry in a Solaris viewport."""
import hashlib
from pathlib import Path
import hou
from hdue_op_textures import opdef_stamp
from materialx_controls import MaterialXLibrary,OfflineView


class PreparedMaterials(OfflineView):
    def __init__(self,live=None):
        super().__init__()
        self.file=None
        self.library=None
        self.embedded={}; self.checked={}
        self.applied=None
        self.substrate=None
        self.substrate_file=None
        self.published_error=None
        self.live=live; self.substrate_base=[]

    def publish_error(self,session,api,message):
        self.applied=None
        key=(session,message)
        if key!=self.published_error and api.surfaces(session,{},error=message):
            self.published_error=key

    def sync(self,stage,time,file,session,api,substrate_file=None):
        identity=None
        if file:
            path=Path(hou.expandString(file)).resolve()
            identity=(str(path),path.stat().st_mtime_ns)
        if identity!=self.file:
            self.file=None
            self.library=None
            self.embedded={}; self.checked={}
            self.settings={}
            if identity:
                self.library=MaterialXLibrary(path,self)
                # Original embedded resources must still match the cooked images.
                import json
                data=json.loads(path.read_text(encoding='utf-8-sig'))
                self.embedded=data.get('embedded_textures',{})
            self.file=identity
        for uri,entry in self.embedded.items():
            # Hashed again only when the HDA holding it changes: this runs on every frame change.
            stamp=opdef_stamp(uri)
            if self.checked.get(uri)==stamp: continue
            if hashlib.sha256(hou.readBinaryFile(uri)).hexdigest()!=entry['sha256']:
                raise ValueError('Embedded texture changed; prepare/import/cook again: '+uri)
            self.checked[uri]=stamp
        self._dataModel.stage=stage; self._dataModel.currentFrame=time
        self._renderer=session
        if self.library:
            self.library.invalidate()
            if not self.library.sync():
                self.publish_error(session,api,self.library.preparation_error)
                raise ValueError(self.library.preparation_error)
        else:
            from surface_effects import resolve
            try: self.settings['surfaceEffects'],_=resolve(stage,{},time)
            except ValueError as error:
                self.publish_error(session,api,str(error))
                raise
        from substrate_library import SubstrateLibrary
        substrate_identity=None
        if substrate_file:
            path=Path(hou.expandString(substrate_file)).resolve()
            substrate_identity=(str(path),path.stat().st_mtime_ns)
        if self.substrate is None or substrate_identity!=self.substrate_file:
            previous=self.substrate.owned if self.substrate else set()
            self.substrate=SubstrateLibrary(path if substrate_identity else None)
            self.substrate_base=list(self.substrate.materials)
            self.substrate.owned=previous; self.substrate_file=substrate_identity; self.applied=None
        if self.live:
            self.live.tick()
            self.substrate.materials=self.substrate_base+list(self.live.entries.values())
        try:
            self.substrate.sync(stage,time,self.library.bound_entries if self.library else {},self.settings)
            if self.live: self.live.wanted=None
        except ValueError as error:
            from substrate_library import NeedsPreparation
            message=str(error)
            if self.live and isinstance(error,NeedsPreparation):
                from prepare_substrate import collect
                records=collect(stage,time,self.library.bound_entries if self.library else {})
                message=self.live.request(records,self.substrate.materials) or message
            self.publish_error(session,api,message)
            raise ValueError(message) from error
        assets=self.settings.get('nativeMaterialAssets',{})
        overrides=self.settings.get('nativeMaterialParameters',{})
        effects=self.settings.get('surfaceEffects',{})
        preview=bool(self.settings.get('wetMaskPreview',False))
        current=(session,assets,overrides,effects,preview)
        if current!=self.applied and api.materials(session,assets,overrides) and api.surfaces(session,effects,preview):
            self.applied=current
            self.published_error=None
            return True
        return False
