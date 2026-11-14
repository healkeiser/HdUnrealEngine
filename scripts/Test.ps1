# One entry point for the test suites. unit needs only uv (CI runs it); native runs the C++ tests of the builds there
# are; hydra, houdini, conformance and extended need the GPU, UE runtime and SDKs (VERIFICATION.md) and write their
# evidence under out\. all is unit, native, hydra and houdini; conformance and extended take long and run when named.
param(
    [ValidateSet('unit', 'native', 'hydra', 'houdini', 'conformance', 'extended', 'all')][string]$Suite = 'unit',
    [string]$RuntimeConfig = '',
    # conformance: the scene patterns (run_conformance.py --only), e.g. materials/*.
    [string[]]$Only = @(),
    # native: the standalone build folder (Build-Host.ps1's); the Houdini build is houdini_build_root.
    [string]$BuildRoot = '',
    # extended: the cooked MaterialX and prepared Substrate libraries the kinds' scenes bind (Import-MaterialX.ps1,
    # Prepare-Substrate.ps1); empty: the runtime profile's materialx_library and substrate_library.
    [string]$MaterialXLibrary = '',
    [string]$SubstrateLibrary = ''
)
$ErrorActionPreference = 'Stop'
$TaskRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$Ran = @()
if ($Suite -eq 'conformance' -and -not $Only) { throw '-Suite conformance needs -Only <area>/*; the full set takes over an hour' }
if ($Suite -in 'unit', 'all') {
    & uv run --python 3.11 (Join-Path $TaskRoot 'tests\test_repository_scope.py')
    if ($LASTEXITCODE) { throw 'Repository scope failed.' }
    & uv run --python 3.11 python (Join-Path $TaskRoot 'tests\render_setting_defaults_test.py')
    if ($LASTEXITCODE) { throw 'render_setting_defaults_test failed.' }
    & uv run --python 3.11 python (Join-Path $TaskRoot 'tests\generated_files_test.py')
    if ($LASTEXITCODE) { throw 'generated_files_test failed.' }
    & (Join-Path $TaskRoot 'tests\local_paths.ps1')
    & (Join-Path $TaskRoot 'tests\copy_project_content.ps1')
    foreach ($Test in 'live_materials_test', 'materialx_live_test', 'materialx_textures_cache_test', 'scene_space_test', 'substrate_graph_test', 'surface_effects_test', 'asset_controls_test', 'abi_mirror_test', 'materialx_lowering_test', 'material_versions_test') {
        & uv run --python 3.11 --with usd-core==26.11 --with numpy==2.4.6 --with MaterialX==1.39.4 python (Join-Path $TaskRoot "tests\$Test.py")
        if ($LASTEXITCODE) { throw "$Test failed." }
    }
    # Prepared images' colour spaces as the delegate converts them, with OCIO 2.5 (hython's) and OpenImageIO.
    & uv run --python 3.11 --with usd-core==26.11 --with numpy==2.4.6 --with MaterialX==1.39.4 --with OpenImageIO==3.2.2.0 --with opencolorio==2.5.0 python (Join-Path $TaskRoot 'tests\materialx_color_space_test.py')
    if ($LASTEXITCODE) { throw 'materialx_color_space_test failed.' }
    foreach ($Test in 'conformance_status_test') {
        & uv run --python 3.11 --with usd-core==26.11 --with numpy==2.4.6 --with OpenImageIO==3.2.2.0 python (Join-Path $TaskRoot "tests\$Test.py")
        if ($LASTEXITCODE) { throw "$Test failed." }
    }
    & uv run --python 3.11 --with numpy==2.4.6 --with OpenImageIO==3.2.2.0 python (Join-Path $TaskRoot 'tests\gallery_checks_test.py')
    if ($LASTEXITCODE) { throw 'gallery_checks_test failed.' }
    # Houdini 22's Python (shutil.rmtree's onexc needs 3.12); no hou needed.
    & uv run --python 3.13 python (Join-Path $TaskRoot 'tests\houdini_menu_test.py')
    if ($LASTEXITCODE) { throw 'houdini_menu_test failed.' }
    & uv run --python 3.13 python (Join-Path $TaskRoot 'tests\hdue_ue_import_test.py')
    if ($LASTEXITCODE) { throw 'hdue_ue_import_test failed.' }
    & uv run --python 3.13 python (Join-Path $TaskRoot 'tests\houdini_viewport_test.py')
    if ($LASTEXITCODE) { throw 'houdini_viewport_test failed.' }
    # The committed gallery: every scene's brightness within its band, every linked image present.
    & uv run --python 3.11 python (Join-Path $PSScriptRoot 'check_gallery.py')
    if ($LASTEXITCODE) { throw 'Gallery check failed.' }
    $Ran += 'unit'
}
if ($Suite -in 'native', 'all') {
    # The C++ tests of the builds there are (no rebuild: Build-Host.ps1 and Build-Houdini.ps1 build and run them).
    $Standalone = if ($BuildRoot) { [IO.Path]::GetFullPath($BuildRoot) } else { Join-Path $TaskRoot 'build' }
    $NativeRan = 0
    foreach ($Build in $Standalone, (Get-HdUELocalPath -Key houdini_build_root)) {
        if (-not (Test-Path -LiteralPath (Join-Path $Build 'CTestTestfile.cmake'))) {
            Write-Output "native: skipped $Build (not built)"
            continue
        }
        & ctest --test-dir $Build -C Release --output-on-failure
        if ($LASTEXITCODE) { throw "Native tests failed in $Build." }
        $NativeRan++
    }
    # Only a suite that ran counts: -Suite native with nothing built is a failure, and all just says it skipped it.
    if ($NativeRan) { $Ran += 'native' }
    elseif ($Suite -eq 'native') { throw 'No native tests ran: neither the standalone nor the Houdini build has a CTestTestfile.cmake (Build-Host.ps1, Build-Houdini.ps1).' }
}
if ($Suite -in 'hydra', 'all') {
    # Native UE and Hydra renders, usdview, camera and buffer checks, with evidence (no rebuild).
    & (Join-Path $PSScriptRoot 'Verify.ps1') -SkipBuild -WithUsdview
    $Ran += 'hydra'
}
if ($Suite -eq 'conformance') {
    # The conformance scenes a change touches (AGENTS.md's tiers), in husk, realtime and path traced.
    $HoudiniRoot = Get-HdUELocalPath -Key houdini_root
    $ProfilePath = if ($RuntimeConfig) { [IO.Path]::GetFullPath($RuntimeConfig) } else { Get-HdUELocalPath -Key runtime_config }
    & (Join-Path $HoudiniRoot 'bin\hython.exe') (Join-Path $PSScriptRoot 'run_conformance.py') --runtime-config $ProfilePath --only @Only
    if ($LASTEXITCODE) { throw "Conformance failed: $($Only -join ' ')" }
    $Ran += 'conformance'
}
if ($Suite -eq 'extended') {
    # The in-Houdini kinds no other runner starts, each on the scene its test drives. Not run: dlss (needs the
    # untracked DLSS plugin), viewport, parity and heavy (benchmarks that assert nothing), and the usdview checks on
    # private scenes (opacity, glass, assets).
    Write-Output 'extended: not run: dlss (needs the untracked DLSS plugin); viewport, parity, heavy (benchmarks, no assertions); usdview opacity, glass, assets (private scenes)'
    $Run = New-HdUERun -Name houdini-extended -Create
    $Scenes = [ordered]@{
        'materials'          = 'samples\materialx\showcase_solaris.usda'
        'lighting'           = 'samples\lighting\lighting.usda'
        'surfaces'           = 'samples\wet_surface\showcase.usda'
        'substrate'          = 'samples\substrate\showcase.usda'
        'substrate-textures' = ''
        'substrate-live'     = ''
        'materialx-live'     = 'samples\materialx\procedural.usda'
        'volume-material'    = 'samples\volume_material\volume_material.usda'
        'decal'              = 'samples\decal\decal.usda'
        'references'         = 'samples\materialx\showcase_solaris.usda'
    }
    $Failed = @()
    foreach ($Kind in $Scenes.Keys) {
        $Output = Join-Path $Run $Kind
        # references: the MaterialX showcase's cooked library by default (Import-MaterialX.ps1 -Source
        # samples\materialx\standard_surface.mtlx,samples\materialx\open_pbr_surface.mtlx -OutputDirectory out\materialx).
        $Library = if ($Kind -eq 'references' -and -not $MaterialXLibrary) { Join-Path $TaskRoot 'out\materialx\library.json' } else { $MaterialXLibrary }
        try {
            if ($Kind -eq 'references' -and -not (Test-Path -LiteralPath $Library)) { throw "No cooked MaterialX library at $Library; run Import-MaterialX.ps1." }
            $Source = if ($Scenes[$Kind]) { @{ InputUsd = Join-Path $TaskRoot $Scenes[$Kind] } } else {
                # The texture Builder's editable HIP (substrate-builder.md), made from the directly textured scene.
                $Hip = Join-Path $Run 'substrate-textures.hip'
                if (-not (Test-Path -LiteralPath $Hip)) {
                    $ProfilePath = if ($RuntimeConfig) { [IO.Path]::GetFullPath($RuntimeConfig) } else { Get-HdUELocalPath -Key runtime_config }
                    & uv run -- (Join-Path (Get-HdUELocalPath -Key houdini_root) 'bin\hython.exe') (Join-Path $PSScriptRoot 'generate_substrate_textures.py') --scene (Join-Path $TaskRoot 'samples\substrate\textures.usda') --hip $Hip --runtime-config $ProfilePath
                    if ($LASTEXITCODE) { throw 'generate_substrate_textures.py failed.' }
                }
                @{ HipFile = $Hip }
            }
            & (Join-Path $PSScriptRoot 'Launch-Houdini.ps1') -RuntimeConfig $RuntimeConfig -MaterialXLibrary $Library -SubstrateLibrary $SubstrateLibrary @Source -LogDirectory $Output -TestKind $Kind
            $Result = Get-Content -LiteralPath (Join-Path $Output 'verified.json') -Raw | ConvertFrom-Json
            if (-not $Result.passed) { throw 'verified.json does not say passed' }
            if ($Kind -eq 'references' -and ($Result.frames.Count -lt 4 -or $Result.captures.Count -lt 2 -or -not $Result.material_stats)) { throw 'Incomplete reference evidence' }
            Write-Output "extended: verified $Kind ($Output)"
        }
        catch {
            Write-Output "extended: FAILED $Kind ($Output): $_"
            $Failed += $Kind
        }
    }
    # The usdview scene check (edits, reopening, the USD file unchanged) on the Cornell box.
    & (Join-Path $PSScriptRoot 'Test-Usdview.ps1') -Check scene -InputUsd (Join-Path $TaskRoot 'samples\cornell\cornell.usda') -RuntimeConfig $RuntimeConfig -OutputDirectory (Join-Path $Run 'usdview-scene')
    if ($Failed) { throw "Extended in-Houdini tests failed: $($Failed -join ', ') ($Run)" }
    $Ran += 'extended'
}
if ($Suite -in 'houdini', 'all') {
    & (Join-Path $PSScriptRoot 'Test-Houdini.ps1') -RuntimeConfig $RuntimeConfig
    & (Join-Path $PSScriptRoot 'Test-HoudiniOpTextures.ps1') -RuntimeConfig $RuntimeConfig
    # hdUE's create/edit prim LOPs (hython, no GPU).
    $Previous = @{ HDUE_RUNTIME_CONFIG = $env:HDUE_RUNTIME_CONFIG }
    try {
        if ($RuntimeConfig) { $env:HDUE_RUNTIME_CONFIG = [IO.Path]::GetFullPath($RuntimeConfig) }
        & uv run --python 3.11 python (Join-Path $TaskRoot 'tests\houdini_prim_lops.py')
        if ($LASTEXITCODE) { throw 'Prim LOP tests failed.' }
    }
    finally { Restore-HdUEEnvironment -Snapshot $Previous }  # the caller's shell keeps its own profile
    # Prepared materials in husk (decal, volume and MaterialX libraries, a stale graph failing the frame).
    $HuskArguments = if ($RuntimeConfig) { @('--runtime-config', [IO.Path]::GetFullPath($RuntimeConfig)) } else { @() }
    & uv run --python 3.11 --with numpy==2.4.6 --with OpenImageIO==3.2.2.0 python (Join-Path $TaskRoot 'tests\husk_prepared_materials.py') @HuskArguments
    if ($LASTEXITCODE) { throw 'husk prepared-material tests failed.' }
    # Streamed scene uploads (a batch per mesh) render as the whole-scene upload.
    & uv run --python 3.11 --with numpy==2.4.6 --with OpenImageIO==3.2.2.0 python (Join-Path $TaskRoot 'tests\husk_streaming.py') @HuskArguments
    if ($LASTEXITCODE) { throw 'husk streaming test failed.' }
    $Ran += 'houdini'
}
if ($Suite -eq 'all') {
    # The long tiers run only when named: say so, so "all passed" is not read as everything.
    Write-Output "Suite all passed ($($Ran -join ', ')). Not run: conformance (-Suite conformance -Only <area>/*), extended (-Suite extended)."
}
else { Write-Output "Suite $Suite passed." }
