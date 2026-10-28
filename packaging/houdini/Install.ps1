# Registers this hdUE folder as a Houdini package, like HtoA or Redshift do.
# The folder stays where it is: move it, then run Install.ps1 again.
param(
    [string]$HoudiniVersion='22.0',
    [string]$PackagesDirectory=''
)
$ErrorActionPreference='Stop'
$Root=$PSScriptRoot
foreach ($Required in 'runtime.json','plugin\Release\hdUE_Houdini22.dll','integrations\houdini\UsdRenderers.json') {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $Required))) {throw "Incomplete hdUE package: $Required is missing."}
}
if (-not $PackagesDirectory) {
    $Prefs=if ($env:HOUDINI_USER_PREF_DIR) {$env:HOUDINI_USER_PREF_DIR -replace '__HVER__',$HoudiniVersion}
        else {Join-Path ([Environment]::GetFolderPath('MyDocuments')) "houdini$HoudiniVersion"}
    $PackagesDirectory=Join-Path $Prefs 'packages'
}
New-Item -ItemType Directory -Force -Path $PackagesDirectory | Out-Null
$Package=[ordered]@{
    enable="houdini_version >= '$HoudiniVersion' and houdini_version < '$HoudiniVersion.999'"
    env=@(
        @{HDUE=($Root -replace '\\','/')},
        @{HDUE_HOUDINI_ENABLE='1'},
        @{HDUE_RUNTIME_CONFIG='$HDUE/runtime.json'},
        @{HDUE_HOUDINI_PLUGIN='$HDUE/plugin/Release/hdUE_Houdini22.dll'},
        @{PXR_PLUGINPATH_NAME=[ordered]@{value='$HDUE/plugin/resources'; method='append'}}
    )
    hpath='$HDUE/integrations/houdini'
}
$Target=Join-Path $PackagesDirectory 'hdUE.json'
[IO.File]::WriteAllText($Target,($Package | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
Write-Output "hdUE installed for Houdini $HoudiniVersion -> $Target"
Write-Output 'Restart Houdini, then pick "Unreal Engine (hdUE)" in the Solaris viewport renderer menu or on a USD Render ROP.'
