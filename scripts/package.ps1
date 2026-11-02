# Packs a build into build\DittoSR.zip: the loader (DittoSRLoader.exe) at its top, next to it build\ for DittoSR's
# binaries, scripts\, the ini and the documents - the repository's layout, so the loader, install.ps1, install.sh and
# bench.ps1 run from it as they are: copy the zip to another PC (a ROG Ally, a Steam Deck), extract it, and run
# DittoSRLoader.exe (or a script) from the extracted folder.
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$build = Join-Path $root "build"
$stage = Join-Path $build "package\DittoSR"
if (Test-Path (Join-Path $build "package")) { Remove-Item -Recurse -Force (Join-Path $build "package") }
New-Item -ItemType Directory -Force (Join-Path $stage "build"), (Join-Path $stage "scripts"), (Join-Path $stage "licenses") | Out-Null
foreach ($f in "amd_fidelityfx_dx12.dll", "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_vk.dll", "libxess.dll", "harness.exe") {
    $p = Join-Path $build $f
    if (-not (Test-Path $p)) { throw "build first (build.bat): $p" }
    Copy-Item $p (Join-Path $stage "build")
}
$loader = Join-Path $build "DittoSRLoader.exe"
if (-not (Test-Path $loader)) { throw "build the loader first (loader\build.bat): $loader" }
Copy-Item $loader $stage
foreach ($f in "install.ps1", "uninstall.ps1", "install.sh", "uninstall.sh", "bench.ps1") { Copy-Item (Join-Path $root "scripts\$f") (Join-Path $stage "scripts") }
foreach ($f in "dittosr.ini", "README.md", "LICENSE", "THIRD-PARTY-NOTICES.md") { Copy-Item (Join-Path $root $f) $stage }
Copy-Item (Join-Path $root "licenses\*") (Join-Path $stage "licenses")
New-Item -ItemType Directory -Force (Join-Path $stage "licenses\ffx_api") | Out-Null
Copy-Item (Join-Path $root "src\ffx_api\LICENSE.txt") (Join-Path $stage "licenses\ffx_api\LICENSE.txt")
$zip = Join-Path $build "DittoSR.zip"
if (Test-Path $zip) { Remove-Item $zip }
# (not Compress-Archive: Windows PowerShell 5.1's stores the paths with backslashes, which Linux's extractors - SteamOS's
# Ark - take as part of a file name)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$base = Split-Path $stage
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in Get-ChildItem -Recurse -File $stage) {
        $entry = $f.FullName.Substring($base.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $archive.Dispose() }
Write-Host "packed $zip"
