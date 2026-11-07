# Runs materialx_coverage.py with OpenUSD's Python (as Import-UsdMaterialX.ps1): which MaterialX nodes reach UE through
# hdUE's export and UE's MaterialX importer. Writes out/materialx_coverage.json.
param([switch]$Trace, [switch]$Export, [switch]$One, [string]$Only = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$OpenUsdRoot = Get-HdUELocalPath -Key openusd_root
$PreviousPath = $env:PATH
$PreviousPythonPath = $env:PYTHONPATH
try {
    $env:PATH = "$OpenUsdRoot\bin;$OpenUsdRoot\lib;$PreviousPath"
    $env:PYTHONPATH = "$OpenUsdRoot\lib\python"
    $Arguments = @(if ($Trace) { '--trace' }) + @(if ($Export) { '--export' }) + @(if ($One) { '--one' }) + @(if ($Only) { "--only=$Only" })
    & uv run --python 3.11 --script (Join-Path $PSScriptRoot 'materialx_coverage.py') @Arguments
    if ($LASTEXITCODE) { throw 'MaterialX coverage failed.' }
}
finally {
    $env:PATH = $PreviousPath
    $env:PYTHONPATH = $PreviousPythonPath
}
