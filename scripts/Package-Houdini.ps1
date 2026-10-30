# Builds a self-contained, relocatable hdUE folder (and zip) for Houdini 22.0:
# delegate DLL, Houdini integration, Python helpers, the staged UE runtime and installers.
param(
    [string]$OutputDirectory = '',
    [string]$RuntimeConfig = '',
    [string]$BuildDirectory = '',
    [switch]$Zip,
    # Package a build from uncommitted changes, or one stamped with another commit: never for a release.
    [switch]$AllowDirty,
    # The backend links Developer modules (Engine Tools): the Unreal Engine EULA (6(d)) allows such a package only
    # through Fab or a fork of Epic's UnrealEngine GitHub network, never as a public download.
    [switch]$UnrealLicenseesOnly
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$TaskRoot = Split-Path -Parent $PSScriptRoot
# ponytail: checks the target source, not the DLL the runtime profile names; rebuild the backend after changing it.
$Target = Get-Content -LiteralPath (Join-Path $TaskRoot 'unreal\HdUEBackend\Source\HdUEBackend.Target.cs') -Raw
if ($Target -match 'bBuildDeveloperTools\s*=\s*true' -and -not $UnrealLicenseesOnly) {
    throw 'The backend is built with Developer modules (bBuildDeveloperTools): pass -UnrealLicenseesOnly and distribute the package only through Fab or a fork of the UnrealEngine GitHub network.'
}
$EngineRoot = Get-HdUELocalPath -Key engine_root
$EngineLicenses = Join-Path $EngineRoot 'Engine\Source\ThirdParty\Licenses'
if (-not (Test-Path -LiteralPath $EngineLicenses)) { throw "Missing $EngineLicenses." }
$Build = Get-HdUELocalPath -Key houdini_build_root -Value $BuildDirectory
if (-not $RuntimeConfig) {
    $RuntimeConfig = Get-HdUELocalPath -Key runtime_config -Optional
    $Active = Join-Path $TaskRoot 'out\project-assets\active-houdini.json'
    if (-not $RuntimeConfig -and (Test-Path -LiteralPath $Active)) { $RuntimeConfig = $Active }
}
if (-not $RuntimeConfig -or -not (Test-Path -LiteralPath $RuntimeConfig)) { throw 'Specify -RuntimeConfig (the profile Launch-Houdini.ps1 uses).' }
$RuntimeConfig = (Resolve-Path -LiteralPath $RuntimeConfig).Path
$Config = Get-Content -LiteralPath $RuntimeConfig -Raw | ConvertFrom-Json
function Resolve-ConfigPath([string]$Value) {
    if ([IO.Path]::IsPathRooted($Value)) { $Value } else { [IO.Path]::GetFullPath((Join-Path (Split-Path $RuntimeConfig) $Value)) }
}
$BackendDll = Resolve-ConfigPath $Config.backend_dll
$Project = Resolve-ConfigPath $Config.runtime_project
$Game = Split-Path $Project
$RuntimeRoot = Split-Path $Game
$Delegate = Join-Path $Build 'Release\hdUE_Houdini22.dll'
foreach ($Path in $BackendDll, $Project, $Delegate, (Join-Path $Build 'resources\plugInfo.json')) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "Missing $Path; build and stage first." }
}
$Version = (& git -C $TaskRoot describe --tags --always --dirty).Trim()
# The delegate's version resource (resources/version.rc.in) names the commit and the Houdini build it was built from.
$Stamp = (Get-Item -LiteralPath $Delegate).VersionInfo
$HoudiniBuild = $Stamp.Comments -replace '^Houdini ', ''
if ($HoudiniBuild -notmatch '^\d+\.\d+\.\d+$') { throw "$Delegate has no Houdini build in its version resource: run Build-Houdini.ps1." }
if (-not $AllowDirty) {
    if ($Version -like '*-dirty') { throw "The tree has uncommitted changes ($Version): commit them, or pass -AllowDirty." }
    if ($Stamp.ProductVersion -ne $Version) { throw "$Delegate was built from $($Stamp.ProductVersion), not ${Version}: run Build-Houdini.ps1, or pass -AllowDirty." }
}
$Out = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $TaskRoot 'out\package' }
$Name = "hdUE-$Version-houdini$HoudiniBuild-win64"
$Package = Join-Path $Out $Name
if (Test-Path -LiteralPath $Package) { Remove-Item -LiteralPath $Package -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Package | Out-Null

function Copy-Tree([string]$From, [string]$To) {
    & robocopy $From $To @args /NFL /NDL /NJH /NJS | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copy failed: $From" }
}
# Same relative layout as the repository, so hdue_houdini.ROOT and its imports keep working.
Copy-Tree (Join-Path $TaskRoot 'integrations\houdini') (Join-Path $Package 'integrations\houdini') /E /XD __pycache__
# Only the scripts/ modules the integration imports, found by following its imports.
$Modules = & uv run --quiet --python 3.13 (Join-Path $PSScriptRoot 'runtime_modules.py')
if ($LASTEXITCODE) { throw 'Cannot list the runtime modules.' }
Copy-Tree (Join-Path $TaskRoot 'scripts') (Join-Path $Package 'scripts') @($Modules)
# Tracked samples only: samples/sponza/intel, samples/jungle_ruins/intel and samples/hdris are local downloads (~12 GB).
$Samples = & git -C $TaskRoot ls-files -- samples
if ($LASTEXITCODE) { throw 'Cannot list the tracked samples.' }
foreach ($File in $Samples) {
    $Target = Join-Path $Package $File
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $TaskRoot $File) -Destination $Target
}
New-Item -ItemType Directory -Force -Path (Join-Path $Package 'plugin\Release'), (Join-Path $Package 'plugin\resources') | Out-Null
Copy-Item -LiteralPath $Delegate -Destination (Join-Path $Package 'plugin\Release')
Copy-Item -LiteralPath (Join-Path $Build 'resources\plugInfo.json') -Destination (Join-Path $Package 'plugin\resources')
# Only the backend DLL the profile names; per-run state (Saved) and other hashed backends stay out. So do the smoke
# test, import libraries and the D3D12/Vulkan debug layers: debugging aids, not part of the product.
$RuntimeTarget = Join-Path $Package 'runtime\Windows'
Copy-Tree $RuntimeRoot $RuntimeTarget /E /XD Saved DerivedDataCache Intermediate /XF HdUEBackend*.dll HdUEBackend*.pdb hdue_backend_smoke.exe *.lib *.exp d3d12SDKLayers.dll VkLayer_*
$BinTarget = Join-Path $RuntimeTarget ([IO.Path]::GetRelativePath($RuntimeRoot, (Split-Path $BackendDll)))
Copy-Item -LiteralPath $BackendDll -Destination $BinTarget
# Houdini's own tbb12.dll is loaded first: every staged DLL must still find its imports there.
& uv run --quiet (Join-Path $PSScriptRoot 'check_shared_dlls.py') $RuntimeTarget (Join-Path (Get-HdUELocalPath -Key houdini_root) 'bin')
if ($LASTEXITCODE) { throw 'A staged DLL imports symbols that Houdini''s copy of the same DLL lacks.' }
$Relative = { param($Path) 'runtime/Windows/' + ([IO.Path]::GetRelativePath($RuntimeRoot, $Path) -replace '\\', '/') }
$Runtime = [ordered]@{version = 1; hdue_version = $Version; houdini_build = $HoudiniBuild; backend_dll = (& $Relative $BackendDll); runtime_project = (& $Relative $Project) }
[IO.File]::WriteAllText((Join-Path $Package 'runtime.json'), ($Runtime | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
Copy-Item -Path (Join-Path $TaskRoot 'packaging\houdini\*') -Destination $Package
# License terms: hdUE's MIT license, the package terms the Unreal Engine EULA requires (4(c), 7(a)) and the
# third-party licenses Unreal Engine ships with (7(b)).
Copy-Item -LiteralPath (Join-Path $TaskRoot 'LICENSE'), (Join-Path $TaskRoot 'THIRD_PARTY_NOTICES.md') -Destination $Package
Copy-Tree $EngineLicenses (Join-Path $Package 'licenses') /E
$global:LASTEXITCODE = 0
Write-Output "Package ready: $Package"
if ($Zip) {
    $Archive = "$Package.zip"
    if (Test-Path -LiteralPath $Archive) { Remove-Item -LiteralPath $Archive }
    # Windows' bsdtar writes zip; a GNU tar earlier on PATH (Git Bash) cannot.
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -a -c -f $Archive -C $Out $Name
    if ($LASTEXITCODE) { throw 'Zip failed.' }
    Write-Output "Archive: $Archive"
    # Symbols stay out of the package; keep this archive with the release to symbolise users' crash dumps.
    $Symbols = Join-Path $Out "$Name-symbols"
    if (Test-Path -LiteralPath $Symbols) { Remove-Item -LiteralPath $Symbols -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Symbols | Out-Null
    # Update-HoudiniRuntime.ps1 copies only the hashed DLL; the staged runtime keeps the backend's symbols unhashed.
    $BackendPdb = Join-Path (Split-Path -Parent $BackendDll) 'HdUEBackend.pdb'
    foreach ($Pdb in [IO.Path]::ChangeExtension($Delegate, 'pdb'), $BackendPdb) {
        if (-not (Test-Path -LiteralPath $Pdb)) { throw "Missing $Pdb." }
        Copy-Item -LiteralPath $Pdb -Destination $Symbols
    }
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -a -c -f "$Symbols.zip" -C $Out "$Name-symbols"
    if ($LASTEXITCODE) { throw 'Symbols zip failed.' }
    Remove-Item -LiteralPath $Symbols -Recurse -Force
    Write-Output "Symbols: $Symbols.zip"
}
