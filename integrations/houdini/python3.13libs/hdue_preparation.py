"""Prepare the displayed LOP output, including references added after settings."""
import hou
from hdue_op_textures import cache,refresh

TAG='hdue:viewportPreparation'

class ViewportPreparation:
    def __init__(self):
        self.node=None
        self.source=None
        self.stamp=None
        self.revision=0

    def close(self):
        with hou.undos.disabler():
            self._close()

    def _close(self):
        node=self.node; self.node=None; self.stamp=None
        if node is None: return
        try:
            source=node.inputs()[0] if node.inputs() else None
            if source and node.isDisplayFlagSet(): source.setDisplayFlag(True)
            # A user can append an Edit LOP to the generated output. Preserve
            # that connection when moving preparation to the new final output.
            for child in node.outputs():
                for index,input_ in enumerate(child.inputs()):
                    if input_==node: child.setInput(index,source)
            node.destroy()
        except hou.ObjectWasDeleted:
            pass
        self.source=None

    def ensure(self,source,controls):
        with hou.undos.disabler():
            return self._ensure(source,controls)

    def _ensure(self,source,controls):
        if source is None: return None
        if self.node is None and source.userData(TAG)=='1':
            self.node=source; self.source=source.inputs()[0] if source.inputs() else None
        if source==self.node:
            self.refresh()
            return self.node
        self.close()
        # Render settings only author /Render; the prepared stage above them is already complete.
        upstream=source
        while upstream and upstream!=controls and upstream.type().name()=='rendersettings':
            upstream=upstream.inputs()[0] if upstream.inputs() else None
        if upstream==controls: return None
        self.source=source
        node=source.parent().createNode('pythonscript','hdUE_viewport_prepare')
        self.node=node
        node.setUserData(TAG,'1'); node.setInput(0,source)
        node.parm('python').set('import hdue_houdini\nhdue_houdini.prepare_viewport(hou.pwd())')
        node.setPosition(source.position()+hou.Vector2(0,-1))
        self.refresh(force=True)
        node.setDisplayFlag(True)
        return node

    def refresh(self,force=False):
        if self.node is None: return
        source=self.node.inputs()[0] if self.node.inputs() else None
        if source is None: raise ValueError('The hdUE viewport preparation node needs an input LOP.')
        stamp=(source.sessionId(),source.cookCount(),hou.frame() if source.isTimeDependent() else None)
        if force or source.needsToCook() or stamp!=self.stamp or cache().dirty(self.node):
            refresh(self.node)
            errors=self.node.errors()
            if errors: raise ValueError('; '.join(errors))
            self.stamp=(source.sessionId(),source.cookCount(),hou.frame() if source.isTimeDependent() else None)
            self.revision+=1
