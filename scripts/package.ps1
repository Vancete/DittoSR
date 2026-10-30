# Packs a build into build\DittoSR.zip, laid out as the repository (build\ for the binaries, scripts\, the ini and
# the documents), so install.ps1 and bench.ps1 run from it as they are: copy the zip to another PC (a ROG Ally),
# extract it, and run scripts\bench.ps1 or scripts\install.ps1 from the extracted folder.
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$build = Join-Path $root "build"
$stage = Join-Path $build "package\DittoSR"
if (Test-Path (Join-Path $build "package")) { Remove-Item -Recurse -Force (Join-Path $build "package") }
New-Item -ItemType Directory -Force (Join-Path $stage "build"), (Join-Path $stage "scripts"), (Join-Path $stage "licenses") | Out-Null
foreach ($f in "amd_fidelityfx_dx12.dll", "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_vk.dll", "harness.exe") {
    $p = Join-Path $build $f
    if (-not (Test-Path $p)) { throw "build first (build.bat): $p" }
    Copy-Item $p (Join-Path $stage "build")
}
foreach ($f in "install.ps1", "uninstall.ps1", "bench.ps1") { Copy-Item (Join-Path $root "scripts\$f") (Join-Path $stage "scripts") }
foreach ($f in "dittosr.ini", "README.md", "LICENSE", "THIRD-PARTY-NOTICES.md") { Copy-Item (Join-Path $root $f) $stage }
Copy-Item (Join-Path $root "licenses\*") (Join-Path $stage "licenses")
New-Item -ItemType Directory -Force (Join-Path $stage "licenses\ffx_api") | Out-Null
Copy-Item (Join-Path $root "src\ffx_api\LICENSE.txt") (Join-Path $stage "licenses\ffx_api\LICENSE.txt")
$zip = Join-Path $build "DittoSR.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "packed $zip"
