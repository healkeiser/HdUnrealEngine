# Records the PSOs hdUE binds while rendering representative scenes in each mode, and turns them into the bundled PSO
# cache (unreal/HdUEBackend/Build/Windows/PipelineCaches/HdUEBackend_PCD3D_SM6.spc). The cook packs it into the
# runtime, which compiles those PSOs on worker threads from startup on, while the scene uploads, instead of on the
# render thread at the first frame. Run it after Prepare-Content.ps1 and Stage-Runtime.ps1; it recooks at the end, so
# stage again afterwards.
param(
    [string]$RuntimeConfig = '',
    [string]$HoudiniRuntimeConfig = '',
    [string]$EditorRoot = '',
    [string]$OpenUsdRoot = '',
    [string]$InputUsd = '',
    [string]$CameraPath = '/Camera',
    [int]$HoudiniSeconds = 90,
    [switch]$SkipCook,
    [switch]$SkipConformance
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$EditorRoot = Get-HdUELocalPath -Key editor_root -Value $EditorRoot
$TaskRoot = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $TaskRoot 'unreal\HdUEBackend\HdUEBackend.uproject'
$RuntimeProject = Join-Path $TaskRoot 'out\runtime\Windows\HdUEBackend\HdUEBackend.uproject'
if ($RuntimeConfig) {
    $ConfigPath = [IO.Path]::GetFullPath($RuntimeConfig)
    $RuntimeProject = (Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json).runtime_project
    if (-not [IO.Path]::IsPathRooted($RuntimeProject)) { $RuntimeProject = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $ConfigPath) $RuntimeProject)) }
}
$Game = Split-Path -Parent $RuntimeProject
$Keys = @(Get-ChildItem -LiteralPath (Join-Path $TaskRoot 'unreal\HdUEBackend\Saved\Cooked\Windows\HdUEBackend\Metadata\PipelineCaches') -Filter '*.shk' -ErrorAction SilentlyContinue)
if (-not $Keys) { throw 'No stable shader keys: run Prepare-Content.ps1 (NeedsShaderStableKeys) and Stage-Runtime.ps1 first.' }
$RunRoot = New-HdUERun -Name pipeline-cache -Create
$Recorded = Join-Path $Game 'Saved\CollectedPSOs'
if (Test-Path -LiteralPath $Recorded) { Remove-Item -LiteralPath $Recorded -Recurse -Force }

