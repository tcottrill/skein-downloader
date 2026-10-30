# Gathers the files for a GitHub release into dist/release/, named with the version from
# CMakeLists.txt, with SHA256SUMS.txt. Run after tools/build-windows.ps1,
# tools/build-installer.ps1 and the Linux builds (tools/build-linux-appimage.sh,
# tools/package-flatpak.sh), or use tools/build-all.ps1 for all of it. Missing packages are
# reported and skipped. Upload everything in dist/release/ to the release for the tag.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$dist = Join-Path $repo 'dist'
$version = [regex]::Match((Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw), 'project\(\w+ VERSION ([0-9.]+)').Groups[1].Value
if (!$version) { throw 'No VERSION in CMakeLists.txt' }
$out = Join-Path $dist 'release'
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory $out | Out-Null

# Windows: the program and the video tool installer it runs from the same folder.
$exe = Join-Path $dist 'skein-downloader.exe'
if (Test-Path -LiteralPath $exe) {
    $stage = Join-Path $out "SkeinDownloader-$version-windows-x64-portable"
    New-Item -ItemType Directory $stage | Out-Null
    Copy-Item -LiteralPath $exe, (Join-Path $repo 'tools/install-video-tools.ps1'), (Join-Path $repo 'LICENSE'), (Join-Path $repo 'README.md') -Destination $stage
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath "$stage.zip"
    Remove-Item -LiteralPath $stage -Recurse -Force
} else { Write-Warning "No Windows build ($exe)" }
$setup = Join-Path $dist "SkeinDownloader-$version-windows-x64-setup.exe"
if (Test-Path -LiteralPath $setup) { Copy-Item -LiteralPath $setup -Destination $out }
else { Write-Warning "No Windows installer ($setup): run tools/build-installer.ps1" }

# Linux packages, and the source of the bundled tools that must be published with them.
$linux = Join-Path $dist 'linux'
foreach ($package in @(@('SkeinDownloader-x86_64.AppImage', "SkeinDownloader-$version-linux-x86_64.AppImage"),
                       @('SkeinDownloader-x86_64.flatpak', "SkeinDownloader-$version-linux-x86_64.flatpak"))) {
    $file = Join-Path $linux $package[0]
    if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination (Join-Path $out $package[1]) }
    else { Write-Warning "No $($package[0]) in $linux" }
}
$sources = Join-Path $linux 'sources'
if (Test-Path -LiteralPath $sources) { Copy-Item -Path (Join-Path $sources '*') -Destination $out }
else { Write-Warning "No ${sources}: the Linux packages need their tool sources published with them" }

$sums = Get-ChildItem -LiteralPath $out -File | Sort-Object Name | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
}
Set-Content -LiteralPath (Join-Path $out 'SHA256SUMS.txt') -Value $sums -Encoding ascii
Get-ChildItem -LiteralPath $out | Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } }
