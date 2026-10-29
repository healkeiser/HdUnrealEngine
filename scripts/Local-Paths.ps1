# Machine-specific SDKs, scenes and runtime profiles belong in ignored local configuration.
function Get-HdUELocalPath {
    param(
        [Parameter(Mandatory=$true)][string]$Key,
        [string]$Value='',
        [switch]$Optional
    )
    if ($Value) { return [IO.Path]::GetFullPath($Value) }
    $Variables=@{
        engine_root='HDUE_ENGINE_ROOT'; editor_root='HDUE_EDITOR_ROOT';
        openusd_root='HDUE_OPENUSD_ROOT'; houdini_root='HDUE_HOUDINI_ROOT';
        runtime_config='HDUE_RUNTIME_CONFIG'; review_scene='HDUE_REVIEW_SCENE'
        houdini_build_root='HDUE_HOUDINI_BUILD_ROOT'
    }
    if (-not $Variables.ContainsKey($Key)) { throw "Unknown local path key: $Key" }
    $Resolved=[Environment]::GetEnvironmentVariable($Variables[$Key],'Process')
    if (-not $Resolved -and $Key -eq 'houdini_root') { $Resolved=$env:HFS }
    if (-not $Resolved) {
        $LocalFile=if ($env:HDUE_LOCAL_CONFIG) {$env:HDUE_LOCAL_CONFIG} else {Join-Path (Split-Path -Parent $PSScriptRoot) '.local\paths.json'}
        if (Test-Path -LiteralPath $LocalFile -PathType Leaf) {
            $Local=Get-Content -LiteralPath $LocalFile -Raw | ConvertFrom-Json
            $Resolved=$Local.$Key
            if ($Resolved -and -not [IO.Path]::IsPathRooted($Resolved)) {
                $Resolved=Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($LocalFile))) $Resolved
            }
        }
    }
    if ($Resolved) { return [IO.Path]::GetFullPath($Resolved) }
    # The one place that names the Houdini delegate's build folder.
    if ($Key -eq 'houdini_build_root') { return Join-Path (Split-Path -Parent $PSScriptRoot) 'build\houdini' }
    if ($Optional) { return '' }
    throw "Configure $Key in .local/paths.json, set $($Variables[$Key]), or pass the corresponding script parameter."
}

# A fresh run directory: $Path, else out\<Name>\<timestamp>. Its parent exists; -Create also creates it (otherwise
# the tool that fills it does, refusing an existing one).
function New-HdUERun {
    param([Parameter(Mandatory=$true)][string]$Name,[string]$Path='',[switch]$Create)
    $Run=if ($Path) {[IO.Path]::GetFullPath($Path)} else {Join-Path (Split-Path -Parent $PSScriptRoot) "out\$Name\$(Get-Date -Format 'yyyyMMdd-HHmmss-fff')"}
    if (Test-Path -LiteralPath $Run) {throw "Fresh evidence is required; $Run already exists."}
    New-Item -ItemType Directory -Force -Path $(if ($Create) {$Run} else {Split-Path -Parent $Run}) | Out-Null
    $Run
}

# The editor modules (HdUE commandlets) for UnrealEditor-Cmd.
function Build-HdUEEditor {
    param([Parameter(Mandatory=$true)][string]$EditorRoot)
    $Project=Join-Path (Split-Path -Parent $PSScriptRoot) 'unreal\HdUEBackend\HdUEBackend.uproject'
    & (Join-Path $EditorRoot 'Engine\Build\BatchFiles\Build.bat') HdUEBackendEditor Win64 Development "-Project=$Project" -gather -WaitMutex -NoHotReload
    if ($LASTEXITCODE) {throw 'HdUEBackendEditor build failed.'}
}

function Restore-HdUEEnvironment {
    param([Parameter(Mandatory=$true)][hashtable]$Snapshot)
    foreach ($Name in $Snapshot.Keys) {
        if ($null -eq $Snapshot[$Name]) {
            # Passing null through the .NET overload can create an empty value.
            # Houdini treats an empty SDK/preferences variable as a real setting.
            Remove-Item -LiteralPath "Env:$Name" -ErrorAction SilentlyContinue
        } else {
            [Environment]::SetEnvironmentVariable($Name,$Snapshot[$Name],'Process')
        }
    }
}
