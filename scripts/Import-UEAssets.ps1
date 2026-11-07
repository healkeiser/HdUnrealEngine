# Cooks chosen assets of a UE project for hdUE (hdUE Unreal Asset LOP, hdue_ue_import.py): copies the project's content
# into hdUE's UE project (the source project is not changed), cooks only the given packages and what they reference
# with their shader code inline (as live materials are, so a running runtime loads them without a shader library),
# and writes the catalog of the project's assets. hdue_ue_import.publish then adds the cooked packages to a runtime.
param(
    [Parameter(Mandatory = $true)][string]$ProjectPath,
    [Parameter(Mandatory = $true)][string]$Packages,  # /Game/... package names, comma separated
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$EditorRoot = '',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$EditorRoot = Get-HdUELocalPath -Key editor_root -Value $EditorRoot
$TaskRoot = Split-Path -Parent $PSScriptRoot
$Run = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $Run | Out-Null
$List = @($Packages.Split(',', [StringSplitOptions]::RemoveEmptyEntries))
foreach ($Package in $List) {
    if ($Package -notmatch '^/Game/[A-Za-z0-9_/\-]+$') { throw "Not a /Game package name: $Package" }
}
& "$PSScriptRoot\Copy-ProjectContent.ps1" -ProjectPath $ProjectPath -OutputDirectory (Join-Path $Run "content-copy-$(Get-Date -Format yyyyMMdd-HHmmss)") | Out-Null
if (-not $SkipBuild) { Build-HdUEEditor $EditorRoot }
$Editor = Join-Path $EditorRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Project = Join-Path $TaskRoot 'unreal\HdUEBackend\HdUEBackend.uproject'
$Log = Join-Path $Run 'cook.log'
& $Editor $Project -run=Cook -TargetPlatform=Windows "-PACKAGE=$($List -join '+')" "-OutputDir=$(Join-Path $Run 'cooked\[Platform]')" '-ini:Game:[/Script/UnrealEd.ProjectPackagingSettings]:bShareMaterialShaderCode=False' -unattended -nop4 -nosplash -nullrhi -stdout -UTF8Output *> $Log
if ($LASTEXITCODE) { throw "Cook failed: $Log" }
$Log = Join-Path $Run 'inspect.log'
& $Editor $Project -run=HdUEInspectAssets "-Output=$(Join-Path $Run 'catalog.json')" -unattended -nop4 -nosplash -nullrhi -stdout -UTF8Output *> $Log
if ($LASTEXITCODE) { throw "Asset inspection failed: $Log" }
Write-Output "Cooked $($List.Count) package(s) into $Run\cooked"
