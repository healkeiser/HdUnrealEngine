param(
    [string]$HoudiniRoot = '',
    [string]$RuntimeConfig = '',
    [string]$OutputDirectory = '',
    [int]$TimeoutSeconds = 420
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$HoudiniRoot = Get-HdUELocalPath -Key houdini_root -Value $HoudiniRoot
$TaskRoot = Split-Path -Parent $PSScriptRoot
$RunOutput = New-HdUERun -Name houdini-op-textures -Path $OutputDirectory
# An installed release package would load ahead of the dev build (the list is comma-separated).
$Previous = @{ HOUDINI_PACKAGE_SKIPLIST = $env:HOUDINI_PACKAGE_SKIPLIST }
try {
    $env:HOUDINI_PACKAGE_SKIPLIST = Get-HdUEPackageSkipList
    & uv run -- (Join-Path $HoudiniRoot 'bin\hython.exe') (Join-Path $TaskRoot 'tests\houdini_op_textures.py')
}
finally { Restore-HdUEEnvironment -Snapshot $Previous }  # the caller's shell keeps its own skip list, even on Ctrl+C
if ($LASTEXITCODE) { throw 'COP texture cache / LOP tests failed.' }
& (Join-Path $PSScriptRoot 'Launch-Houdini.ps1') -HoudiniRoot $HoudiniRoot -RuntimeConfig $RuntimeConfig -LogDirectory $RunOutput -TestKind optextures -TimeoutSeconds $TimeoutSeconds
$Result = Get-Content -LiteralPath (Join-Path $RunOutput 'verified.json') -Raw | ConvertFrom-Json
if (-not $Result.passed -or $Result.frames.Count -lt 7 -or $Result.captures.Count -lt 5 -or -not $Result.runtime.closed -or $Result.runtime.runtime_state -ne 4 -or $Result.runtime.ue_initializations -ne 1) {
    throw "Incomplete live op: texture evidence: $RunOutput"
}
Write-Output "Verified live COP textures / Solaris / same-process UE: $RunOutput"
