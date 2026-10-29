# Removes the Houdini package file written by Install.ps1. The hdUE folder itself is left in place.
param(
    [string]$HoudiniVersion = '22.0',
    [string]$PackagesDirectory = ''
)
$ErrorActionPreference = 'Stop'
if (-not $PackagesDirectory) {
    $Prefs = if ($env:HOUDINI_USER_PREF_DIR) { $env:HOUDINI_USER_PREF_DIR -replace '__HVER__', $HoudiniVersion }
    else { Join-Path ([Environment]::GetFolderPath('MyDocuments')) "houdini$HoudiniVersion" }
    $PackagesDirectory = Join-Path $Prefs 'packages'
}
$Target = Join-Path $PackagesDirectory 'hdUE.json'
if (Test-Path -LiteralPath $Target) { Remove-Item -LiteralPath $Target; Write-Output "Removed $Target" }
else { Write-Output "No hdUE package at $Target" }
