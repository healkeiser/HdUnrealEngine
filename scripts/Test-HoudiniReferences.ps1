# An instanceable Reference LOP downstream of the hdUE settings (tests/houdini/python3.13libs/hdue_reference_test.py):
# the MaterialX showcase referenced as /ReferencedAsset, its materials mapped to the cooked library's native assets with
# textures, and a transform edit moving it in realtime and Path Tracer frames.
param(
    [string]$HoudiniRoot = '',
    [string]$RuntimeConfig = '',
    [string]$BuildDirectory = '',
    # Import-MaterialX.ps1 -Source samples\materialx\standard_surface.mtlx,samples\materialx\open_pbr_surface.mtlx
    # -OutputDirectory out\materialx, then Stage-Runtime.ps1 so the runtime holds the cooked materials.
    [string]$MaterialXLibrary = '',
    [string]$OutputDirectory = '',
    [int]$TimeoutSeconds = 420
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$TaskRoot = Split-Path -Parent $PSScriptRoot
if (-not $MaterialXLibrary) { $MaterialXLibrary = Join-Path $TaskRoot 'out\materialx\library.json' }
if (-not (Test-Path -LiteralPath $MaterialXLibrary)) { throw "No cooked MaterialX library at $MaterialXLibrary; run Import-MaterialX.ps1 (see this script's -MaterialXLibrary)." }
$RunOutput = New-HdUERun -Name houdini-references -Path $OutputDirectory
& (Join-Path $PSScriptRoot 'Launch-Houdini.ps1') -HoudiniRoot $HoudiniRoot -RuntimeConfig $RuntimeConfig -BuildDirectory $BuildDirectory -InputUsd (Join-Path $TaskRoot 'samples\materialx\showcase_solaris.usda') -MaterialXLibrary $MaterialXLibrary -LogDirectory $RunOutput -TestKind references -TimeoutSeconds $TimeoutSeconds
$Result = Get-Content -LiteralPath (Join-Path $RunOutput 'verified.json') -Raw | ConvertFrom-Json
if (-not $Result.passed -or $Result.frames.Count -lt 4 -or $Result.captures.Count -lt 2 -or -not $Result.material_stats) {
    throw "Incomplete reference evidence: $RunOutput"
}
Write-Output "Verified Solaris references / same-process UE: $RunOutput"
