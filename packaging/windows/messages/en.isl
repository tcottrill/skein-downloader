; Skein Downloader's own installer text in English, for skein-downloader.iss. Every file in
; this folder has the same names; tools\translations.py checks them. The script fills in
; %1 and %2; %n starts a new line. Refer to the Install button by the label the language's
; Inno Setup messages give it.

[CustomMessages]
InternetTitle=Internet connection required
InternetSubtitle=Setup downloads the video tools Skein Downloader uses.
InternetText=These programs are not part of Skein Downloader. Setup downloads their latest releases from GitHub when you click Install, and stops without installing anything if it cannot. Setup downloads:
ToolYtDlp=yt-dlp, the video downloader (yt-dlp.exe, about 22 MB)
ToolDeno=Deno, the JavaScript runtime yt-dlp needs for some sites%n(deno-x86_64-pc-windows-msvc.zip, about 40 MB; deno.exe is kept)
ToolFFmpeg=FFmpeg and FFprobe, which join and convert audio and video%n(ffmpeg-master-latest-win64-gpl.zip, about 190 MB; ffmpeg.exe and ffprobe.exe are kept)
DownloadFrom=from %1
DownloadLicense=License: %1
LicenseYtDlp=Unlicense; the executable contains other components under their own licenses
LicenseGpl3=GPL version 3
LicenseTexts=Their license texts, from %1
DownloadTotal=Total download: about 255 MB. Each file is checked against the SHA-256 checksum GitHub publishes for it before it is used.
ProgressTitle=Downloading video tools
ProgressSubtitle=Setup is downloading yt-dlp, Deno and FFmpeg from GitHub. This can take a few minutes.
ReadyDownloads=Downloads from GitHub (about 255 MB):
ReadyFrom=%1 from %2
Connecting=Connecting to GitHub...
ChecksumNote=Files are checked against their published SHA-256 checksums.
PowerShellFailed=Setup could not start Windows PowerShell to download the video tools.
DownloadIncomplete=The video tools were downloaded but are incomplete. Click Install to try again.
DownloadFailed=Setup could not download the video tools:%n%n%1%n%nCheck the internet connection, then click Install to try again.
DownloadTimeout=Downloading the video tools took longer than 30 minutes and was stopped. Check the internet connection, then click Install to try again.
DownloadNotPossible=The video tools (yt-dlp, Deno, FFmpeg) could not be downloaded from GitHub.
