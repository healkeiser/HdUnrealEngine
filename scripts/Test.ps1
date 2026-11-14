# One entry point for the test suites. unit needs only uv (CI runs it); native runs the C++ tests of the builds there
# are; hydra, houdini, conformance and extended need the GPU, UE runtime and SDKs (VERIFICATION.md) and write their
# evidence under out\. all is unit, native, hydra and houdini; conformance and extended take long and run when named.
param(
    [ValidateSet('unit', 'native', 'hydra', 'houdini', 'conformance', 'extended', 'all')][string]$Suite = 'unit',
    [string]$RuntimeConfig = '',
    # conformance: the scene patterns (run_conformance.py --only), e.g. materialx/*.
    [string[]]$Only = @(),
    # native: the standalone build folder (Build-Host.ps1's); the Houdini build is houdini_build_root.
    [string]$BuildRoot = '',
    # extended: the folder of the prepared libraries the kinds' scenes and husk_prepared_materials.py bind (default
    # out\): materialx, materialx-wet, substrate\decal-* and substrate\volume-* (docs/building.md has the commands).
    [string]$LibraryRoot = ''
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
        'materials'          = 'samples\materialx\showcase_subsets.usda'  # its GeomSubset checks need one
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
    # The prepared libraries the kinds bind (the newest Substrate one of each scene); a missing one fails its kind.
    $Root = if ($LibraryRoot) { [IO.Path]::GetFullPath($LibraryRoot) } else { Join-Path $TaskRoot 'out' }
    function Get-Library([string]$Pattern) {
        $Found = Get-ChildItem -Path (Join-Path $Root $Pattern) -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
        if ($Found) { $Found.FullName } else { Join-Path $Root $Pattern }
    }
    $MaterialX = Get-Library 'materialx\library.json'
    $Libraries = @{
        'materials'          = @{ MaterialXLibrary = $MaterialX }
        'references'         = @{ MaterialXLibrary = $MaterialX }
        # The Substrate Builder graphs layer over the showcase's MaterialX materials (substrate-builder.md).
        'substrate'          = @{ MaterialXLibrary = $MaterialX; SubstrateLibrary = Get-Library 'substrate\showcase-*\library.json' }
        'substrate-textures' = @{ MaterialXLibrary = $MaterialX }
        'substrate-live'     = @{ MaterialXLibrary = $MaterialX }
        'surfaces'           = @{ MaterialXLibrary = Get-Library 'materialx-wet\library.json' }
        'volume-material'    = @{ SubstrateLibrary = Get-Library 'substrate\volume-*\library.json' }
        'decal'              = @{ SubstrateLibrary = Get-Library 'substrate\decal-*\library.json' }
    }
    $Failed = @()
    foreach ($Kind in $Scenes.Keys) {
        $Output = Join-Path $Run $Kind
        $Library = if ($Libraries[$Kind]) { $Libraries[$Kind] } else { @{} }
        try {
            foreach ($File in $Library.Values) { if (-not (Test-Path -LiteralPath $File)) { throw "No prepared library at $File (docs/building.md, Extended tier)." } }
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
            & (Join-Path $PSScriptRoot 'Launch-Houdini.ps1') -RuntimeConfig $RuntimeConfig @Library @Source -LogDirectory $Output -TestKind $Kind
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
    # Prepared materials in husk (decal, volume and MaterialX libraries, a stale graph failing the frame): every check
    # runs (--require-all), with the libraries above (a missing one is not prepared and staged here).
    $HuskArguments = @('--require-all', '--materialx-library', $MaterialX, '--wet-library', $Libraries['surfaces'].MaterialXLibrary, '--decal-library', $Libraries['decal'].SubstrateLibrary, '--volume-library', $Libraries['volume-material'].SubstrateLibrary)
    if ($RuntimeConfig) { $HuskArguments += @('--runtime-config', [IO.Path]::GetFullPath($RuntimeConfig)) }
    & uv run --python 3.11 --with numpy==2.4.6 --with OpenImageIO==3.2.2.0 python (Join-Path $TaskRoot 'tests\husk_prepared_materials.py') @HuskArguments
    if ($LASTEXITCODE) { Write-Output 'extended: FAILED husk prepared materials'; $Failed += 'husk-prepared-materials' }
    # The usdview scene check (edits, reopening, the USD file unchanged) on the smoke scene without its light: it
    # exercises viewer lighting, which lights only a scene that has none.
    & (Join-Path $PSScriptRoot 'Test-Usdview.ps1') -Check scene -InputUsd (Join-Path $TaskRoot 'tests\usdview\smoke_unlit.usda') -RuntimeConfig $RuntimeConfig -OutputDirectory (Join-Path $Run 'usdview-scene')
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
    # Streamed scene uploads (a batch per mesh) render as the whole-scene upload.
    $HuskArguments = if ($RuntimeConfig) { @('--runtime-config', [IO.Path]::GetFullPath($RuntimeConfig)) } else { @() }
    & uv run --python 3.11 --with numpy==2.4.6 --with OpenImageIO==3.2.2.0 python (Join-Path $TaskRoot 'tests\husk_streaming.py') @HuskArguments
    if ($LASTEXITCODE) { throw 'husk streaming test failed.' }
    $Ran += 'houdini'
}
if ($Suite -eq 'all') {
    # The long tiers run only when named: say so, so "all passed" is not read as everything.
    $Skipped = if ('native' -notin $Ran) { 'native (no build), ' } else { '' }
    Write-Output "Suite all passed ($($Ran -join ', ')). Not run: $($Skipped)conformance (-Suite conformance -Only <area>/*), extended (-Suite extended)."
}
else { Write-Output "Suite $Suite passed." }
