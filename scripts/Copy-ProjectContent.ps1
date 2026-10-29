param([Parameter(Mandatory = $true)][string]$ProjectPath, [Parameter(Mandatory = $true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Local-Paths.ps1')
$TaskRoot = Split-Path -Parent $PSScriptRoot
$SourceProject = if (Test-Path -LiteralPath $ProjectPath -PathType Container) {
    $Projects = @(Get-ChildItem -LiteralPath $ProjectPath -Filter *.uproject)
    if ($Projects.Count -ne 1) { throw 'Specify one .uproject file.' }
    $Projects[0].FullName
}
else { (Resolve-Path -LiteralPath $ProjectPath).Path }
$SourceContent = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $SourceProject) 'Content'))
$Destination = [IO.Path]::GetFullPath((Join-Path $TaskRoot 'unreal\HdUEBackend\Content'))
if ($SourceContent.TrimEnd('\') -eq $Destination.TrimEnd('\')) { throw 'Source and destination content must be different.' }
$Run = New-HdUERun -Name content-copy -Path $OutputDirectory -Create
# Preflight every collision before copying; never overwrite workspace assets. robocopy lists the existing files whose
# size or time differ; those must still hold the same bytes.
foreach ($Line in robocopy $SourceContent $Destination /E /L /XX /NJH /NJS /NDL /NP /NS /FP) {
    if ($Line -notmatch '^\s*(Newer|Older|Changed)\s+(.+)$') { continue }
    $File = $Matches[2].Trim(); $Target = Join-Path $Destination ([IO.Path]::GetRelativePath($SourceContent, $File))
    if ((Get-FileHash -LiteralPath $File).Hash -ne (Get-FileHash -LiteralPath $Target).Hash) { throw "Different asset already exists: $Target" }
}
if ($LASTEXITCODE -ge 8) { throw 'Cannot enumerate source content.' }
# Only new files (/XC /XN /XO); the log is the receipt.
& robocopy $SourceContent $Destination /E /XC /XN /XO /NP /NDL /FP "/LOG:$(Join-Path $Run 'robocopy.log')" | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Content copy failed; see $Run\robocopy.log." }
@{source_project = $SourceProject; project_sha256 = (Get-FileHash -LiteralPath $SourceProject).Hash.ToLowerInvariant(); destination = $Destination } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Run 'copied-content.json') -Encoding utf8
$global:LASTEXITCODE = 0
Write-Output "Copied new content files into hdUE (receipt: $Run\robocopy.log); source project unchanged."
