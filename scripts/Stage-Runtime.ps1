param([string]$EngineRoot = '', [string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$EngineRoot = Get-HdUELocalPath -Key engine_root -Value $EngineRoot
$TaskRoot = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $TaskRoot 'unreal\HdUEBackend\HdUEBackend.uproject'
$Stage = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $TaskRoot 'out\runtime' }
& (Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat') BuildCookRun "-project=$Project" -noP4 -platform=Win64 -clientconfig=Development -target=HdUEBackend -skipbuild -skipcook -stage -skippackage "-stagingdirectory=$Stage" -unattended -utf8output
if ($LASTEXITCODE) { throw 'Runtime staging failed.' }
$Runtime = Join-Path $Stage 'Windows'
$Game = Join-Path $Runtime 'HdUEBackend'
$Bin = Join-Path $Game 'Binaries\Win64'
New-Item -ItemType Directory -Force -Path $Bin | Out-Null
$HostBuild = Join-Path $TaskRoot 'build'
$Smoke = Join-Path $HostBuild 'Release\hdue_backend_smoke.exe'
if (-not (Test-Path -LiteralPath $Smoke)) {
    # The Houdini-only path skips Build-Host.ps1; the smoke host needs no OpenUSD.
    # Keep an existing Build-Host.ps1 configuration instead of disabling Hydra in its cache.
    if (-not (Test-Path -LiteralPath (Join-Path $HostBuild 'CMakeCache.txt'))) {
        & cmake -S $TaskRoot -B $HostBuild -G 'Visual Studio 17 2022' -A x64 -DHDUE_BUILD_HYDRA=OFF
        if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
    }
    & cmake --build $HostBuild --config Release --target hdue_backend_smoke
    if ($LASTEXITCODE) { throw 'Backend smoke host build failed.' }
}
Copy-Item -LiteralPath $Smoke -Destination $Bin
Copy-Item -LiteralPath $Project -Destination (Join-Path $Game 'HdUEBackend.uproject')
Copy-Item -LiteralPath (Join-Path $TaskRoot 'unreal\HdUEBackend\Shaders') -Destination $Game -Recurse -Force
Write-Output "Runtime ready: $Runtime"

