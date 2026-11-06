# COP and embedded textures

Shader file inputs accept absolute Houdini node paths such as `op:/obj/geo1/copnet1/OUT[0]`. The suffix selects a Copernicus output port; the default is zero.

The scene keeps its `op:` paths. Only Houdini can evaluate them, so the host exports each image the displayed stage's shaders and lights use to an EXR, outside LOP cooks, and passes the delegate the file for each path (`hdue_set_viewport_textures` in `include/hdue/viewport_api.h`); materials and lights then re-read their textures. Dirty and time-dependent outputs are checked on each host tick. Changed images receive immutable content-hash paths and reset Path Tracer accumulation. Unchanged images are reused. Cache files and the helper ROP are removed on shutdown. A path the host has not exported yet renders as untextured. husk does not evaluate `op:` paths (it has no node to cook): it renders them untextured with a warning naming each path, so save COP images to files for husk.

Images are exported as 32-bit float without colour conversion; colour inputs are then converted from the OCIO `scene_linear` role to linear Rec.709 as for any other EXR, and `raw` inputs are left as they are. Legacy COP color-plane export requires integer frames.

MaterialX Image nodes and UsdUVTexture read these textures in every input hdUE translates (see [MaterialX surfaces](materialx-surfaces.md)). `<UDIM>` file patterns are read at runtime ([Normal maps and UDIMs](../materials.md#normal-maps-and-udims)). COP UDIM collections, alpha-to-opacity wiring, and texture replacement inside cooked native materials are not implemented.

Embedded `opdef:` images are also supported. Houdini extracts the original image bytes into immutable content-hash files without changing the source definition or USD asset. MaterialX preparation automatically extracts these images when `houdini_root` is configured; `Import-UsdMaterialX.ps1 -HoudiniRoot` can select it explicitly.

```powershell
.\scripts\Test-HoudiniOpTextures.ps1
```

Light `op:` and `opdef:` texture inputs use the same export and cache path as material images.

For husk, Houdini's USD ROP and USD Render ROP write each `op:` image to disk when they save the stage (tiled, mip-mapped EXRs next to the USD, the paths rewritten to them), as for Karma: the saved stage holds ordinary files, which husk renders like any texture. Checked with a COP texture exported by the USD ROP: husk renders its colour. A stage that still holds `op:` paths (saved without the ROP) has no Houdini to evaluate them in husk; hdUE warns and renders without the image.
