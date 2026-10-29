param(
    [string]$EngineRoot = '',
    [string]$EditorRoot = '',
    [string]$OpenUsdRoot = '',
    [switch]$SkipBuild,
    [switch]$WithUsdview
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$EngineRoot = Get-HdUELocalPath -Key engine_root -Value $EngineRoot
$EditorRoot = Get-HdUELocalPath -Key editor_root -Value $EditorRoot
$OpenUsdRoot = Get-HdUELocalPath -Key openusd_root -Value $OpenUsdRoot
$TaskRoot = Split-Path -Parent $PSScriptRoot
$RunRoot = New-HdUERun -Name verification -Create
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'Build-Host.ps1') -OpenUsdRoot $OpenUsdRoot *> (Join-Path $RunRoot 'host-build.log')
    & (Join-Path $PSScriptRoot 'Prepare-Content.ps1') -EditorRoot $EditorRoot *> (Join-Path $RunRoot 'content-build.log')
    & (Join-Path $PSScriptRoot 'Build-Backend.ps1') -EngineRoot $EngineRoot *> (Join-Path $RunRoot 'backend-build.log')
}
& (Join-Path $PSScriptRoot 'Stage-Runtime.ps1') -EngineRoot $EngineRoot *> (Join-Path $RunRoot 'stage.log')
$Native = Join-Path $RunRoot 'native'
$Hydra = Join-Path $RunRoot 'hydra'
# Each step must exit 0 (native failures do not always throw) and leave passing evidence.
function Invoke-Step([string]$Name, [scriptblock]$Step) {
    & $Step *> (Join-Path $RunRoot "$Name.log")
    if ($LASTEXITCODE) { throw "$Name failed (exit $LASTEXITCODE). Evidence: $RunRoot" }
}
function Read-Passed([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "Missing evidence: $Path" }
    $Data = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if (-not $Data.passed) { throw "Failed evidence: $Path" }
    $Data
}
Invoke-Step native { & (Join-Path $PSScriptRoot 'Test-Backend.ps1') -Mode both -Cycles 3 -OutputDirectory $Native }
Invoke-Step hydra { & (Join-Path $PSScriptRoot 'Test-Hydra.ps1') -OpenUsdRoot $OpenUsdRoot -Mode both -Cycles 2 -SelfTest -OutputDirectory $Hydra }
$NativeMetrics = Read-Passed (Join-Path $Native 'metrics.json')
$HydraMetrics = Read-Passed (Join-Path $Hydra 'metrics.json')
& uv run --quiet (Join-Path $PSScriptRoot 'compare_pfm.py') (Join-Path $Native 'cycle-0-pathtraced.pfm') (Join-Path $Hydra 'cycle-0-pathtraced.pfm') --output (Join-Path $RunRoot 'reference-comparison.json')
if ($LASTEXITCODE) { throw "Native UE versus Hydra image comparison failed. Evidence: $RunRoot" }
if ($WithUsdview) {
    Invoke-Step usdview { & (Join-Path $PSScriptRoot 'Test-Usdview.ps1') -OpenUsdRoot $OpenUsdRoot -OutputDirectory (Join-Path $RunRoot 'usdview') }
    Invoke-Step camera { & (Join-Path $PSScriptRoot 'Test-Usdview.ps1') -Check camera -OpenUsdRoot $OpenUsdRoot -OutputDirectory (Join-Path $RunRoot 'camera') }
    Invoke-Step buffer { & (Join-Path $PSScriptRoot 'Test-BufferPerf.ps1') -OpenUsdRoot $OpenUsdRoot -OutputFile (Join-Path $RunRoot 'buffer.json') }
    # These suites check their own counts and throw; their evidence must exist.
    foreach ($File in 'usdview\verified.json', 'camera\verified.json', 'buffer.json') {
        if (-not (Test-Path -LiteralPath (Join-Path $RunRoot $File))) { throw "Missing evidence: $File" }
    }
}
$Version = Get-Content -Raw -LiteralPath (Join-Path $EngineRoot 'Engine\Build\Build.version') | ConvertFrom-Json
$Evidence = [ordered]@{
    passed               = $true # every step above threw otherwise
    engine               = "$($Version.MajorVersion).$($Version.MinorVersion).$($Version.PatchVersion)"
    native_frames        = @($NativeMetrics.frames).Count
    hydra_frames         = @($HydraMetrics.frames).Count
    path_tracing_samples = (@($NativeMetrics.frames) | Where-Object { $_.mode -eq 'pathtraced' } | Select-Object -Last 1).samples
    engine_commit        = 'f45729c0b6f097038e72b52d366a62baeca3275b' # 5.8.4-release, as Setup-Engine.ps1 checks out
    hdue_commit          = (git -C $TaskRoot rev-parse HEAD)
    native_metrics       = (Join-Path $Native 'metrics.json')
    hydra_metrics        = (Join-Path $Hydra 'metrics.json')
    comparison           = (Join-Path $RunRoot 'reference-comparison.json')
}
if ($WithUsdview) {
    $Evidence.usdview_verification = (Join-Path $RunRoot 'usdview\verified.json')
    $Evidence.camera_verification = (Join-Path $RunRoot 'camera\verified.json')
    $Evidence.buffer_benchmark = (Join-Path $RunRoot 'buffer.json')
}
$Evidence | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $RunRoot 'verified.json') -Encoding utf8
Write-Output "Verified: $RunRoot"
