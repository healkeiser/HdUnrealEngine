param(
    [string]$RuntimeConfig = '',
    [string]$OutputConfig = '',
    [switch]$SetDefault,
    [ValidateSet('Development', 'Test')][string]$Configuration = 'Development'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$TaskRoot = Split-Path -Parent $PSScriptRoot
$RuntimeConfig = Get-HdUELocalPath -Key runtime_config -Value $RuntimeConfig
$ConfigPath = [IO.Path]::GetFullPath($RuntimeConfig)
$Config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
if ($Config.version -ne 1) { throw 'Unsupported runtime configuration.' }
# Every relative path resolves against the profile's folder, as the delegate and hdue_houdini.py resolve them, since the
# profile may be written into another folder.
foreach ($Property in $Config.PSObject.Properties) {
    if ($Property.Value -is [string] -and $Property.Value -and -not [IO.Path]::IsPathRooted($Property.Value)) {
        $Property.Value = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $ConfigPath) $Property.Value))
    }
}
$Source = Join-Path $TaskRoot ('unreal\HdUEBackend\Binaries\Win64\' + $(if ($Configuration -eq 'Test') { 'HdUEBackend-Win64-Test.dll' } else { 'HdUEBackend.dll' }))
if (-not (Test-Path -LiteralPath $Source)) { throw 'Build-Backend.ps1 must finish first.' }
foreach ($SourceShader in Get-ChildItem -LiteralPath (Join-Path $TaskRoot 'unreal\HdUEBackend\Shaders') -Filter '*.usf') {
    $RuntimeShader = Join-Path (Split-Path -Parent $Config.runtime_project) "Shaders\$($SourceShader.Name)"
    if (-not (Test-Path -LiteralPath $RuntimeShader) -or
        (Get-FileHash -LiteralPath $SourceShader.FullName).Hash -ne (Get-FileHash -LiteralPath $RuntimeShader).Hash) {
        throw 'This runtime needs the updated global shaders. Prepare content, build the backend, and stage a fresh runtime before updating its DLL. Preserve any caller-supplied asset catalog/material library in the new runtime configuration.'
    }
}
$SourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
$Destination = Join-Path (Split-Path -Parent $Config.backend_dll) ("HdUEBackend-h22-$($SourceHash.Substring(0, 16).ToLowerInvariant()).dll")
$DestinationHash = { if (Test-Path -LiteralPath $Destination) { (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash } }
# A copy an earlier run left incomplete is replaced (a loaded DLL is never an incomplete one).
if ((& $DestinationHash) -ne $SourceHash) { Copy-Item -LiteralPath $Source -Destination $Destination -Force }
if ((& $DestinationHash) -ne $SourceHash) { throw 'Runtime DLL copy verification failed.' }
# The runtime's unhashed DLL (Stage-Runtime.ps1's copy, which .local/runtime.json, usdview and Test-Backend.ps1 load)
# follows the build too, so every profile of this runtime passes the delegate's ABI check. Only when the default profile
# (runtime-houdini.json) is updated: a side profile stages its build for its own tests and leaves the shared DLL alone.
$DefaultProfile = Join-Path (Split-Path -Parent $ConfigPath) 'runtime-houdini.json'
if (-not $OutputConfig) { $OutputConfig = $DefaultProfile }
$IsDefault = $SetDefault -or [IO.Path]::GetFullPath($OutputConfig) -eq [IO.Path]::GetFullPath($DefaultProfile)
$Unhashed = Join-Path (Split-Path -Parent $Destination) 'HdUEBackend.dll'
if ($IsDefault -and (Test-Path -LiteralPath $Unhashed) -and
    (Get-FileHash -LiteralPath $Unhashed -Algorithm SHA256).Hash -ne $SourceHash) {
    try { Copy-Item -LiteralPath $Source -Destination $Unhashed -Force }
    catch { Write-Warning "$Unhashed is in use and keeps the previous build: $($_.Exception.Message)" }
}
$Config.backend_dll = $Destination
# Profiles are replaced whole (written beside, then moved), as Prepare-LiveMaterials.ps1 publishes: a husk starting
# while Set-Content had truncated the file read half a profile.
function Write-Profile([string]$Path) {
    $Pending = "$Path.pending"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null  # a side profile's new folder
    $Config | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Pending -Encoding utf8
    Move-Item -LiteralPath $Pending -Destination $Path -Force
}
Write-Profile ([IO.Path]::GetFullPath($OutputConfig))
Write-Output "Houdini runtime configuration: $([IO.Path]::GetFullPath($OutputConfig))"
if ($SetDefault) {
    $DefaultConfig = Join-Path $TaskRoot 'out\project-assets\active-houdini.json'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $DefaultConfig) | Out-Null
    Write-Profile $DefaultConfig
    Write-Output "Default Houdini runtime: $DefaultConfig"
}
# Hashed DLLs that no profile names any more go (253 MB each); one a running Houdini or husk has loaded stays until a
# later run.
# The profiles read are the default ones and those beside -OutputConfig (a side profile can live anywhere); the DLL
# just staged always stays.
$ProfileFolders = @((Join-Path $TaskRoot '.local\*.json'), (Join-Path $TaskRoot 'out\project-assets\*.json'), (Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($OutputConfig))) '*.json'))
$Named = @([IO.Path]::GetFullPath($Destination)) + @(Get-ChildItem -Path $ProfileFolders -ErrorAction SilentlyContinue |
        ForEach-Object { try { (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).backend_dll } catch { $null } } |
        Where-Object { $_ } | ForEach-Object { [IO.Path]::GetFullPath($_) })
foreach ($Old in Get-ChildItem -LiteralPath (Split-Path -Parent $Destination) -Filter 'HdUEBackend-h22-*.dll') {
    if ($Named -notcontains $Old.FullName) {
        try { Remove-Item -LiteralPath $Old.FullName -ErrorAction Stop } catch { Write-Verbose "$($Old.Name) is in use" }
    }
}
