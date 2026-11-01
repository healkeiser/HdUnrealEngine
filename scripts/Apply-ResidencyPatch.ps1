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
