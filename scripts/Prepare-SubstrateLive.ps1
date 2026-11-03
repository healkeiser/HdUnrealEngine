param(
    [Parameter(Mandatory = $true)][string]$GraphJob,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$Destination,
    [Parameter(Mandatory = $true)][string]$RuntimeProject
)
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
    Write-ProgressState 'waiting' 'Waiting for the Substrate preparation worker'
    $Deadline = [DateTime]::UtcNow.AddMinutes(15)
    while (-not $Owned) {
        try { $Owned = $Mutex.WaitOne(200) } catch [Threading.AbandonedMutexException] { $Owned = $true }
        if ([DateTime]::UtcNow -gt $Deadline) { throw 'Timed out waiting for the Substrate preparation worker.' }
    }
    Write-ProgressState 'compiling' 'Compiling and cooking the edited Substrate graphs'
    # One editor compiles every graph of the job and cooks them with inline shader code (-CookAfter), then one Python
    # writes the library and publishes the packages.
    $Project = Join-Path $TaskRoot 'unreal\HdUEBackend\HdUEBackend.uproject'
    $CookDirectory = Join-Path $TaskRoot ('unreal\HdUEBackend\Content\' + $Destination.Substring('/Game/'.Length))
    $CookArguments = @('-TargetPlatform=Windows', "-CookDir=$CookDirectory", '-CookSkipRequests', '-CookSkipHardRefs',
        "-OutputDir=$(Join-Path $RunOutput 'cooked\[Platform]')",
        '-ini:Game:[/Script/UnrealEd.ProjectPackagingSettings]:bShareMaterialShaderCode=False')
    & (Join-Path $PSScriptRoot 'Prepare-Substrate.ps1') -GraphJob $GraphJob -OutputDirectory $RunOutput -Destination $Destination -SkipBuild -SkipCook -SkipRegistry -CookArguments $CookArguments
    Write-ProgressState 'publishing' 'Publishing the completed materials to the active runtime'
    & (Join-Path $PSScriptRoot 'Run-UsdPython.ps1') -Script (Join-Path $PSScriptRoot 'prepare_substrate.py') -ScriptArguments @('finish', $RunOutput, '--runtime-project', $RuntimeProject, '--destination', $Destination)
    if ($LASTEXITCODE) { throw 'Live material publication failed.' }
    Write-ProgressState 'ready' 'The edited Substrate graph is ready'
}
catch {
    Write-ProgressState 'failed' $_.Exception.Message
    throw
}
finally {
    if ($Owned) { $Mutex.ReleaseMutex() }
    $Mutex.Dispose()
}
