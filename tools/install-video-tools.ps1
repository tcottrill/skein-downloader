param([switch]$FunctionsOnly)
$ErrorActionPreference = 'Stop'
# A parent PowerShell 7 process can pass a module path that omits Windows
# PowerShell's built-ins. Load the two required system modules explicitly.
Import-Module "$PSHOME\Modules\Microsoft.PowerShell.Management\Microsoft.PowerShell.Management.psd1"
Import-Module "$PSHOME\Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1"
function Assert-AssetHash($Path, $Digest) {
    if ($Digest -notmatch '^sha256:([a-fA-F0-9]{64})$') { throw 'Release asset has no SHA-256 digest; installation stopped.' }
    $expected = $Matches[1]
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $expected) { throw 'Release checksum mismatch; installation stopped.' }
}
function Publish-Bundle($Root, $Name) {
    if ($Name -notmatch '^bundle-[a-f0-9-]+$') { throw 'Invalid bundle name' }
    $bundle = Join-Path $Root $Name
    foreach ($file in @('yt-dlp.exe','deno.exe','ffmpeg.exe','ffprobe.exe')) {
        $item = Get-Item -LiteralPath (Join-Path $bundle $file) -ErrorAction Stop
        if ($item.PSIsContainer -or $item.Length -eq 0) { throw 'Incomplete video tools bundle' }
    }
    $current = Join-Path $Root 'current.txt'
    $temp = Join-Path $Root 'current.new'
    [IO.File]::WriteAllText($temp, $Name)
    if ([IO.File]::Exists($current)) { [IO.File]::Replace($temp,$current,(Join-Path $Root 'previous.txt')) }
    else { [IO.File]::Move($temp,$current) }
}
function Get-ReleaseAsset($Repo, $Name, $Destination) {
    Write-Output "Checking $Repo..."
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ 'User-Agent'='SkeinDownloader' } -TimeoutSec 60
    $assets = @($release.assets | Where-Object { $_.name -eq $Name })
    if ($assets.Count -ne 1) { throw "Release asset missing: $Name" }
    $asset = $assets[0]
    if (!$asset.browser_download_url.StartsWith("https://github.com/$Repo/releases/download/")) { throw 'Unexpected release URL' }
    Write-Output "Downloading $Name ($([math]::Round($asset.size / 1MB)) MiB)..."
    Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -OutFile $Destination -TimeoutSec 600
    Assert-AssetHash $Destination $asset.digest
    $script:releases += [pscustomobject]@{ repository=$Repo; tag=$release.tag_name; asset=$Name; sha256=$asset.digest; url=$asset.browser_download_url; source=$release.tarball_url }
    Write-Output "Verified $Name."
}
function Extract-Executable($Zip, $Suffix, $Destination) {
    $archive = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $entries = @($archive.Entries | Where-Object { $_.FullName -eq $Suffix -or $_.FullName.EndsWith('/' + $Suffix) })
        if ($entries.Count -ne 1) { throw "Missing or ambiguous executable: $Suffix" }
        # Extract only the explicitly named file to our own destination. Never
        # interpret archive paths as filesystem paths.
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entries[0], $Destination, $false)
    } finally { $archive.Dispose() }
}
if ($FunctionsOnly) { return }
try {
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false
    $ProgressPreference = 'SilentlyContinue'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $root = Join-Path $PSScriptRoot 'video-tools'
    $name = 'bundle-' + [guid]::NewGuid().ToString('D')
    $bundle = Join-Path $root $name
    [IO.Directory]::CreateDirectory($bundle) | Out-Null
    $script:releases = @()
    Get-ReleaseAsset 'yt-dlp/yt-dlp' 'yt-dlp.exe' (Join-Path $bundle 'yt-dlp.exe')
    $denoZip = Join-Path $bundle 'deno.zip'
    Get-ReleaseAsset 'denoland/deno' 'deno-x86_64-pc-windows-msvc.zip' $denoZip
    Extract-Executable $denoZip 'deno.exe' (Join-Path $bundle 'deno.exe')
    Remove-Item -LiteralPath $denoZip
    $ffmpegZip = Join-Path $bundle 'ffmpeg.zip'
    Get-ReleaseAsset 'yt-dlp/FFmpeg-Builds' 'ffmpeg-master-latest-win64-gpl.zip' $ffmpegZip
    Extract-Executable $ffmpegZip 'bin/ffmpeg.exe' (Join-Path $bundle 'ffmpeg.exe')
    Extract-Executable $ffmpegZip 'bin/ffprobe.exe' (Join-Path $bundle 'ffprobe.exe')
    Remove-Item -LiteralPath $ffmpegZip
    $script:releases | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $bundle 'releases.json') -Encoding UTF8
    foreach ($notice in @(
        @('yt-dlp-LICENSE.txt','https://raw.githubusercontent.com/yt-dlp/yt-dlp/master/LICENSE'),
        @('yt-dlp-THIRD-PARTY.txt','https://raw.githubusercontent.com/yt-dlp/yt-dlp/master/THIRD_PARTY_LICENSES.txt'),
        @('deno-LICENSE.txt','https://raw.githubusercontent.com/denoland/deno/main/LICENSE.md'),
        @('ffmpeg-COPYING-GPLv3.txt','https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/COPYING.GPLv3')
    )) { Invoke-WebRequest -UseBasicParsing -Uri $notice[1] -OutFile (Join-Path $bundle $notice[0]) -TimeoutSec 60 }
    Publish-Bundle $root $name
    Write-Output 'Video tools installed. Ready to download.'
} catch { Write-Output ('Installation failed: ' + $_.Exception.Message); exit 1 }
