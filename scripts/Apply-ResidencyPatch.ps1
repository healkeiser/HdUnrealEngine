param([string]$EngineRoot = '', [string]$EditorRoot = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$EngineRoot = Get-HdUELocalPath -Key engine_root -Value $EngineRoot
$EditorRoot = Get-HdUELocalPath -Key editor_root -Value $EditorRoot -Optional
$Version = Get-Content -Raw -LiteralPath (Join-Path $EngineRoot 'Engine\Build\Build.version') | ConvertFrom-Json
if ($Version.MajorVersion -ne 5 -or $Version.MinorVersion -ne 8 -or $Version.PatchVersion -ne 4) { throw 'Residency adapter requires UE 5.8.4.' }
# Applies $Edits (pairs of old, new text) to an engine file unless it already contains $Marker. Every old text must occur
# exactly once in the unpatched file; "`n" in an edit becomes the file's own newline. Written as UTF-8 without BOM.
function Edit-EngineSource([string]$Path, [string]$Marker, [object[]]$Edits, [string]$Label) {
    $Source = [IO.File]::ReadAllText($Path)
    if ($Source.Contains($Marker)) { return }
    $Newline = if ($Source.Contains("`r`n")) { "`r`n" } else { "`n" }
    $Edits = @($Edits | ForEach-Object { , @($_[0].Replace("`n", $Newline), $_[1].Replace("`n", $Newline)) })
    foreach ($Edit in $Edits) {
        if (($Source.Split($Edit[0]).Length - 1) -ne 1) { throw "Unexpected $Path; inspect the engine before applying the $Label." }
    }
    foreach ($Edit in $Edits) { $Source = $Source.Replace($Edit[0], $Edit[1]) }
    [IO.File]::WriteAllText($Path, $Source, [Text.UTF8Encoding]::new($false))
    Write-Host "Applied hdUE $Label to $Path."
}
$Anchor = "`tfriend class FRendererModule;"
Edit-EngineSource -Path (Join-Path $EngineRoot 'Engine\Source\Runtime\Renderer\Private\VT\VirtualTextureSystem.h') -Label 'VT residency friend adapter' `
    -Marker "`tfriend class FHdueVirtualTextureResidency;" -Edits @(, @($Anchor, "$Anchor`n`tfriend class FHdueVirtualTextureResidency;"))
# Runtime Nanite builds (Scene.cpp BuildNanite): NaniteBuilder reads displacement texture sources, which exist only in editor builds.
$Open = "`t`t`tif ( IsValid( DisplacementMap.Texture ) && DisplacementMap.Texture->Source.IsValid() )"
$Close = "better to crash then continuing with some false data`n`t`t`t`tcheckNoEntry();`n`t`t`t}"
Edit-EngineSource -Path (Join-Path $EngineRoot 'Engine\Source\Developer\NaniteBuilder\Private\NaniteDisplace.cpp') -Label 'runtime Nanite build patch' `
    -Marker '#if WITH_EDITORONLY_DATA' -Edits @(
    @($Open, "#if WITH_EDITORONLY_DATA`n$Open"),
    @($Close, "$Close`n#endif // WITH_EDITORONLY_DATA (hdUE: texture sources exist only in the editor; runtime builds have no displacement maps)"))
