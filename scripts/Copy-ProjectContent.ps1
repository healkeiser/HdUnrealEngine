# Copies a UE project's content into hdUE's UE project (unreal\HdUEBackend\Content, or -Destination) for the import
# (Import-UEAssets.ps1, Prepare-ProjectAssets.ps1). New files are copied. A file hdUE already has is replaced only when
# the manifest (Saved\hdue-imported-content.json beside the destination) says the same project copied it and it still
# holds the bytes recorded then; hdUE's own assets (Content\HdUE) and files from other projects are never replaced.
#Requires -Version 7
param(
    [Parameter(Mandatory = $true)][string]$ProjectPath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Destination = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$TaskRoot = Split-Path -Parent $PSScriptRoot
$SourceProject = Resolve-HdUEUProject $ProjectPath
$SourceContent = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $SourceProject) 'Content'))
$Destination = [IO.Path]::GetFullPath($(if ($Destination) { $Destination } else { Join-Path $TaskRoot 'unreal\HdUEBackend\Content' }))
if ($SourceContent.TrimEnd('\') -eq $Destination.TrimEnd('\')) { throw 'Source and destination content must be different.' }
$ManifestPath = Join-Path (Split-Path -Parent $Destination) 'Saved\hdue-imported-content.json'
$Run = New-HdUERun -Name content-copy -Path $OutputDirectory -Create
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ManifestPath) | Out-Null
# Two imports at once would each rewrite the manifest from their own copy and drop the other's entries: its
# read-modify-write holds a lock on byte 0 of a file beside it, as the asset catalog's does (hdue_ue_import._locked).
$Lock = [IO.File]::Open("$ManifestPath.lock", 'OpenOrCreate', 'ReadWrite', 'ReadWrite')
$Waited = [Diagnostics.Stopwatch]::StartNew()
while ($true) {
    try { $Lock.Lock(0, 1); break }
    catch [IO.IOException] {
        if ($Waited.Elapsed.TotalMinutes -ge 10) { $Lock.Dispose(); throw "Another import holds $ManifestPath.lock." }
        Start-Sleep -Milliseconds 200
    }
}
try {
    $Manifest = if (Test-Path -LiteralPath $ManifestPath -PathType Leaf) { Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json -AsHashtable } else { @{} }
    # Preflight every collision before copying: a file hdUE has whose size or time differs from the source's (and
    # whose bytes differ) may only be replaced by the project that copied it, unchanged since. No tool output is
    # parsed, so this does not depend on the Windows language.
    $Added = @(); $Replace = @()
    foreach ($Item in Get-ChildItem -LiteralPath $SourceContent -Recurse -File) {
        $Relative = [IO.Path]::GetRelativePath($SourceContent, $Item.FullName); $Target = Join-Path $Destination $Relative
        if (-not (Test-Path -LiteralPath $Target -PathType Leaf)) { $Added += $Relative; continue }
        $TargetItem = Get-Item -LiteralPath $Target
        if ($TargetItem.Length -eq $Item.Length -and $TargetItem.LastWriteTimeUtc -eq $Item.LastWriteTimeUtc) { continue }
        $TargetHash = (Get-FileHash -LiteralPath $Target).Hash.ToLowerInvariant()
        if ((Get-FileHash -LiteralPath $Item.FullName).Hash -eq $TargetHash) { continue }
        $Entry = $Manifest[$Relative]
        $Reason = if ($Relative -like 'HdUE\*') { "hdUE's own asset" }
        elseif (-not $Entry) { "it was not copied by an import, or before the manifest existed; delete it from $Destination to import this project's version" }
        elseif ($Entry.source_project -ne $SourceProject) { "copied from $($Entry.source_project)" }
        elseif ($Entry.sha256 -ne $TargetHash) { "changed in hdUE's workspace since it was copied" }
        if ($Reason) { throw "Different asset already exists: $Target ($Reason)" }
        $Replace += $Relative
    }
    # Each file is copied beside its target, moved into place and recorded at once: a cancelled import (hdue_ue_import's
    # taskkill /F skips any finally) leaves no half-written target and no copied file unrecorded. The manifest is
    # written beside, then moved: a reader never sees half of it.
    foreach ($Relative in $Added + $Replace) {
        $Target = Join-Path $Destination $Relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Target) | Out-Null
        Copy-Item -LiteralPath (Join-Path $SourceContent $Relative) -Destination "$Target.pending" -Force
        Move-Item -LiteralPath "$Target.pending" -Destination $Target -Force
        $Manifest[$Relative] = @{ source_project = $SourceProject; sha256 = (Get-FileHash -LiteralPath $Target).Hash.ToLowerInvariant() }
        $Manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$ManifestPath.pending" -Encoding utf8
        Move-Item -LiteralPath "$ManifestPath.pending" -Destination $ManifestPath -Force
    }
}
finally { $Lock.Dispose() }
Set-Content -LiteralPath (Join-Path $Run 'copied-files.txt') -Value (@($Added | ForEach-Object { "new      $_" }) + @($Replace | ForEach-Object { "replaced $_" })) -Encoding utf8
@{source_project = $SourceProject; project_sha256 = (Get-FileHash -LiteralPath $SourceProject).Hash.ToLowerInvariant(); destination = $Destination } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Run 'copied-content.json') -Encoding utf8
$global:LASTEXITCODE = 0
Write-Output "Copied $($Added.Count) new and replaced $($Replace.Count) content files in hdUE (list: $Run\copied-files.txt); source project unchanged."
