# Measures DittoSR's GPU time on this machine (for an integrated GPU such as a ROG Ally's Radeon 780M): the harness at
# 1080p and 1440p output, at the ratios of FSR's Quality (1.5x) and Performance (2x), for each history format and
# shader precision. Prints a table and saves it to bench-results.txt next to this script.
#   bench.ps1 [-Bin <folder with harness.exe and amd_fidelityfx_dx12.dll>] [-Frames 300]
param(
    [string]$Bin = "",
    [int]$Frames = 300
)
$ErrorActionPreference = "Stop"
# (Windows PowerShell 5.1 leaves $PSScriptRoot empty in the param block's defaults)
if (-not $Bin) { $Bin = Join-Path $PSScriptRoot "..\build" }
$harness = Join-Path $Bin "harness.exe"
$dll = Join-Path $Bin "amd_fidelityfx_dx12.dll"
$ini = Join-Path $PSScriptRoot "..\dittosr.ini"
if (-not (Test-Path $ini)) { $ini = Join-Path $Bin "dittosr.ini" }
foreach ($f in $harness, $dll, $ini) { if (-not (Test-Path $f)) { throw "not found: $f" } }
$work = Join-Path ([IO.Path]::GetTempPath()) "dittosr-bench"
New-Item -ItemType Directory -Force $work | Out-Null
Copy-Item $dll $work -Force
# (the shipped settings with the performance switches under [dittosr]: the advanced keys are read from there)
$base = @("[dittosr]") + (Get-Content $ini | Where-Object { $_ -match "^(sharpness|frame_weight)=" })
$rows = @()
# (a first run thrown away: the GPU leaves its idle clocks)
($base + "history_10bit=1" + "fp16=0") | Set-Content -Encoding ascii (Join-Path $work "dittosr.ini")
& $harness $work (Join-Path $work "o") scene=grass psnr=0 frames=300 out=1920x1080 ratio=2 | Out-Null
foreach ($c in @(@{h = 0; f = 0; n = "RGBA16F, FP32"}, @{h = 1; f = 0; n = "RGB10A2, FP32"}, @{h = 0; f = 1; n = "RGBA16F, FP16"}, @{h = 1; f = 1; n = "RGB10A2, FP16"})) {
    ($base + "history_10bit=$($c.h)" + "fp16=$($c.f)") | Set-Content -Encoding ascii (Join-Path $work "dittosr.ini")
    foreach ($o in "1920x1080", "2560x1440") {
        foreach ($r in "1.5", "2") {
            $out = & $harness $work (Join-Path $work "o") scene=grass dense=2 vel=5 psnr=0 "frames=$Frames" "out=$o" "ratio=$r" | Select-Object -Last 1
            $ms = if ($out -match '([0-9.]+) ms GPU') { $Matches[1] } else { "failed: $out" }
            $rows += [pscustomobject]@{ Setup = $c.n; Output = $o; Ratio = "${r}x"; "GPU ms" = $ms }
            Write-Host "$($c.n)  $o  ${r}x  $ms ms"
        }
    }
}
$gpu = (Select-String -Path (Join-Path $work "dittosr.log") -Pattern "GPU: (.*)" | Select-Object -First 1).Matches.Groups[1].Value
$fp16 = (Select-String -Path (Join-Path $work "dittosr.log") -Pattern "FP16 shaders: (.*)" | Select-Object -First 1).Matches.Groups[1].Value
$report = "DittoSR GPU time (median of $Frames upscales), $gpu - FP16 shaders: $fp16`n" + ($rows | Format-Table -AutoSize | Out-String)
$report | Set-Content -Encoding utf8 (Join-Path $PSScriptRoot "bench-results.txt")
Write-Host $report
Write-Host "saved to $(Join-Path $PSScriptRoot 'bench-results.txt')"