# UE 5.8.4's AmbientCubemapComposite.usf never defines SUBSTRATE_LOAD_FROM_MATERIALCONTAINER, so with Substrate's adaptive
# GBuffer (r.Substrate.ProjectGBufferFormat=1, UE's SM6 default) it reads the legacy GBuffer, which that format does not
# write, and adds nothing. Define it as the other Substrate composites do (ReflectionEnvironmentPixelShader.usf). The
# editor that cooks the runtime compiles its global shaders, so its copy is patched too.
$Anchor = '#define ENABLE_SIMPLE_ENV_LIGHT 1'
$Define = (
    $Anchor, '#if SUBSTRATE_GBUFFER_FORMAT==0', '#define SUBSTRATE_LOAD_FROM_MATERIALCONTAINER 0', '#else',
    '#define SUBSTRATE_LOAD_FROM_MATERIALCONTAINER 1 // hdUE: the adaptive GBuffer reads the material container', '#endif'
) -join "`n"
foreach ($Root in @($EngineRoot, $EditorRoot) | Where-Object { $_ } | Select-Object -Unique) {
    Edit-EngineSource -Path (Join-Path $Root 'Engine\Shaders\Private\AmbientCubemapComposite.usf') -Label 'Ambient Cubemap shader patch' `
        -Marker 'SUBSTRATE_LOAD_FROM_MATERIALCONTAINER 1' -Edits @(, @($Anchor, $Define))
}
# Runtime-built Nanite meshes (Scene.cpp BuildNanite) keep their streaming pages in memory: outside the editor, UE's
# streaming manager reads non-root pages only through the IoDispatcher, with a chunk id only cooked packages have, so
# every page had to be a root page in a pool of at most 2 GB. In non-editor builds, a page of loaded (in-memory) bulk
# data is copied into the staging buffer instead, as the editor's EState::Memory path installs it; cooked bulk data is
# never loaded, so cooked meshes stream from disk as before. Each file has its own marker: a run interrupted between the
# two writes patched the header only.
Edit-EngineSource -Path (Join-Path $EngineRoot 'Engine\Source\Runtime\Engine\Public\Rendering\NaniteStreamingManager.h') `
    -Label 'runtime Nanite streaming patch' -Marker 'bFromMemory' -Edits @(
    @("`t`tFIoBuffer`t`t`t`tRequestBuffer;", "`t`tbool`t`t`t`t`tbFromMemory = false;`t// hdUE: copied from in-memory bulk data`n`t`tFIoBuffer`t`t`t`tRequestBuffer;"),
    @("`t`t`tRequestBuffer = FIoBuffer();", "`t`t`tbFromMemory = false;`n`t`t`tRequestBuffer = FIoBuffer();"))
Edit-EngineSource -Path (Join-Path $EngineRoot 'Engine\Source\Runtime\Engine\Private\Rendering\NaniteStreamingManager.cpp') `
    -Label 'runtime Nanite streaming patch' -Marker 'bFromMemory' -Edits @(
    @('const bool bDiskRequest = true;', 'const bool bDiskRequest = !BulkData.IsBulkDataLoaded();	// hdUE: runtime-built pages are in memory'),
    @("Batch.Read(BulkData, PageStreamingState.BulkOffset, PageStreamingState.BulkSize, AIOP_Low, PendingPage.RequestBuffer, PendingPage.Request);`n`t`t`t`t`t`tbIssueIOBatch = true;",
        (("#if !WITH_EDITOR", "`t`t`t`t`t`tif (!bDiskRequest)`t// hdUE: in-memory bulk data (runtime-built Nanite) has no IO chunk to read", "`t`t`t`t`t`t{",
            "`t`t`t`t`t`t`tconst uint8* BulkDataPtr = (const uint8*)BulkData.LockReadOnly();",
            "`t`t`t`t`t`t`tFMemory::Memcpy(Dst, BulkDataPtr + PageStreamingState.BulkOffset, PageStreamingState.BulkSize);",
            "`t`t`t`t`t`t`tBulkData.Unlock();", "`t`t`t`t`t`t`tPendingPage.bFromMemory = true;", "`t`t`t`t`t`t}", "`t`t`t`t`t`telse", "#endif", "`t`t`t`t`t`t{",
            "`t`t`t`t`t`t`tBatch.Read(BulkData, PageStreamingState.BulkOffset, PageStreamingState.BulkSize, AIOP_Low, PendingPage.RequestBuffer, PendingPage.Request);",
            "`t`t`t`t`t`t`tbIssueIOBatch = true;", "`t`t`t`t`t`t}") -join "`n")),
    @('if (PendingPage.Request.IsCompleted())', 'if (PendingPage.bFromMemory || PendingPage.Request.IsCompleted())'),
    @('if (!PendingPage.Request.IsOk())', 'if (!PendingPage.bFromMemory && !PendingPage.Request.IsOk())'),
    @('check(Task.PendingPage->Request.IsCompleted());', 'check(Task.PendingPage->bFromMemory || Task.PendingPage->Request.IsCompleted());'))
# UE 5.8.4's Nanite AddPass_Rasterize assigns its read-only dummy buffer to the renderer's TotalPrevDrawClustersBuffer
# member when a pass has no previous draw data; a later draw of the same renderer that has previous draw data (Lumen's
# card capture in several Nanite passes, as NVIDIA's Da Vinci Workshop needs) then creates a UAV on that dummy, and D3D12
# removes the device (DXGI_ERROR_INVALID_CALL). The dummy is used through a local instead.
$CullRaster = Join-Path $EngineRoot 'Engine\Source\Runtime\Renderer\Private\Nanite\NaniteCullRaster.cpp'
Edit-EngineSource -Path $CullRaster -Label 'Nanite raster patch' -Marker 'hdUE: a local' -Edits @(
    @("`tif (!bHasPrevDrawData)`n`t{`n`t`tTotalPrevDrawClustersBuffer = DummyBuffer8;`n`t}",
        "`t// hdUE: a local, not the member: a later draw with previous draw data made a UAV of the read-only dummy.`n`tconst FRDGBufferRef TotalPrevDrawClusters = bHasPrevDrawData ? TotalPrevDrawClustersBuffer : DummyBuffer8;"),
    @("RasterPassParameters->InTotalPrevDrawClusters`t= GraphBuilder.CreateSRV(TotalPrevDrawClustersBuffer);",
        "RasterPassParameters->InTotalPrevDrawClusters`t= GraphBuilder.CreateSRV(TotalPrevDrawClusters);"))
# The binning passes, which run after that assignment, read the dummy the same way (a created buffer that no pass has
# written fails RDG validation).
Edit-EngineSource -Path $CullRaster -Label 'Nanite binning patch' -Marker 'hdUE: as AddPass_Rasterize' -Edits @(, @(
        "CommonBinBuildParameters.InTotalPrevDrawClusters`t= GraphBuilder.CreateSRV(TotalPrevDrawClustersBuffer);",
        "CommonBinBuildParameters.InTotalPrevDrawClusters`t= GraphBuilder.CreateSRV((RenderFlags & NANITE_RENDER_FLAG_HAS_PREV_DRAW_DATA) ? TotalPrevDrawClustersBuffer : GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, 8));`t// hdUE: as AddPass_Rasterize"))
