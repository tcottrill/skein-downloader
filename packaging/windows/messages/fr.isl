; Skein Downloader's installer text in French. See en.isl.

[CustomMessages]
InternetTitle=Connexion Internet requise
InternetSubtitle=Le programme d'installation télécharge les outils vidéo utilisés par Skein Downloader.
InternetText=Ces programmes ne font pas partie de Skein Downloader. Le programme d'installation télécharge leurs dernières versions depuis GitHub lorsque vous cliquez sur Installer, et s'arrête sans rien installer s'il n'y parvient pas. Il télécharge :
ToolYtDlp=yt-dlp, le téléchargeur de vidéos (yt-dlp.exe, environ 22 Mo)
ToolDeno=Deno, l'environnement d'exécution JavaScript dont yt-dlp a besoin pour certains sites%n(deno-x86_64-pc-windows-msvc.zip, environ 40 Mo ; seul deno.exe est conservé)
ToolFFmpeg=FFmpeg et FFprobe, qui assemblent et convertissent l'audio et la vidéo%n(ffmpeg-master-latest-win64-gpl.zip, environ 190 Mo ; seuls ffmpeg.exe et ffprobe.exe sont conservés)
DownloadFrom=depuis %1
DownloadLicense=Licence : %1
LicenseYtDlp=Unlicense ; l'exécutable contient d'autres composants sous leurs propres licences
LicenseGpl3=GPL version 3
LicenseTexts=Leurs textes de licence, depuis %1
DownloadTotal=Téléchargement total : environ 255 Mo. Avant d'être utilisé, chaque fichier est vérifié avec la somme de contrôle SHA-256 que GitHub publie pour lui.
ProgressTitle=Téléchargement des outils vidéo
ProgressSubtitle=Le programme d'installation télécharge yt-dlp, Deno et FFmpeg depuis GitHub. Cela peut prendre quelques minutes.
ReadyDownloads=Téléchargements depuis GitHub (environ 255 Mo) :
ReadyFrom=%1 depuis %2
Connecting=Connexion à GitHub...
ChecksumNote=Les fichiers sont vérifiés avec leurs sommes de contrôle SHA-256 publiées.
PowerShellFailed=Le programme d'installation n'a pas pu lancer Windows PowerShell pour télécharger les outils vidéo.
DownloadIncomplete=Les outils vidéo ont été téléchargés mais sont incomplets. Cliquez sur Installer pour réessayer.
DownloadFailed=Le programme d'installation n'a pas pu télécharger les outils vidéo :%n%n%1%n%nVérifiez la connexion Internet, puis cliquez sur Installer pour réessayer.
DownloadTimeout=Le téléchargement des outils vidéo a pris plus de 30 minutes et a été interrompu. Vérifiez la connexion Internet, puis cliquez sur Installer pour réessayer.
DownloadNotPossible=Les outils vidéo (yt-dlp, Deno, FFmpeg) n'ont pas pu être téléchargés depuis GitHub.
