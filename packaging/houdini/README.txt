hdUE for Houdini 22.0 (Windows)
===============================

Unreal Engine 5.8 Hydra render delegate for Solaris, husk and the USD Render ROP.

Install
  1. Put this folder where it will stay, on a drive you can write to
     (UE writes its shader cache and logs into runtime\Windows\HdUEBackend\Saved).
  2. Double-click Install.cmd (or run Install.ps1). It writes
     Documents\houdini22.0\packages\hdUE.json pointing at this folder.
  3. Restart Houdini. Pick "Unreal Engine (hdUE)" in the Solaris viewport
     renderer menu, or as the renderer of a USD Render ROP / husk:
       husk --renderer HdUEHoudiniRendererPlugin -o out.exr scene.usd

Uninstall
  Run Uninstall.cmd, then delete this folder.

Requirements
  - Houdini 22.0 (USD 26.05), Windows 10/11 x64.
  - A DirectX 12 GPU with SM6 (ray tracing for the Path Tracer mode).
  - Microsoft Visual C++ 2015-2022 x64 redistributable (installed with Houdini).

Options
  Install.ps1 -PackagesDirectory <dir>   write hdUE.json somewhere else
                                         (for example a studio HOUDINI_PACKAGE_DIR).
