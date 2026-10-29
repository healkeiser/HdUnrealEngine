param(
    [string]$HoudiniRoot='',
    [string]$RuntimeConfig='',
    [string]$OutputDirectory='',
    [int]$TimeoutSeconds=420
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$HoudiniRoot=Get-HdUELocalPath -Key houdini_root -Value $HoudiniRoot
$TaskRoot=Split-Path -Parent $PSScriptRoot
$RunOutput=New-HdUERun -Name houdini -Path $OutputDirectory
# LOP cook, native asset proxies, stage space, render outputs and the material re-preparation change filter.
foreach ($Test in 'houdini_cook','asset_controls_test','scene_space_test','render_outputs_test','shading_change_test') {
    & uv run -- (Join-Path $HoudiniRoot 'bin\hython.exe') (Join-Path $TaskRoot "tests\$Test.py")
    if ($LASTEXITCODE) {throw "$Test failed."}
}
& (Join-Path $PSScriptRoot 'Launch-Houdini.ps1') -HoudiniRoot $HoudiniRoot -RuntimeConfig $RuntimeConfig -InputUsd (Join-Path $TaskRoot 'samples\smoke.usda') -LogDirectory $RunOutput -TestKind integration -TimeoutSeconds $TimeoutSeconds
$Result=Get-Content -LiteralPath (Join-Path $RunOutput 'verified.json') -Raw | ConvertFrom-Json
$Runtime=Get-Content -LiteralPath (Join-Path $RunOutput 'runtime-state.json') -Raw | ConvertFrom-Json
# Minimums: the test adds frames and captures as it grows; it fails by itself on a missing one.
if (-not $Result.passed -or -not $Result.native_solaris_compositor_verified -or $Result.frames.Count -lt 10 -or $Result.captures.Count -lt 6 -or $Runtime.ue_initializations -ne 1 -or $Runtime.runtime_state -ne 4 -or -not $Runtime.closed) {
    throw "Incomplete Houdini / UE evidence: $RunOutput"
}
Write-Output "Verified Solaris / same-process UE: $RunOutput"
