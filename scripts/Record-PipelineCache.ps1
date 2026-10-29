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
    [switch]$SkipCook
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
$RunRoot = Join-Path $TaskRoot ('out\pipeline-cache\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $RunRoot | Out-Null
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
    Add-Content -LiteralPath $Ini -Value "`r`n[SystemSettings]`r`nr.ShaderPipelineCache.LogPSO=1`r`nr.ShaderPipelineCache.SaveBoundPSOLog=1"
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
        } finally { $env:HOUDINI_PATH = $PreviousPath }
        $Houdini = Get-Process -Id ([regex]::Match("$Launch", 'Houdini PID (\d+)').Groups[1].Value)
        if (-not $Houdini.WaitForExit(($HoudiniSeconds + 120) * 1000)) { $Houdini.Kill(); throw "Houdini (PID $($Houdini.Id)) did not quit; killed it." }
    }
} finally {
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
