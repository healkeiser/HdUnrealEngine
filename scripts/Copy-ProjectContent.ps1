# Copies a UE project's content into hdUE's UE project (unreal\HdUEBackend\Content, or -Destination) for the import
# (Import-UEAssets.ps1, Prepare-ProjectAssets.ps1). New files are copied. A file hdUE already has is replaced only when
# the manifest (Saved\hdue-imported-content.json beside the destination) says the same project copied it and it still
# holds the bytes recorded then; hdUE's own assets (Content\HdUE) and files from other projects are never replaced.
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
$Manifest = if (Test-Path -LiteralPath $ManifestPath -PathType Leaf) { Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json -AsHashtable } else { @{} }
$Run = New-HdUERun -Name content-copy -Path $OutputDirectory -Create
# Preflight every collision before copying. robocopy lists the new files and the existing files whose size or time
# differ; those may only be replaced by the project that copied them, unchanged since.
$Added = @(); $Replace = @()
foreach ($Line in robocopy $SourceContent $Destination /E /L /XX /NJH /NJS /NDL /NP /NS /FP) {
    if ($Line -match '^\s*New File\s+(.+)$') { $Added += $Matches[1].Trim(); continue }
    if ($Line -notmatch '^\s*(Newer|Older|Changed)\s+(.+)$') { continue }
    $File = $Matches[2].Trim(); $Relative = [IO.Path]::GetRelativePath($SourceContent, $File); $Target = Join-Path $Destination $Relative
    $TargetHash = (Get-FileHash -LiteralPath $Target).Hash.ToLowerInvariant()
    if ((Get-FileHash -LiteralPath $File).Hash -eq $TargetHash) { continue }
    $Entry = $Manifest[$Relative]
    $Reason = if ($Relative -like 'HdUE\*') { "hdUE's own asset" }
    elseif (-not $Entry) { "it was not copied by an import, or before the manifest existed; delete it from $Destination to import this project's version" }
    elseif ($Entry.source_project -ne $SourceProject) { "copied from $($Entry.source_project)" }
    elseif ($Entry.sha256 -ne $TargetHash) { "changed in hdUE's workspace since it was copied" }
    if ($Reason) { throw "Different asset already exists: $Target ($Reason)" }
    $Replace += $File
}
if ($LASTEXITCODE -ge 8) { throw 'Cannot enumerate source content.' }
# New files (/XC /XN /XO: never an existing one); the log is the receipt.
& robocopy $SourceContent $Destination /E /XC /XN /XO /NP /NDL /FP "/LOG:$(Join-Path $Run 'robocopy.log')" | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Content copy failed; see $Run\robocopy.log." }
foreach ($File in $Replace) { Copy-Item -LiteralPath $File -Destination (Join-Path $Destination ([IO.Path]::GetRelativePath($SourceContent, $File))) -Force }
# Written beside, then moved: a reader never sees half a manifest. Only after the copy succeeded.
foreach ($File in $Added + $Replace) {
    $Relative = [IO.Path]::GetRelativePath($SourceContent, $File)
    $Manifest[$Relative] = @{ source_project = $SourceProject; sha256 = (Get-FileHash -LiteralPath (Join-Path $Destination $Relative)).Hash.ToLowerInvariant() }
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ManifestPath) | Out-Null
$Manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$ManifestPath.pending" -Encoding utf8
Move-Item -LiteralPath "$ManifestPath.pending" -Destination $ManifestPath -Force
@{source_project = $SourceProject; project_sha256 = (Get-FileHash -LiteralPath $SourceProject).Hash.ToLowerInvariant(); destination = $Destination } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Run 'copied-content.json') -Encoding utf8
$global:LASTEXITCODE = 0
Write-Output "Copied new content files into hdUE (receipt: $Run\robocopy.log); source project unchanged."
