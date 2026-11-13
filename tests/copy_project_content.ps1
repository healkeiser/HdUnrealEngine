# Copy-ProjectContent.ps1 against temporary projects and a temporary destination (never hdUE's real Content).
$ErrorActionPreference = 'Stop'
$Script = Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts\Copy-ProjectContent.ps1'
$Fixture = Join-Path ([IO.Path]::GetTempPath()) ('copy-content-test-' + [guid]::NewGuid().ToString('N'))
$Destination = Join-Path $Fixture 'Dest\Content'
$Count = 0
function New-Project([string]$Name) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Fixture "$Name\Content") | Out-Null
    New-Item -ItemType File -Force -Path (Join-Path $Fixture "$Name\$Name.uproject") | Out-Null
}
function Set-Asset([string]$Project, [string]$Relative, [string]$Text) {
    $File = Join-Path $Fixture "$Project\Content\$Relative"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $File) | Out-Null
    Set-Content -LiteralPath $File -Value $Text -NoNewline
    $script:Count++
    (Get-Item -LiteralPath $File).LastWriteTimeUtc = [DateTime]::UtcNow.AddSeconds($script:Count * 10)  # robocopy compares times
}
function Copy-Content([string]$Project) {
    $script:Count++
    & $Script -ProjectPath (Join-Path $Fixture "$Project\$Project.uproject") -OutputDirectory (Join-Path $Fixture "run$script:Count") -Destination $Destination | Out-Null
}
function Assert-Refusal([string]$Project, [string]$Reason) {
    try { Copy-Content $Project }
    catch {
        if ($_.Exception.Message -notlike "*Different asset already exists*$Reason*") { throw "Wrong refusal for ${Project}: $($_.Exception.Message)" }
        return
    }
    throw "$Project was allowed to replace a file ($Reason)"
}
function Assert-Content([string]$Relative, [string]$Text) {
    $Actual = Get-Content -LiteralPath (Join-Path $Destination $Relative) -Raw
    if ($Actual -ne $Text) { throw "$Relative holds '$Actual', expected '$Text'" }
}
try {
    New-Project P; New-Project Q
    Set-Asset P 'Meshes\SM_A.uasset' 'v1'
    Copy-Content P
    Assert-Content 'Meshes\SM_A.uasset' 'v1'
    Set-Asset P 'Meshes\SM_A.uasset' 'version 2'
    Copy-Content P
    Assert-Content 'Meshes\SM_A.uasset' 'version 2'
    Set-Asset Q 'Meshes\SM_A.uasset' 'q'
    Assert-Refusal Q 'copied from'
    Assert-Content 'Meshes\SM_A.uasset' 'version 2'
    Set-Content -LiteralPath (Join-Path $Destination 'Meshes\SM_A.uasset') -Value 'edited here' -NoNewline
    Set-Asset P 'Meshes\SM_A.uasset' 'version 3 of it'
    Assert-Refusal P "changed in hdUE's workspace"
    Assert-Content 'Meshes\SM_A.uasset' 'edited here'
    # A file hdUE has that no import recorded (its own, or copied before the manifest) is refused.
    New-Project S
    Set-Asset S 'Meshes\SM_B.uasset' 'from S'
    Set-Content -LiteralPath (Join-Path $Destination 'Meshes\SM_B.uasset') -Value 'foreign' -NoNewline
    Assert-Refusal S 'not copied by an import'
    # hdUE's own content is never replaced, even by the project that put it there.
    New-Project R
    Set-Asset R 'HdUE\M_Own.uasset' 'own 1'
    Copy-Content R
    Assert-Content 'HdUE\M_Own.uasset' 'own 1'
    Set-Asset R 'HdUE\M_Own.uasset' 'own version 2'
    Assert-Refusal R "hdUE's own asset"
    Write-Output 'PASS content copy: own re-copy replaces; other projects, workspace edits, unrecorded files and HdUE content are refused'
}
finally {
    if (-not $Fixture.StartsWith([IO.Path]::GetTempPath(), [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture cleanup escaped the temp folder' }
    Remove-Item -LiteralPath $Fixture -Recurse -Force -ErrorAction SilentlyContinue
}
