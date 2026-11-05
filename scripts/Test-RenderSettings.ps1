param(
    [Parameter(Mandatory)][string]$RuntimeConfig,
    [string]$OutputDirectory = '',
    [string]$InputUsd = ''
)
# Renders with every generated render setting (scripts/generate_render_settings.py) authored at its default, then
# checks the UE log: no setting may be skipped, and each console variable's default must be the value UE had.
$ErrorActionPreference = 'Stop'
$TaskRoot = Split-Path -Parent $PSScriptRoot
$Run = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $TaskRoot ('out\settings\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $Run | Out-Null
$Settings = Join-Path $Run 'settings.json'
& python (Join-Path $PSScriptRoot 'generate_render_settings.py') --defaults-json $Settings
if ($LASTEXITCODE) { throw 'Generating the default settings failed.' }
# The run's own UE log (-ABSLOG): UE writes HdUEBackend_2.log, _3, ... when another process still holds HdUEBackend.log,
# so the default name can hold an older run.
$Log = Join-Path $Run 'ue.log'
$PreviousArguments = $env:HDUE_UE_ARGS
$env:HDUE_UE_ARGS = ("$PreviousArguments -ABSLOG=`"$Log`"").Trim()
$Arguments = @{RuntimeConfig = $RuntimeConfig; RendererSettings = $Settings; OutputDirectory = (Join-Path $Run 'render'); Mode = 'both'; Cycles = 1 }
if ($InputUsd) { $Arguments.InputUsd = $InputUsd }
try { & (Join-Path $PSScriptRoot 'Test-Hydra.ps1') @Arguments } finally { $env:HDUE_UE_ARGS = $PreviousArguments }
$Lines = Get-Content -LiteralPath $Log
$Skipped = @($Lines | Select-String 'hdUE: (post-process setting|console variable) .* skipped')
$Skipped | ForEach-Object { Write-Host $_.Line }
$Declared = (Get-Content -Raw -LiteralPath $Settings | ConvertFrom-Json).declaredConsoleDefaults
$Wrong = @()
foreach ($Variable in $Declared.PSObject.Properties) {
    # Offline hosts log their profile's changes first, with UE's value before it (Bridge.cpp, Initialize).
    $Set = $Lines | Select-String ('hdUE: (?:offline profile )?console variable ' + [regex]::Escape($Variable.Name) + ' = \S+ \(was (\S*)\)') | Select-Object -First 1
    if (-not $Set) { $Wrong += "$($Variable.Name): never set"; continue }
    $Was = $Set.Matches[0].Groups[1].Value
    if ($Was -match '^(true|false)$') { $Was = [int]($Was -eq 'true') }
    $Differs = if ($Variable.Value -is [string]) { $Was -ne $Variable.Value } else { [double]$Was -ne [double]$Variable.Value }
    if ($Differs) { $Wrong += "$($Variable.Name): generated default $($Variable.Value), UE had $Was" }
}
$Wrong | ForEach-Object { Write-Host $_ }
$Count = ((Get-Content -Raw -LiteralPath $Settings | ConvertFrom-Json).settings.PSObject.Properties | Measure-Object).Count
Write-Host "$Count settings authored, $($Skipped.Count) skipped, $($Wrong.Count) console defaults differ from UE ($Log)"
if ($Skipped.Count -or $Wrong.Count) { throw 'Render settings check failed.' }
