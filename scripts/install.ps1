# Installs DittoSR into a folder: a game's (next to its FidelityFX DLL) or OptiScaler's. DittoSRLoader.exe does the
# same in a window.
#   install.ps1 -Dir <folder> [-Build <build folder>] [-Name amd_fidelityfx_dx12.dll | amd_fidelityfx_loader_dx12.dll | amd_fidelityfx_vk.dll]
# For each of amd_fidelityfx_dx12.dll (FidelityFX SDK 1.1, FSR 3.1), amd_fidelityfx_loader_dx12.dll (SDK 2.x),
# amd_fidelityfx_vk.dll (FSR 3.1 on Vulkan) and amd_fidelityfx_upscaler_dx12.dll where the folder has neither of the
# first two (an SDK 2.x game with the loader built into its executable) found in the folder, AMD's file is backed up
# to <folder>\dittosr-backup\ (with its SHA-256), copied to <name>.orig.dll (what DittoSR hands frame generation and the
# FSR toggle to) and replaced by DittoSR - the same DLL serves D3D12 and Vulkan. With none of them there, DittoSR goes
# in as -Name alone (OptiScaler, a game that loads it optionally). dittosr.ini goes next to it (kept if already
# there). Run it again after a rebuild: the backups and the originals' copies are left as they are.
param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [string]$Build = "",
    [string]$Name = "amd_fidelityfx_dx12.dll"
)
$ErrorActionPreference = "Stop"
# (Windows PowerShell 5.1 leaves $PSScriptRoot empty in the param block's defaults)
if (-not $Build) { $Build = Join-Path $PSScriptRoot "..\build" }
$dll = Join-Path $Build "amd_fidelityfx_dx12.dll"
if (-not (Test-Path $dll)) { throw "build DittoSR first (build.bat): $dll" }
if (-not (Test-Path $Dir)) { throw "not found: $Dir" }
$backup = Join-Path $Dir "dittosr-backup"
$names = @("amd_fidelityfx_dx12.dll", "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_vk.dll") | Where-Object { Test-Path (Join-Path $Dir $_) }
if (-not ($names -match "_dx12\.dll$") -and (Test-Path (Join-Path $Dir "amd_fidelityfx_upscaler_dx12.dll"))) {
    $names = @($names) + "amd_fidelityfx_upscaler_dx12.dll"  # (the game loads the upscaler itself)
}
if (-not $names) { $names = @($Name); Write-Host "no FidelityFX DLL in $Dir`: DittoSR alone as $Name" }
foreach ($n in $names) {
    $target = Join-Path $Dir $n
    $orig = Join-Path $Dir ([IO.Path]::GetFileNameWithoutExtension($n) + ".orig.dll")
    if ((Test-Path $target) -and -not (Test-Path $orig)) {
        # the first install: AMD's file, before anything replaces it
        $sig = Get-AuthenticodeSignature $target
        if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch "Advanced Micro Devices") {
            throw "$target is not AMD's (not signed by Advanced Micro Devices): restore the game's file first"
        }
        New-Item -ItemType Directory -Force $backup | Out-Null
        Copy-Item $target (Join-Path $backup $n)
        (Get-FileHash $target).Hash | Set-Content -Encoding ascii (Join-Path $backup "$n.sha256")
        Copy-Item $target $orig
        Write-Host "backed up $n to $backup, AMD's copy as $orig"
    }
    Copy-Item $dll $target -Force
    Write-Host "installed DittoSR as $target"
}
$ini = Join-Path $Dir "dittosr.ini"
if (-not (Test-Path $ini)) { Copy-Item (Join-Path $PSScriptRoot "..\dittosr.ini") $ini }
else {
    # (an ini from before 0.2.2 with the then default sharpness=0.2, a soft blur at rest: the new default, 0.6)
    $lines = Get-Content $ini
    if ($lines -match '^\s*sharpness\s*=\s*0\.2\s*$') {
        $lines -replace '^\s*sharpness\s*=\s*0\.2\s*$', 'sharpness=0.6' | Set-Content $ini -Encoding ascii
        Write-Host "settings: sharpness 0.2 (the old default) set to 0.6 (the new default)"
    }
}
Write-Host "settings: $ini; log: $(Join-Path $Dir 'dittosr.log')"
