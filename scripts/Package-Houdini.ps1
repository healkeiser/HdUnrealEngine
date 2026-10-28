# Builds a self-contained, relocatable hdUE folder (and zip) for Houdini 22.0:
# delegate DLL, Houdini integration, Python helpers, the staged UE runtime and installers.
param(
    [string]$OutputDirectory='',
    [string]$RuntimeConfig='',
    [string]$BuildDirectory='',
    [switch]$Zip
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$TaskRoot=Split-Path -Parent $PSScriptRoot
$BuildDirectory=Get-HdUELocalPath -Key houdini_build_root -Value $BuildDirectory -Optional
$Build=if ($BuildDirectory) {[IO.Path]::GetFullPath($BuildDirectory)} else {Join-Path $TaskRoot 'build\houdini-22.0.466'}
if (-not $RuntimeConfig) {
    $RuntimeConfig=Get-HdUELocalPath -Key runtime_config -Optional
    $Active=Join-Path $TaskRoot 'out\project-assets\active-houdini.json'
    if (-not $RuntimeConfig -and (Test-Path -LiteralPath $Active)) {$RuntimeConfig=$Active}
}
if (-not $RuntimeConfig -or -not (Test-Path -LiteralPath $RuntimeConfig)) {throw 'Specify -RuntimeConfig (the profile Launch-Houdini.ps1 uses).'}
$RuntimeConfig=(Resolve-Path -LiteralPath $RuntimeConfig).Path
$Config=Get-Content -LiteralPath $RuntimeConfig -Raw | ConvertFrom-Json
function Resolve-ConfigPath([string]$Value) {
    if ([IO.Path]::IsPathRooted($Value)) {$Value} else {[IO.Path]::GetFullPath((Join-Path (Split-Path $RuntimeConfig) $Value))}
}
$BackendDll=Resolve-ConfigPath $Config.backend_dll
$Project=Resolve-ConfigPath $Config.runtime_project
$Game=Split-Path $Project
$RuntimeRoot=Split-Path $Game
$Delegate=Join-Path $Build 'Release\hdUE_Houdini22.dll'
foreach ($Path in $BackendDll,$Project,$Delegate,(Join-Path $Build 'resources\plugInfo.json')) {
    if (-not (Test-Path -LiteralPath $Path)) {throw "Missing $Path; build and stage first."}
}
$Version=(& git -C $TaskRoot describe --tags --always --dirty).Trim()
$Out=if ($OutputDirectory) {[IO.Path]::GetFullPath($OutputDirectory)} else {Join-Path $TaskRoot 'out\package'}
$Name="hdUE-$Version-houdini22.0-win64"
$Package=Join-Path $Out $Name
if (Test-Path -LiteralPath $Package) {Remove-Item -LiteralPath $Package -Recurse -Force}
New-Item -ItemType Directory -Force -Path $Package | Out-Null

# Same relative layout as the repository, so hdue_houdini.ROOT and its imports keep working.
& robocopy (Join-Path $TaskRoot 'integrations\houdini') (Join-Path $Package 'integrations\houdini') /E /XD __pycache__ /NFL /NDL /NJH /NJS | Out-Null
& robocopy (Join-Path $TaskRoot 'scripts') (Join-Path $Package 'scripts') *.py *.ps1 /XD __pycache__ /NFL /NDL /NJH /NJS | Out-Null
& robocopy (Join-Path $TaskRoot 'samples') (Join-Path $Package 'samples') /E /NFL /NDL /NJH /NJS | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Package 'plugin\Release'),(Join-Path $Package 'plugin\resources') | Out-Null
Copy-Item -LiteralPath $Delegate -Destination (Join-Path $Package 'plugin\Release')
Copy-Item -LiteralPath (Join-Path $Build 'resources\plugInfo.json') -Destination (Join-Path $Package 'plugin\resources')
# Only the backend DLL the profile names; per-run state (Saved) and other hashed backends stay out.
$RuntimeTarget=Join-Path $Package 'runtime\Windows'
& robocopy $RuntimeRoot $RuntimeTarget /E /XD Saved DerivedDataCache Intermediate /XF HdUEBackend*.dll HdUEBackend*.pdb /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) {throw 'Runtime copy failed.'}
$BinTarget=Join-Path $RuntimeTarget ([IO.Path]::GetRelativePath($RuntimeRoot,(Split-Path $BackendDll)))
Copy-Item -LiteralPath $BackendDll -Destination $BinTarget
$Relative={param($Path) 'runtime/Windows/'+([IO.Path]::GetRelativePath($RuntimeRoot,$Path) -replace '\\','/')}
$Runtime=[ordered]@{version=1; backend_dll=(& $Relative $BackendDll); runtime_project=(& $Relative $Project)}
[IO.File]::WriteAllText((Join-Path $Package 'runtime.json'),($Runtime | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
Copy-Item -Path (Join-Path $TaskRoot 'packaging\houdini\*') -Destination $Package
Copy-Item -LiteralPath (Join-Path $TaskRoot 'LICENSE') -Destination $Package -ErrorAction SilentlyContinue
$global:LASTEXITCODE=0
Write-Output "Package ready: $Package"
if ($Zip) {
    $Archive="$Package.zip"
    if (Test-Path -LiteralPath $Archive) {Remove-Item -LiteralPath $Archive}
    & tar.exe -a -c -f $Archive -C $Out $Name
    if ($LASTEXITCODE) {throw 'Zip failed.'}
    Write-Output "Archive: $Archive"
}