# -logPSO for every host (the hosts pass no UE arguments): record each bound PSO, saved to Saved/CollectedPSOs on exit.
$Ini = Join-Path $Game 'Config\DefaultEngine.ini'
$OriginalIni = [IO.File]::ReadAllBytes($Ini)
# A PSO compiled from an already bundled cache is not logged as new, so record without it.
$Bundled = Join-Path $Game 'Content\PipelineCaches'
$SetAside = Join-Path $RunRoot 'staged-PipelineCaches'
if (Test-Path -LiteralPath $Bundled) { Move-Item -LiteralPath $Bundled -Destination $SetAside }
try {
    # AutoSaveTimeBoundPSO: husk ends its process without shutting UE down, so its recording is saved while it renders
    # (UE leaves this autosave off by default since it can deadlock on CPUs with few cores; recording only).
    Add-Content -LiteralPath $Ini -Value "`r`n[SystemSettings]`r`nr.ShaderPipelineCache.LogPSO=1`r`nr.ShaderPipelineCache.SaveBoundPSOLog=1`r`nr.ShaderPipelineCache.AutoSaveTimeBoundPSO=1"
    $Common = @{ OpenUsdRoot = $OpenUsdRoot; RuntimeConfig = $RuntimeConfig; Mode = 'both'; Cycles = 1 }
    & (Join-Path $PSScriptRoot 'Test-Backend.ps1') -RuntimeConfig $RuntimeConfig -Mode both -Cycles 1 -OutputDirectory (Join-Path $RunRoot 'backend')
    & (Join-Path $PSScriptRoot 'Test-Hydra.ps1') @Common -SelfTest -OutputDirectory (Join-Path $RunRoot 'hydra-selftest')
    # Settings that switch render passes: Lumen off, and the Unreal tonemapper off (its post-processing chain).
    $Variants = [ordered]@{ default = @{}; 'no-tonemapper' = @{ tonemapper = $false }; 'no-lumen' = @{ lumenEnabled = $false } }
    $Scenes = @(@{ Name = 'smoke'; Usd = (Join-Path $TaskRoot 'samples\smoke.usda'); Camera = '/Camera' })
    if ($InputUsd) { $Scenes += @{ Name = 'scene'; Usd = [IO.Path]::GetFullPath($InputUsd); Camera = $CameraPath } }
    foreach ($Scene in $Scenes) {
        foreach ($Variant in $Variants.GetEnumerator()) {
            $Settings = Join-Path $RunRoot "$($Variant.Key).json"
            @{ version = 1; settings = $Variant.Value } | ConvertTo-Json | Set-Content -LiteralPath $Settings -Encoding utf8
            & (Join-Path $PSScriptRoot 'Test-Hydra.ps1') @Common -InputUsd $Scene.Usd -CameraPath $Scene.Camera -RendererSettings $Settings `
                -Width 1280 -Height 720 -OutputDirectory (Join-Path $RunRoot "$($Scene.Name)-$($Variant.Key)")
        }
    }
    # The Solaris viewport's own passes (half-resolution TSR while navigating, GPU frames): Houdini shows the scene
    # in the hdUE viewport for $HoudiniSeconds, then quits without a save prompt so UE writes its recording.
    if ($HoudiniRuntimeConfig) {
        $Hook = Join-Path $RunRoot 'houdini-hook'
        New-Item -ItemType Directory -Force -Path (Join-Path $Hook 'python3.13libs') | Out-Null
        Set-Content -LiteralPath (Join-Path $Hook 'python3.13libs\pythonrc.py') -Encoding utf8 -Value @"
import threading, hdefereval, hou
threading.Timer($HoudiniSeconds, lambda: hdefereval.executeDeferred(lambda: hou.exit(suppress_save_prompt=True))).start()
"@
        $PreviousPath = $env:HOUDINI_PATH
        try {
            $env:HOUDINI_PATH = "$Hook;&"
            $Launch = & (Join-Path $PSScriptRoot 'Launch-Houdini.ps1') -RuntimeConfig $HoudiniRuntimeConfig `
                -InputUsd $(if ($InputUsd) { $InputUsd } else { Join-Path $TaskRoot 'samples\smoke.usda' }) -LogDirectory (Join-Path $RunRoot 'houdini')
        }
        finally { $env:HOUDINI_PATH = $PreviousPath }
        $Houdini = Get-Process -Id ([regex]::Match("$Launch", 'Houdini PID (\d+)').Groups[1].Value)
        if (-not $Houdini.WaitForExit(($HoudiniSeconds + 120) * 1000)) { $Houdini.Kill(); throw "Houdini (PID $($Houdini.Id)) did not quit; killed it." }
        # The viewport's other profiles: the in-Houdini tests render realtime and Path Tracer frames, the ID passes
        # behind Solaris's selection (the Cornell box through its camera too), camera moves (the interactive
        # resolution), native assets, post-process settings, shared mesh builds and a resize, each quitting Houdini.
        # The runs Test-Houdini.ps1 makes; a failed check only warns: the recording is what counts here.
        $Smoke = Join-Path $TaskRoot 'samples\smoke.usda'
        $Runs = @(
            @{ Kind = 'integration'; Usd = $Smoke }, @{ Kind = 'postprocess'; Usd = $Smoke }, @{ Kind = 'shared-builds'; Usd = $Smoke },
            @{ Kind = 'selection'; Usd = (Join-Path $TaskRoot 'tests\houdini\selection.usda'); Camera = ''; Select = '/World/Cube,/World/Mesh' },
            @{ Kind = 'selection'; Name = 'selection-cornell'; Usd = (Join-Path $TaskRoot 'samples\cornell\cornell.usda'); Camera = '/cornell_box/camera'; Select = '/cornell_box/tall_box_front/tall_box_front' })
        # NVIDIA DLSS (off by default, as in the plugin; users turn it on): its passes, with and without Ray
        # Reconstruction, when the runtime has the plugin.
        $Dlss = Test-Path -LiteralPath (Join-Path $Game 'Plugins\DLSS\DLSS.uplugin')
        if ($Dlss) { $Runs += @{ Kind = 'dlss'; Usd = $Smoke } }
        # Cooked MaterialX materials (Test-HoudiniReferences.ps1) and the wet surface layer (-TestKind surfaces), when a
        # library has been imported.
        $Library = Join-Path $TaskRoot 'out\materialx\library.json'
        if (Test-Path -LiteralPath $Library) {
            $Runs += @{ Kind = 'references'; Usd = (Join-Path $TaskRoot 'samples\materialx\showcase_solaris.usda'); Library = $Library }
            $Runs += @{ Kind = 'surfaces'; Usd = (Join-Path $TaskRoot 'samples\wet_surface\showcase.usda'); Library = $Library }
        }
        foreach ($Run in $Runs) {
            $env:HDUE_TEST_CAMERA = $Run.Camera
            $env:HDUE_TEST_SELECT = $Run.Select
            try {
                $Extra = if ($Run.Library) { @{ MaterialXLibrary = $Run.Library } } else { @{} }
                & (Join-Path $PSScriptRoot 'Launch-Houdini.ps1') -RuntimeConfig $HoudiniRuntimeConfig -InputUsd $Run.Usd -TestKind $Run.Kind `
                    -LogDirectory (Join-Path $RunRoot "houdini-$(if ($Run.Name) { $Run.Name } else { $Run.Kind })") @Extra
            }
            catch { Write-Warning "$($Run.Kind) test: $_ (its PSOs are recorded anyway)" }
            finally { Remove-Item Env:HDUE_TEST_CAMERA, Env:HDUE_TEST_SELECT -ErrorAction SilentlyContinue }
        }
        # husk: the scene's own render products (render vars, AOVs) in both modes; three frames, so the last PSOs
        # bound get saved (AutoSaveTimeBoundPSO) before husk ends.
        if ($InputUsd) {
            $HoudiniRoot = Get-HdUELocalPath -Key houdini_root
            $Names = 'HOUDINI_PATH', 'PXR_PLUGINPATH_NAME', 'HDUE_RUNTIME_CONFIG'
            $Saved = @{}; foreach ($Name in $Names) { $Saved[$Name] = [Environment]::GetEnvironmentVariable($Name, 'Process') }
            try {
                $env:HOUDINI_PATH = "$TaskRoot\integrations\houdini;&"
                $env:PXR_PLUGINPATH_NAME = Join-Path (Get-HdUELocalPath -Key houdini_build_root) 'resources'
                $env:HDUE_RUNTIME_CONFIG = [IO.Path]::GetFullPath($HoudiniRuntimeConfig)
                # The mode is set on the scene's render settings prim (renderSettingsPrimPath), as run_conformance.py does.
                $Settings = (Get-Content -LiteralPath $InputUsd -TotalCount 40 | Select-String 'renderSettingsPrimPath = "([^"]+)"').Matches
                if (-not $Settings) { throw "$InputUsd names no renderSettingsPrimPath for husk." }
                $Prims = $Settings[0].Groups[1].Value.Trim('/') -split '/'
                foreach ($Variant in @('realtime', 'pathtraced') + $(if ($Dlss) { 'realtime-dlss' })) {
                    $Mode = $Variant -replace '-dlss$'
                    $Out = Join-Path $RunRoot "husk-$Variant"
                    New-Item -ItemType Directory -Force -Path $Out | Out-Null
                    $Root = Join-Path $Out 'root.usda'
                    $Lines = @('#usda 1.0', '(', "    renderSettingsPrimPath = `"$($Settings[0].Groups[1].Value)`"",
                        "    subLayers = [@$(([IO.Path]::GetFullPath($InputUsd)) -replace '\\', '/')@]", ')')
                    foreach ($Prim in $Prims) { $Lines += "over `"$Prim`"", '{' }
                    $Lines += "token unreal:renderMode = `"$Mode`""
                    if ($Variant -ne $Mode) { $Lines += 'int unreal:cvar:r:NGX:DLSS:Enable = 1' }
                    foreach ($Prim in $Prims) { $Lines += '}' }
                    Set-Content -LiteralPath $Root -Encoding utf8 -Value $Lines
                    & (Join-Path $HoudiniRoot 'bin\husk.exe') --renderer HdUnrealEngine -f 1 -n 3 -o (Join-Path $Out 'frame.$F4.exr') $Root *> (Join-Path $Out 'husk.log')
                    if ($LASTEXITCODE) { throw "husk ($Variant) failed; see $Out\husk.log." }
                }
            }
            finally { foreach ($Name in $Names) { [Environment]::SetEnvironmentVariable($Name, $Saved[$Name], 'Process') } }
        }
        # The conformance set (samples/conformance, both modes through husk, about an hour): the USD features hdUE
        # renders. Its checks only report here; -SkipConformance leaves it out.
        if (-not $SkipConformance) {
            & (Join-Path (Get-HdUELocalPath -Key houdini_root) 'bin\hython.exe') (Join-Path $PSScriptRoot 'run_conformance.py') `
                --runtime-config ([IO.Path]::GetFullPath($HoudiniRuntimeConfig)) *> (Join-Path $RunRoot 'conformance.log')
        }
    }
}
finally {
    [IO.File]::WriteAllBytes($Ini, $OriginalIni)
    if (Test-Path -LiteralPath $SetAside) { Move-Item -LiteralPath $SetAside -Destination $Bundled }
}

$Recordings = @(Get-ChildItem -LiteralPath $Recorded -Filter '*.rec.upipelinecache' -ErrorAction SilentlyContinue)
if (-not $Recordings) { throw "No PSOs were recorded under $Recorded." }
$Output = Join-Path $TaskRoot 'unreal\HdUEBackend\Build\Windows\PipelineCaches\HdUEBackend_PCD3D_SM6.spc'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null
$Editor = Join-Path $EditorRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
& $Editor $Project -run=ShaderPipelineCacheTools expand @($Recordings.FullName) @($Keys.FullName) $Output -unattended -nop4 -nosplash -nullrhi -stdout -UTF8Output *> (Join-Path $RunRoot 'expand.log')
if ($LASTEXITCODE -or -not (Test-Path -LiteralPath $Output)) { throw "PSO cache expansion failed; see $RunRoot\expand.log." }
Copy-Item -LiteralPath $Recordings.FullName -Destination $RunRoot
if (-not $SkipCook) {
    & (Join-Path $PSScriptRoot 'Prepare-Content.ps1') -EditorRoot $EditorRoot
}
Write-Output "PSO cache: $Output (recordings and logs: $RunRoot). Stage the runtime again to ship it."
