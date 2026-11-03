param(
    [string]$InputUsd = '', [string]$GraphJob = '',
    [string]$MaterialXLibrary = '',
    [string]$OutputDirectory = '',
    [string]$Destination = '/Game/HdUE/Substrate',
    [string]$HoudiniRoot = '', [string]$EditorRoot = '',
    [switch]$SkipBuild, [switch]$SkipCook
)
$ErrorActionPreference = 'Stop'
if ((-not $InputUsd -and -not $GraphJob) -or ($InputUsd -and $GraphJob)) { throw 'Choose InputUsd or GraphJob.' }
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$HoudiniRoot = Get-HdUELocalPath houdini_root -Value $HoudiniRoot -Optional:([bool]$GraphJob)
$EditorRoot = Get-HdUELocalPath editor_root -Value $EditorRoot
$TaskRoot = Split-Path -Parent $PSScriptRoot
$RunOutput = New-HdUERun -Name substrate -Path $OutputDirectory
if (-not $Destination.StartsWith('/Game/HdUE/')) { throw 'Substrate assets must be prepared below /Game/HdUE/.' }
$Arguments = if ($GraphJob) { @('export-job', [IO.Path]::GetFullPath($GraphJob), '--output', $RunOutput, '--destination', $Destination) } else { @('export', [IO.Path]::GetFullPath($InputUsd), '--output', $RunOutput, '--destination', $Destination) }
if ($MaterialXLibrary) { $Arguments += @('--library', [IO.Path]::GetFullPath($MaterialXLibrary)) }
if ($GraphJob) {
    & (Join-Path $PSScriptRoot 'Run-UsdPython.ps1') -Script (Join-Path $PSScriptRoot 'prepare_substrate.py') -ScriptArguments $Arguments
}
else {
    & uv run -- (Join-Path $HoudiniRoot 'bin\hython.exe') (Join-Path $PSScriptRoot 'prepare_substrate.py') @Arguments
}
if ($LASTEXITCODE) { throw 'Substrate graph export failed.' }
$Project = Join-Path $TaskRoot 'unreal\HdUEBackend\HdUEBackend.uproject'
if (-not $SkipBuild) { Build-HdUEEditor $EditorRoot *> (Join-Path $RunOutput 'build.log') }
$Editor = Join-Path $EditorRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Manifests = Get-Content -LiteralPath (Join-Path $RunOutput 'export.json') -Raw | ConvertFrom-Json
# Every graph of the job (each material and its mask-preview variant) in one editor: its start-up once, not per file.
$Compiled = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$Lines = foreach ($Manifest in $Manifests) {
    foreach ($File in @($Manifest.file, $Manifest.preview)) {
        if ($Compiled.Add($File)) { $File + '|' + [IO.Path]::ChangeExtension($File, '.receipt.json') }
    }
}
$List = Join-Path $RunOutput 'graphs.txt'
[IO.File]::WriteAllLines($List, [string[]]$Lines, [Text.UTF8Encoding]::new($false))
$Log = Join-Path $RunOutput 'substrate.log'
& $Editor $Project -run=HdUESubstrate "-GraphList=$List" -unattended -nop4 -nosplash -nullrhi -stdout *> $Log
if ($LASTEXITCODE) { throw "Substrate graph compilation failed: $Log" }
foreach ($Line in $Lines) {
    if (-not (Test-Path -LiteralPath $Line.Split('|')[1])) { throw "Substrate graph compilation wrote no receipt for $($Line.Split('|')[0]): $Log" }
}
if ((Get-Content -LiteralPath $Log -Raw) -match 'Log\w+: Error:|Fatal error') { throw "Substrate graph compilation reported an error: $Log" }
if ($GraphJob) {
    & (Join-Path $PSScriptRoot 'Run-UsdPython.ps1') -Script (Join-Path $PSScriptRoot 'prepare_substrate.py') -ScriptArguments @('registry', $RunOutput)
}
else {
    & uv run -- (Join-Path $HoudiniRoot 'bin\hython.exe') (Join-Path $PSScriptRoot 'prepare_substrate.py') registry $RunOutput
}
if ($LASTEXITCODE) { throw 'Substrate registry generation failed.' }
if (-not $SkipCook) {
    & $Editor $Project -run=Cook -TargetPlatform=Windows -Map=/Game/HdUETest -unattended -nop4 -nosplash -stdout *> (Join-Path $RunOutput 'cook.log')
    if ($LASTEXITCODE) { throw 'Substrate cook failed.' }
    # The cook compiles the shaders: UE's Substrate graph rules fail there, not in the commandlet (as Prepare-SubstrateLive.ps1).
    if ((Get-Content -LiteralPath (Join-Path $RunOutput 'cook.log') -Raw) -match 'Failed to compile Material') { throw "Substrate material compilation failed; see $(Join-Path $RunOutput 'cook.log')." }
}
Write-Output "Substrate library: $(Join-Path $RunOutput 'library.json')"
