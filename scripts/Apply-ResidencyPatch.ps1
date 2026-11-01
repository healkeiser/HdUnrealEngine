param([string]$EngineRoot = '', [string]$EditorRoot = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$EngineRoot = Get-HdUELocalPath -Key engine_root -Value $EngineRoot
$EditorRoot = Get-HdUELocalPath -Key editor_root -Value $EditorRoot -Optional
$Version = Get-Content -Raw -LiteralPath (Join-Path $EngineRoot 'Engine\Build\Build.version') | ConvertFrom-Json
if ($Version.MajorVersion -ne 5 -or $Version.MinorVersion -ne 8 -or $Version.PatchVersion -ne 4) { throw 'Residency adapter requires UE 5.8.4.' }
$Header = Join-Path $EngineRoot 'Engine\Source\Runtime\Renderer\Private\VT\VirtualTextureSystem.h'
$Source = [IO.File]::ReadAllText($Header)
$Friend = "`tfriend class FHdueVirtualTextureResidency;"
if (-not $Source.Contains($Friend)) {
    $Anchor = "`tfriend class FRendererModule;"
    if (($Source.Split($Anchor).Length - 1) -ne 1) { throw 'Unexpected VirtualTextureSystem.h; inspect the pinned engine before applying the residency patch.' }
    $Newline = if ($Source.Contains("`r`n")) { "`r`n" } else { "`n" }
    [IO.File]::WriteAllText($Header, $Source.Replace($Anchor, $Anchor + $Newline + $Friend), [Text.UTF8Encoding]::new($false))
    Write-Host 'Applied hdUE VT residency friend adapter to UE 5.8.4.'
}
# Runtime Nanite builds (Scene.cpp BuildNanite): NaniteBuilder reads displacement texture sources, which exist only in editor builds.
$Displace = Join-Path $EngineRoot 'Engine\Source\Developer\NaniteBuilder\Private\NaniteDisplace.cpp'
$Source = [IO.File]::ReadAllText($Displace)
if (-not $Source.Contains('#if WITH_EDITORONLY_DATA')) {
    $Newline = if ($Source.Contains("`r`n")) { "`r`n" } else { "`n" }
    $Open = "`t`t`tif ( IsValid( DisplacementMap.Texture ) && DisplacementMap.Texture->Source.IsValid() )"
    $Close = "better to crash then continuing with some false data$Newline`t`t`t`tcheckNoEntry();$Newline`t`t`t}"
    if (($Source.Split($Open).Length - 1) -ne 1 -or ($Source.Split($Close).Length - 1) -ne 1) { throw 'Unexpected NaniteDisplace.cpp; inspect the pinned engine before applying the Nanite patch.' }
    $Source = $Source.Replace($Open, "#if WITH_EDITORONLY_DATA$Newline$Open").Replace($Close, "$Close$Newline#endif // WITH_EDITORONLY_DATA (hdUE: texture sources exist only in the editor; runtime builds have no displacement maps)")
    [IO.File]::WriteAllText($Displace, $Source, [Text.UTF8Encoding]::new($false))
    Write-Host 'Applied hdUE runtime Nanite build patch to UE 5.8.4.'
}
# UE 5.8.4's AmbientCubemapComposite.usf never defines SUBSTRATE_LOAD_FROM_MATERIALCONTAINER, so with Substrate's adaptive
# GBuffer (r.Substrate.ProjectGBufferFormat=1, UE's SM6 default) it reads the legacy GBuffer, which that format does not
# write, and adds nothing. Define it as the other Substrate composites do (ReflectionEnvironmentPixelShader.usf). The
# editor that cooks the runtime compiles its global shaders, so its copy is patched too.
foreach ($Root in @($EngineRoot, $EditorRoot) | Where-Object { $_ } | Select-Object -Unique) {
    $Shader = Join-Path $Root 'Engine\Shaders\Private\AmbientCubemapComposite.usf'
    $Source = [IO.File]::ReadAllText($Shader)
    if (-not $Source.Contains('SUBSTRATE_LOAD_FROM_MATERIALCONTAINER 1')) {
        $Newline = if ($Source.Contains("`r`n")) { "`r`n" } else { "`n" }
        $Anchor = '#define ENABLE_SIMPLE_ENV_LIGHT 1'
        if (($Source.Split($Anchor).Length - 1) -ne 1) { throw "Unexpected $Shader; inspect the engine before applying the Ambient Cubemap patch." }
        $Define = (
            '#if SUBSTRATE_GBUFFER_FORMAT==0', '#define SUBSTRATE_LOAD_FROM_MATERIALCONTAINER 0', '#else',
            '#define SUBSTRATE_LOAD_FROM_MATERIALCONTAINER 1 // hdUE: the adaptive GBuffer reads the material container', '#endif'
        ) -join $Newline
        [IO.File]::WriteAllText($Shader, $Source.Replace($Anchor, $Anchor + $Newline + $Define), [Text.UTF8Encoding]::new($false))
        Write-Host "Applied hdUE Ambient Cubemap shader patch to $Root."
    }
}
# Runtime-built Nanite meshes (Scene.cpp BuildNanite) keep their streaming pages in memory: outside the editor, UE's
# streaming manager reads non-root pages only through the IoDispatcher, with a chunk id only cooked packages have, so
# every page had to be a root page in a pool of at most 2 GB. In non-editor builds, a page of loaded (in-memory) bulk
# data is copied into the staging buffer instead, as the editor's EState::Memory path installs it; cooked bulk data is
# never loaded, so cooked meshes stream from disk as before.
$Header = Join-Path $EngineRoot 'Engine\Source\Runtime\Engine\Public\Rendering\NaniteStreamingManager.h'
$Manager = Join-Path $EngineRoot 'Engine\Source\Runtime\Engine\Private\Rendering\NaniteStreamingManager.cpp'
$HeaderSource = [IO.File]::ReadAllText($Header)
$ManagerSource = [IO.File]::ReadAllText($Manager)
if (-not $ManagerSource.Contains('bFromMemory')) {
    $Newline = if ($ManagerSource.Contains("`r`n")) { "`r`n" } else { "`n" }
    $Edits = @(
        @($Header, "`t`tFIoBuffer`t`t`t`tRequestBuffer;", "`t`tbool`t`t`t`t`tbFromMemory = false;`t// hdUE: copied from in-memory bulk data$Newline`t`tFIoBuffer`t`t`t`tRequestBuffer;"),
        @($Header, "`t`t`tRequestBuffer = FIoBuffer();", "`t`t`tbFromMemory = false;$Newline`t`t`tRequestBuffer = FIoBuffer();"),
        @($Manager, 'const bool bDiskRequest = true;', 'const bool bDiskRequest = !BulkData.IsBulkDataLoaded();	// hdUE: runtime-built pages are in memory'),
        @($Manager, "Batch.Read(BulkData, PageStreamingState.BulkOffset, PageStreamingState.BulkSize, AIOP_Low, PendingPage.RequestBuffer, PendingPage.Request);$Newline`t`t`t`t`t`tbIssueIOBatch = true;",
            (("#if !WITH_EDITOR", "`t`t`t`t`t`tif (!bDiskRequest)`t// hdUE: in-memory bulk data (runtime-built Nanite) has no IO chunk to read", "`t`t`t`t`t`t{",
             "`t`t`t`t`t`t`tconst uint8* BulkDataPtr = (const uint8*)BulkData.LockReadOnly();",
             "`t`t`t`t`t`t`tFMemory::Memcpy(Dst, BulkDataPtr + PageStreamingState.BulkOffset, PageStreamingState.BulkSize);",
             "`t`t`t`t`t`t`tBulkData.Unlock();", "`t`t`t`t`t`t`tPendingPage.bFromMemory = true;", "`t`t`t`t`t`t}", "`t`t`t`t`t`telse", "#endif", "`t`t`t`t`t`t{",
             "`t`t`t`t`t`t`tBatch.Read(BulkData, PageStreamingState.BulkOffset, PageStreamingState.BulkSize, AIOP_Low, PendingPage.RequestBuffer, PendingPage.Request);",
             "`t`t`t`t`t`t`tbIssueIOBatch = true;", "`t`t`t`t`t`t}") -join $Newline)),
        @($Manager, 'if (PendingPage.Request.IsCompleted())', 'if (PendingPage.bFromMemory || PendingPage.Request.IsCompleted())'),
        @($Manager, 'if (!PendingPage.Request.IsOk())', 'if (!PendingPage.bFromMemory && !PendingPage.Request.IsOk())'),
        @($Manager, 'check(Task.PendingPage->Request.IsCompleted());', 'check(Task.PendingPage->bFromMemory || Task.PendingPage->Request.IsCompleted());')
    )
    foreach ($Edit in $Edits) {
        $Text = if ($Edit[0] -eq $Header) { $HeaderSource } else { $ManagerSource }
        if (($Text.Split($Edit[1]).Length - 1) -ne 1) { throw "Unexpected $($Edit[0]); inspect the engine before applying the Nanite streaming patch." }
    }
    foreach ($Edit in $Edits) {
        if ($Edit[0] -eq $Header) { $HeaderSource = $HeaderSource.Replace($Edit[1], $Edit[2]) } else { $ManagerSource = $ManagerSource.Replace($Edit[1], $Edit[2]) }
    }
    [IO.File]::WriteAllText($Header, $HeaderSource, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($Manager, $ManagerSource, [Text.UTF8Encoding]::new($false))
    Write-Host 'Applied hdUE runtime Nanite streaming patch to UE 5.8.4.'
}
