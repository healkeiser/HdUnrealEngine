# One entry point for the test suites. unit needs only uv (CI runs it); hydra and houdini need the GPU, UE runtime and
# SDKs (VERIFICATION.md) and write their evidence under out\.
param(
    [ValidateSet('unit', 'hydra', 'houdini', 'all')][string]$Suite = 'unit',
    [string]$RuntimeConfig = ''
)
$ErrorActionPreference = 'Stop'
$TaskRoot = Split-Path -Parent $PSScriptRoot
if ($Suite -in 'unit', 'all') {
    & uv run --python 3.11 (Join-Path $TaskRoot 'tests\test_repository_scope.py')
    if ($LASTEXITCODE) { throw 'Repository scope failed.' }
    & (Join-Path $TaskRoot 'tests\local_paths.ps1')
    foreach ($Test in 'substrate_live_test', 'scene_space_test', 'substrate_graph_test', 'surface_effects_test', 'asset_controls_test') {
        & uv run --python 3.11 --with usd-core --with numpy python (Join-Path $TaskRoot "tests\$Test.py")
        if ($LASTEXITCODE) { throw "$Test failed." }
    }
    # The committed gallery: every scene's brightness within its band, every linked image present.
    & uv run --python 3.11 python (Join-Path $PSScriptRoot 'check_gallery.py')
    if ($LASTEXITCODE) { throw 'Gallery check failed.' }
}
if ($Suite -in 'hydra', 'all') {
    # Native UE and Hydra renders, usdview, camera and buffer checks, with evidence (no rebuild).
    & (Join-Path $PSScriptRoot 'Verify.ps1') -SkipBuild -WithUsdview
}
if ($Suite -in 'houdini', 'all') {
    & (Join-Path $PSScriptRoot 'Test-Houdini.ps1') -RuntimeConfig $RuntimeConfig
    & (Join-Path $PSScriptRoot 'Test-HoudiniOpTextures.ps1') -RuntimeConfig $RuntimeConfig
}
Write-Output "Suite $Suite passed."
