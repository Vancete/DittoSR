# Removes DittoSR from a folder install.ps1 installed it into: AMD's files back from <folder>\dittosr-backup\ (after a
# hash check), DittoSR's own files removed.
#   uninstall.ps1 -Dir <folder>
param([Parameter(Mandatory = $true)][string]$Dir)
$ErrorActionPreference = "Stop"
$backup = Join-Path $Dir "dittosr-backup"
foreach ($n in "amd_fidelityfx_dx12.dll", "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_vk.dll") {
    $target = Join-Path $Dir $n
    $saved = Join-Path $backup $n
    $orig = Join-Path $Dir ([IO.Path]::GetFileNameWithoutExtension($n) + ".orig.dll")
    if (Test-Path $saved) {
        $want = (Get-Content "$saved.sha256").Trim()
        if ((Get-FileHash $saved).Hash -ne $want) { throw "the backup of $n does not match its hash: verify the game's files in its launcher instead" }
        Copy-Item $saved $target -Force
        if (Test-Path $orig) { Remove-Item $orig }
        Write-Host "restored AMD's $n"
    } elseif ((Test-Path $target) -and (Get-AuthenticodeSignature $target).Status -ne "Valid") {
        # (installed alone - no AMD file was there: DittoSR's, unsigned)
        Remove-Item $target
        Write-Host "removed DittoSR's $n"
    }
}
foreach ($f in "dittosr.ini", "dittosr.log") {
    $p = Join-Path $Dir $f
    if (Test-Path $p) { Remove-Item $p }
}
Write-Host "done (the backups, if any, stay in $backup)"
