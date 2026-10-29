param(
    [Parameter(Mandatory)][string]$RuntimeConfig,
    [string]$OutputDirectory = '',
    [string]$InputUsd = ''
)
# Renders with every generated render setting (scripts/generate_render_settings.py) authored at its default, then
# checks the UE log: no setting may be skipped, and each console variable's default must be the value UE had.
$ErrorActionPreference = 'Stop'
$TaskRoot = Split-Path -Parent $PSScriptRoot
$Run = if ($OutputDirectory) {[IO.Path]::GetFullPath($OutputDirectory)} else {Join-Path $TaskRoot ('out\settings\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))}
New-Item -ItemType Directory -Force -Path $Run | Out-Null
$Settings = Join-Path $Run 'settings.json'
& python (Join-Path $PSScriptRoot 'generate_render_settings.py') --defaults-json $Settings
if ($LASTEXITCODE) { throw 'Generating the default settings failed.' }
$Project = (Get-Content -Raw -LiteralPath $RuntimeConfig | ConvertFrom-Json).runtime_project
if (-not [IO.Path]::IsPathRooted($Project)) { $Project = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($RuntimeConfig))) $Project }
$Log = Join-Path (Split-Path -Parent $Project) 'Saved\Logs\HdUEBackend.log'
$Arguments = @{RuntimeConfig = $RuntimeConfig; RendererSettings = $Settings; OutputDirectory = (Join-Path $Run 'render'); Mode = 'both'; Cycles = 1}
if ($InputUsd) { $Arguments.InputUsd = $InputUsd }
& (Join-Path $PSScriptRoot 'Test-Hydra.ps1') @Arguments
$Lines = Get-Content -LiteralPath $Log
$Skipped = @($Lines | Select-String 'hdUE: (post-process setting|console variable) .* skipped')
$Skipped | ForEach-Object { Write-Host $_.Line }
$Declared = (Get-Content -Raw -LiteralPath $Settings | ConvertFrom-Json).declaredConsoleDefaults
$Wrong = @()
foreach ($Variable in $Declared.PSObject.Properties) {
    $Set = $Lines | Select-String ('hdUE: console variable ' + [regex]::Escape($Variable.Name) + ' = \S+ \(was (\S*)\)') | Select-Object -First 1
    if (-not $Set) { $Wrong += "$($Variable.Name): never set"; continue }
    $Was = $Set.Matches[0].Groups[1].Value
    if ($Was -match '^(true|false)$') { $Was = [int]($Was -eq 'true') }
    if ([double]$Was -ne [double]$Variable.Value) { $Wrong += "$($Variable.Name): generated default $($Variable.Value), UE had $Was" }
}
$Wrong | ForEach-Object { Write-Host $_ }
$Count = ((Get-Content -Raw -LiteralPath $Settings | ConvertFrom-Json).settings.PSObject.Properties | Measure-Object).Count
Write-Host "$Count settings authored, $($Skipped.Count) skipped, $($Wrong.Count) console defaults differ from UE ($Log)"
if ($Skipped.Count -or $Wrong.Count) { throw 'Render settings check failed.' }
