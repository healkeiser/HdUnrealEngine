param(
    [Parameter(Mandatory = $true)][string]$GraphJob,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$Destination,
    [Parameter(Mandatory = $true)][string]$RuntimeProject
)
# One live preparation job (live_materials.LivePreparation): its Substrate Builder graphs and its MaterialX graphs in
# one editor (UE's MaterialX importer, the HdUESubstrate compiler, then UE's cook with inline shader code), between two
# Python steps (export; library and publication into the running runtime).
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$TaskRoot = Split-Path -Parent $PSScriptRoot
$RunOutput = [IO.Path]::GetFullPath($OutputDirectory)
$ProgressFile = Join-Path (Split-Path -Parent $RunOutput) 'progress.json'
function Write-ProgressState([string]$Phase, [string]$Message) {
    $Json = @{phase = $Phase; message = $Message } | ConvertTo-Json
    $Temporary = $ProgressFile + '.pending'
    [IO.File]::WriteAllText($Temporary, $Json, [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $Temporary -Destination $ProgressFile -Force
}
$Hash = [Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($TaskRoot.ToLowerInvariant()))
$Name = 'Local\hdUE_Substrate_' + ([BitConverter]::ToString($Hash).Replace('-', '').Substring(0, 24))
$Mutex = [Threading.Mutex]::new($false, $Name); $Owned = $false
try {
    Write-ProgressState 'waiting' 'Waiting for the material preparation worker'
    $Deadline = [DateTime]::UtcNow.AddMinutes(15)
    while (-not $Owned) {
        try { $Owned = $Mutex.WaitOne(200) } catch [Threading.AbandonedMutexException] { $Owned = $true }
        if ([DateTime]::UtcNow -gt $Deadline) { throw 'Timed out waiting for the material preparation worker.' }
    }
    Write-ProgressState 'compiling' 'Preparing and cooking the edited materials'
    $Script = Join-Path $PSScriptRoot 'live_materials.py'
    & (Join-Path $PSScriptRoot 'Run-UsdPython.ps1') -Script $Script -ScriptArguments @('export', [IO.Path]::GetFullPath($GraphJob), '--output', $RunOutput, '--destination', $Destination)
    $Lines = foreach ($Manifest in @(Get-Content -LiteralPath (Join-Path $RunOutput 'export.json') -Raw | ConvertFrom-Json)) {
        # A batch render's job has no mask-preview graphs (prepare_substrate.export_records).
        foreach ($File in @($Manifest.file, $Manifest.preview)) { if ($File) { $File + '|' + [IO.Path]::ChangeExtension($File, '.receipt.json') } }
    }
    # Two Builder materials with one graph share its export: compile it once.
    $Lines = @($Lines | Select-Object -Unique)
    $List = Join-Path $RunOutput 'graphs.txt'
    [IO.File]::WriteAllLines($List, [string[]]@($Lines), [Text.UTF8Encoding]::new($false))
    $MaterialX = @()
    $MaterialXExport = Join-Path $RunOutput 'materialx\export.json'
    if (Test-Path -LiteralPath $MaterialXExport) {
        $Source = (Get-Content -LiteralPath $MaterialXExport -Raw | ConvertFrom-Json).source
        $MaterialX = @("-MaterialXSource=$Source", "-MaterialXDestination=$Destination/MX", "-MaterialXReport=$(Join-Path $RunOutput 'materialx\import.json')")
    }
    $EditorRoot = Get-HdUELocalPath editor_root
    $Editor = Join-Path $EditorRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    $Project = Join-Path $TaskRoot 'unreal\HdUEBackend\HdUEBackend.uproject'
    $CookDirectory = Join-Path $TaskRoot ('unreal\HdUEBackend\Content\' + $Destination.Substring('/Game/'.Length))
    $Log = Join-Path $RunOutput 'editor.log'
    & $Editor $Project -run=HdUESubstrate "-GraphList=$List" @MaterialX -RunAsCookCommandlet -CookAfter -TargetPlatform=Windows "-CookDir=$CookDirectory" -CookSkipRequests -CookSkipHardRefs "-OutputDir=$(Join-Path $RunOutput 'cooked\[Platform]')" '-ini:Game:[/Script/UnrealEd.ProjectPackagingSettings]:bShareMaterialShaderCode=False' -unattended -nop4 -nosplash -nullrhi -stdout *> $Log
    if ($LASTEXITCODE) { throw "Material preparation failed: $Log" }
    $Text = Get-Content -LiteralPath $Log -Raw
    # The cook compiles the shaders: UE's Substrate graph rules fail there.
    if ($Text -match 'Failed to compile Material|Log\w+: Error:|Fatal error') { throw "Material preparation reported an error: $Log" }
    foreach ($Line in $Lines) {
        if (-not (Test-Path -LiteralPath $Line.Split('|')[1])) { throw "Substrate graph compilation wrote no receipt for $($Line.Split('|')[0]): $Log" }
    }
    Write-ProgressState 'publishing' 'Publishing the completed materials to the active runtime'
    & (Join-Path $PSScriptRoot 'Run-UsdPython.ps1') -Script $Script -ScriptArguments @('finish', $RunOutput, '--runtime-project', $RuntimeProject, '--destination', $Destination)
    Write-ProgressState 'ready' 'The edited materials are ready'
}
catch {
    Write-ProgressState 'failed' $_.Exception.Message
    throw
}
finally {
    if ($Owned) { $Mutex.ReleaseMutex() }
    $Mutex.Dispose()
}
