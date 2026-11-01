param(
    [string]$EngineRoot = '',
    [int]$MaxParallelActions = 24,
    # Development for work and CI; packages ship Test, Shipping with console commands and stats kept (UE's Build
    # Configurations), which hdUE's cvars need. Test writes HdUEBackend-Win64-Test.dll next to HdUEBackend.dll.
    [ValidateSet('Development', 'Test')][string]$Configuration = 'Development'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$EngineRoot = Get-HdUELocalPath -Key engine_root -Value $EngineRoot
$TaskRoot = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $TaskRoot 'unreal\HdUEBackend\HdUEBackend.uproject'
$Version = Get-Content -Raw -LiteralPath (Join-Path $EngineRoot 'Engine\Build\Build.version') | ConvertFrom-Json
if ($Version.MajorVersion -ne 5 -or $Version.MinorVersion -ne 8 -or $Version.PatchVersion -ne 4) {
    throw 'The backend is pinned to Unreal Engine 5.8.4.'
}
if (Test-Path -LiteralPath (Join-Path $EngineRoot 'Engine\Build\InstalledBuild.txt')) {
    throw 'The monolithic DLL requires a source engine, not a Launcher installed build.'
}
& (Join-Path $PSScriptRoot 'Apply-ResidencyPatch.ps1') -EngineRoot $EngineRoot
& (Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat') HdUEBackend Win64 $Configuration "-Project=$Project" -gather -WaitMutex -NoHotReload "-MaxParallelActions=$MaxParallelActions"
if ($LASTEXITCODE) { throw 'Unreal backend build failed.' }

