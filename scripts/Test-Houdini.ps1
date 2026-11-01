param(
    [string]$HoudiniRoot = '',
    [string]$RuntimeConfig = '',
    [string]$OutputDirectory = '',
    [int]$TimeoutSeconds = 420
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$HoudiniRoot = Get-HdUELocalPath -Key houdini_root -Value $HoudiniRoot
$TaskRoot = Split-Path -Parent $PSScriptRoot
$RunOutput = New-HdUERun -Name houdini -Path $OutputDirectory
# LOP cook, native asset proxies, stage space, render outputs, the material re-preparation change filter and the
# Substrate Material Builder.
# The repo's HDAs and Python come first, as in Launch-Houdini.ps1: an installed hdUE package would otherwise win.
$PreviousHoudiniPath = $env:HOUDINI_PATH
$env:HOUDINI_PATH = "$TaskRoot\integrations\houdini;" + $(if ($PreviousHoudiniPath) { $PreviousHoudiniPath } else { '&' })
try {
    foreach ($Test in 'houdini_cook', 'asset_controls_test', 'scene_space_test', 'render_outputs_test', 'shading_change_test', 'substrate_builder_test') {
        & uv run -- (Join-Path $HoudiniRoot 'bin\hython.exe') (Join-Path $TaskRoot "tests\$Test.py")
        if ($LASTEXITCODE) { throw "$Test failed." }
    }
}
finally { $env:HOUDINI_PATH = $PreviousHoudiniPath }
& (Join-Path $PSScriptRoot 'Launch-Houdini.ps1') -HoudiniRoot $HoudiniRoot -RuntimeConfig $RuntimeConfig -InputUsd (Join-Path $TaskRoot 'samples\smoke.usda') -LogDirectory $RunOutput -TestKind integration -TimeoutSeconds $TimeoutSeconds
$Result = Get-Content -LiteralPath (Join-Path $RunOutput 'verified.json') -Raw | ConvertFrom-Json
$Runtime = Get-Content -LiteralPath (Join-Path $RunOutput 'runtime-state.json') -Raw | ConvertFrom-Json
# Minimums: the test adds frames and captures as it grows; it fails by itself on a missing one.
if (-not $Result.passed -or -not $Result.native_solaris_compositor_verified -or $Result.frames.Count -lt 10 -or $Result.captures.Count -lt 6 -or $Runtime.ue_initializations -ne 1 -or $Runtime.runtime_state -ne 4 -or -not $Runtime.closed) {
    throw "Incomplete Houdini / UE evidence: $RunOutput"
}
Write-Output "Verified Solaris / same-process UE: $RunOutput"
# A post-process setting set back to its default (Revert to Defaults, or typed) renders as before it was changed.
& (Join-Path $PSScriptRoot 'Launch-Houdini.ps1') -HoudiniRoot $HoudiniRoot -RuntimeConfig $RuntimeConfig -InputUsd (Join-Path $TaskRoot 'samples\smoke.usda') -LogDirectory (Join-Path $RunOutput 'postprocess') -TestKind postprocess -TimeoutSeconds $TimeoutSeconds
Write-Output "Verified Solaris post-process defaults: $RunOutput\postprocess"
# Selection highlight and click picking: a Y-up metre stage (cube, mesh, instancer, points, curves) in the free view,
# and the Z-up Cornell box through its camera (render buffers of another size than the viewport).
foreach ($Case in @(
        @{ Name = 'selection'; Usd = 'tests\houdini\selection.usda'; Camera = ''; Select = '/World/Cube,/World/Mesh,/World/Instancer,/World/Points,/World/Curves' },
        @{ Name = 'selection-cornell'; Usd = 'samples\cornell\cornell.usda'; Camera = '/cornell_box/camera'; Select = '/cornell_box/tall_box_front/tall_box_front,/cornell_box/short_box_front/short_box_front' })) {
    $env:HDUE_TEST_CAMERA = $Case.Camera
    $env:HDUE_TEST_SELECT = $Case.Select
    try {
        $CaseOutput = Join-Path $RunOutput $Case.Name
        & (Join-Path $PSScriptRoot 'Launch-Houdini.ps1') -HoudiniRoot $HoudiniRoot -RuntimeConfig $RuntimeConfig -InputUsd (Join-Path $TaskRoot $Case.Usd) -LogDirectory $CaseOutput -TestKind selection -TimeoutSeconds $TimeoutSeconds
    }
    finally { Remove-Item Env:HDUE_TEST_CAMERA, Env:HDUE_TEST_SELECT -ErrorAction SilentlyContinue }
    if (-not (Get-Content -LiteralPath (Join-Path $CaseOutput 'selection.json') -Raw | ConvertFrom-Json).passed) { throw "Selection test failed: $CaseOutput" }
    Write-Output "Verified Solaris selection ($($Case.Name)): $CaseOutput"
}
