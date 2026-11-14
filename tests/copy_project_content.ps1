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
    (Get-Item -LiteralPath $File).LastWriteTimeUtc = [DateTime]::UtcNow.AddSeconds($script:Count * 10)  # a differing size or time is what makes a file a candidate for replacement
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
    # A replacement that fails (the file is held open) still leaves the new files that were copied before it recorded.
    New-Project T
    Set-Asset T 'Meshes\SM_T1.uasset' 't1'
    Copy-Content T
    Set-Asset T 'Meshes\SM_T1.uasset' 't1 version 2'
    Set-Asset T 'Meshes\SM_T2.uasset' 't2'
    $Held = [IO.File]::Open((Join-Path $Destination 'Meshes\SM_T1.uasset'), 'Open', 'Read', 'Read')
    try { Copy-Content T; throw 'A held file was replaced' } catch { if ($_.Exception.Message -eq 'A held file was replaced') { throw } } finally { $Held.Dispose() }
    $Recorded = Get-Content -LiteralPath (Join-Path $Fixture 'Dest\Saved\hdue-imported-content.json') -Raw | ConvertFrom-Json -AsHashtable
    if (-not $Recorded.ContainsKey('Meshes\SM_T2.uasset')) { throw 'A new file copied before a failed replacement was not recorded' }
    # An import waits while another holds the manifest's lock, then keeps the other's entries.
    New-Project U
    Set-Asset U 'Meshes\SM_U.uasset' 'u'
    $Lock = [IO.File]::Open((Join-Path $Fixture 'Dest\Saved\hdue-imported-content.json.lock'), 'OpenOrCreate', 'ReadWrite', 'ReadWrite')
    $Lock.Lock(0, 1)
    $Job = Start-Job -ScriptBlock { & $using:Script -ProjectPath (Join-Path $using:Fixture 'U\U.uproject') -OutputDirectory (Join-Path $using:Fixture 'runU') -Destination $using:Destination }
    try {
        Start-Sleep -Seconds 3
        if (Test-Path -LiteralPath (Join-Path $Destination 'Meshes\SM_U.uasset')) { throw 'An import copied while another held the manifest lock' }
        $Recorded['Meshes\SM_Other.uasset'] = @{ source_project = 'other'; sha256 = '0' }
        $Recorded | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Fixture 'Dest\Saved\hdue-imported-content.json') -Encoding utf8
    }
    finally { $Lock.Dispose() }
    $Job | Wait-Job -Timeout 60 | Receive-Job | Out-Null
    $Recorded = Get-Content -LiteralPath (Join-Path $Fixture 'Dest\Saved\hdue-imported-content.json') -Raw | ConvertFrom-Json -AsHashtable
    if (-not ($Recorded.ContainsKey('Meshes\SM_U.uasset') -and $Recorded.ContainsKey('Meshes\SM_Other.uasset'))) { throw 'An import dropped the entries written while it waited for the lock' }
    if (Get-ChildItem -LiteralPath $Destination -Recurse -Filter '*.pending' | Where-Object Name -NotLike 'SM_T1*') { throw 'A completed copy left a .pending file' }
    Write-Output 'PASS content copy: own re-copy replaces; other projects, workspace edits, unrecorded files and HdUE content are refused; imports wait for the manifest lock'
}
finally {
    if (-not $Fixture.StartsWith([IO.Path]::GetTempPath(), [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture cleanup escaped the temp folder' }
    Remove-Item -LiteralPath $Fixture -Recurse -Force -ErrorAction SilentlyContinue
}
