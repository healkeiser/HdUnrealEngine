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
    foreach ($Test in 'live_materials_test', 'materialx_live_test', 'materialx_textures_cache_test', 'scene_space_test', 'substrate_graph_test', 'surface_effects_test', 'asset_controls_test') {
        & uv run --python 3.11 --with usd-core --with numpy --with MaterialX==1.39.4 python (Join-Path $TaskRoot "tests\$Test.py")
        if ($LASTEXITCODE) { throw "$Test failed." }
    }
    # Houdini 22's Python (shutil.rmtree's onexc needs 3.12); no hou needed.
    & uv run --python 3.13 python (Join-Path $TaskRoot 'tests\houdini_menu_test.py')
    if ($LASTEXITCODE) { throw 'houdini_menu_test failed.' }
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
    # hdUE's create/edit prim LOPs (hython, no GPU).
    if ($RuntimeConfig) { $env:HDUE_RUNTIME_CONFIG = [IO.Path]::GetFullPath($RuntimeConfig) }
    & uv run --python 3.11 python (Join-Path $TaskRoot 'tests\houdini_prim_lops.py')
    if ($LASTEXITCODE) { throw 'Prim LOP tests failed.' }
    # Prepared materials in husk (decal, volume and MaterialX libraries, a stale graph failing the frame).
    $HuskArguments = if ($RuntimeConfig) { @('--runtime-config', [IO.Path]::GetFullPath($RuntimeConfig)) } else { @() }
    & uv run --python 3.11 --with numpy --with OpenImageIO python (Join-Path $TaskRoot 'tests\husk_prepared_materials.py') @HuskArguments
    if ($LASTEXITCODE) { throw 'husk prepared-material tests failed.' }
    # Streamed scene uploads (a batch per mesh) render as the whole-scene upload.
    & uv run --python 3.11 --with numpy --with OpenImageIO python (Join-Path $TaskRoot 'tests\husk_streaming.py') @HuskArguments
    if ($LASTEXITCODE) { throw 'husk streaming test failed.' }
}
Write-Output "Suite $Suite passed."
