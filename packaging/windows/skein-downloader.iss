; Inno Setup script for the Windows installer, SkeinDownloader-<version>-windows-x64-setup.exe.
; Build it with tools\build-installer.ps1 after tools\build-windows.ps1. It is written to
; build\installer; tools\collect-release.ps1 copies it to dist\release with the other downloads.
;
; Installs for the current user (no administrator rights) into
; %LOCALAPPDATA%\Programs\Skein Downloader, where the program can update its video tools.
; Setup needs an internet connection: on the Ready page it downloads the video tools
; (yt-dlp, Deno, FFmpeg) with install-video-tools.ps1, which checks each file against the
; SHA-256 that GitHub publishes for it, and installs nothing if that fails. The wizard
; lists what is downloaded and from where before anything is fetched.

#define Root AddBackslash(SourcePath) + "..\.."
#define AppExe Root + "\dist\skein-downloader.exe"
#if !FileExists(AppExe)
  #error dist\skein-downloader.exe is missing: run tools\build-windows.ps1 first
#endif
#define AppVersion GetStringFileInfo(AppExe, "ProductVersion")

[Setup]
AppId={{BCABA400-C5ED-4A7A-9E00-D0E460D7B90F}
AppName=Skein Downloader
AppVersion={#AppVersion}
AppVerName=Skein Downloader {#AppVersion}
AppPublisher=Tim Cottrill
AppPublisherURL=https://github.com/tcottrill/skein-downloader
AppSupportURL=https://github.com/tcottrill/skein-downloader/issues
AppCopyright=Copyright (C) 2026 Tim Cottrill. GPL-3.0-or-later.
DefaultDirName={autopf}\Skein Downloader
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupIconFile={#Root}\packaging\icons\skein-downloader.ico
UninstallDisplayIcon={app}\skein-downloader.exe
UninstallDisplayName=Skein Downloader
VersionInfoVersion={#AppVersion}
OutputDir={#Root}\build\installer
OutputBaseFilename=SkeinDownloader-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes

[Tasks]
Name: desktopicon; Description: "Create a &desktop shortcut"; Flags: unchecked

[InstallDelete]
; A fresh set of video tools replaces the old one on every install or upgrade.
Type: filesandordirs; Name: "{app}\video-tools"

[Files]
; Run from {tmp} on the Ready page (see DownloadVideoTools) and installed for the program's
; own Install/Repair button.
Source: "{#Root}\tools\install-video-tools.ps1"; Flags: dontcopy
Source: "{#Root}\tools\install-video-tools.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#Root}\README.md"; DestDir: "{app}"; Flags: ignoreversion
; The video tools downloaded into {tmp}\video-tools before the files are copied.
Source: "{tmp}\video-tools\*"; DestDir: "{app}\video-tools"; Flags: external recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Skein Downloader"; Filename: "{app}\skein-downloader.exe"
Name: "{autodesktop}\Skein Downloader"; Filename: "{app}\skein-downloader.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\skein-downloader.exe"; Description: "Start Skein Downloader"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; yt-dlp updates itself and the program can reinstall the tools, so this folder holds
; files Setup did not copy. Settings and history in %LOCALAPPDATA%\SkeinDownloader stay.
Type: filesandordirs; Name: "{app}\video-tools"

[Code]
const
  DownloadList =
    'yt-dlp, the video downloader (yt-dlp.exe, about 22 MB)' + #13#10 +
    '    from https://github.com/yt-dlp/yt-dlp/releases/latest' + #13#10 +
    '    License: Unlicense; the executable contains other components under their own licenses' + #13#10 +
    '' + #13#10 +
    'Deno, the JavaScript runtime yt-dlp needs for some sites' + #13#10 +
    '(deno-x86_64-pc-windows-msvc.zip, about 40 MB; deno.exe is kept)' + #13#10 +
    '    from https://github.com/denoland/deno/releases/latest' + #13#10 +
    '    License: MIT' + #13#10 +
    '' + #13#10 +
    'FFmpeg and FFprobe, which join and convert audio and video' + #13#10 +
    '(ffmpeg-master-latest-win64-gpl.zip, about 190 MB; ffmpeg.exe and ffprobe.exe are kept)' + #13#10 +
    '    from https://github.com/yt-dlp/FFmpeg-Builds/releases/latest' + #13#10 +
    '    License: GPL version 3' + #13#10 +
    '' + #13#10 +
    'Their license texts, from https://raw.githubusercontent.com' + #13#10 +
    '' + #13#10 +
    'Total download: about 255 MB. Each file is checked against the SHA-256 checksum ' +
    'GitHub publishes for it before it is used.';

var
  ProgressPage: TOutputProgressWizardPage;
  Downloaded: Boolean;

procedure InitializeWizard;
begin
  CreateOutputMsgMemoPage(wpWelcome,
    'Internet connection required',
    'Setup downloads the video tools Skein Downloader uses.',
    'These programs are not part of Skein Downloader. Setup downloads their latest ' +
    'releases from GitHub when you click Install, and stops without installing anything ' +
    'if it cannot. Setup downloads:',
    DownloadList);
  ProgressPage := CreateOutputProgressPage('Downloading video tools',
    'Setup is downloading yt-dlp, Deno and FFmpeg from GitHub. This can take a few minutes.');
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then
    Result := Result + MemoTasksInfo + NewLine + NewLine;
  Result := Result + 'Downloads from GitHub (about 255 MB):' + NewLine +
    Space + 'yt-dlp.exe from github.com/yt-dlp/yt-dlp' + NewLine +
    Space + 'deno-x86_64-pc-windows-msvc.zip from github.com/denoland/deno' + NewLine +
    Space + 'ffmpeg-master-latest-win64-gpl.zip from github.com/yt-dlp/FFmpeg-Builds';
end;

// The last line install-video-tools.ps1 has written so far, or ''.
function LastLine(const Log: String): String;
var
  Lines: TArrayOfString;
  I: Integer;
begin
  Result := '';
  if LoadStringsFromFile(Log, Lines) then
    for I := GetArrayLength(Lines) - 1 downto 0 do
      if Trim(Lines[I]) <> '' then begin
        Result := Trim(Lines[I]);
        Exit;
      end;
end;

// Runs install-video-tools.ps1 from {tmp}, so the tools land in {tmp}\video-tools, and shows
// each line it prints (which file it is downloading, and its size) until it reports the
// result. Returns False, after saying why, when the tools could not be installed.
function DownloadVideoTools: Boolean;
var
  Script, Log, Line, Shown: String;
  ResultCode, Step: Integer;
  Ticks: Integer;
begin
  Result := False;
  ExtractTemporaryFile('install-video-tools.ps1');
  Script := ExpandConstant('{tmp}\install-video-tools.ps1');
  Log := ExpandConstant('{tmp}\install-video-tools.log');
  DelTree(ExpandConstant('{tmp}\video-tools'), True, True, True);
  DeleteFile(Log);
  ProgressPage.SetText('Connecting to GitHub...', '');
  ProgressPage.SetProgress(0, 10);
  ProgressPage.Show;
  try
    if not Exec(ExpandConstant('{cmd}'),
        '/c ""' + ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe') +
        '" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + Script +
        '" > "' + Log + '" 2>&1"', '', SW_HIDE, ewNoWait, ResultCode) then begin
      SuppressibleMsgBox('Setup could not start Windows PowerShell to download the video tools.' + #13#10#13#10 +
        SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
      Exit;
    end;
    Ticks := 0;
    Shown := '';
    Step := 0;
    repeat
      Sleep(250);
      Ticks := Ticks + 1;
      Line := LastLine(Log);
      if (Line <> '') and (Line <> Shown) then begin
        Shown := Line;
        if Step < 9 then
          Step := Step + 1;
        ProgressPage.SetText(Line, 'Files are checked against their published SHA-256 checksums.');
      end;
      ProgressPage.SetProgress(Step, 10);
      if Pos('Video tools installed', Line) = 1 then begin
        Result := FileExists(ExpandConstant('{tmp}\video-tools\current.txt'));
        if not Result then
          SuppressibleMsgBox('The video tools were downloaded but are incomplete. Click Install to try again.',
            mbError, MB_OK, IDOK);
        Exit;
      end;
      if Pos('Installation failed', Line) = 1 then begin
        SuppressibleMsgBox('Setup could not download the video tools:' + #13#10#13#10 + Line + #13#10#13#10 +
          'Check the internet connection, then click Install to try again.', mbError, MB_OK, IDOK);
        Exit;
      end;
    until Ticks > 30 * 60 * 4;
    SuppressibleMsgBox('Downloading the video tools took longer than 30 minutes and was stopped. ' +
      'Check the internet connection, then click Install to try again.', mbError, MB_OK, IDOK);
  finally
    ProgressPage.Hide;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpReady then begin
    Downloaded := DownloadVideoTools;
    Result := Downloaded;
  end;
end;

// Silent installs (/SILENT, /VERYSILENT) skip the Ready page: download here instead, before
// any file is copied, and stop the install if that fails.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not Downloaded then begin
    Downloaded := DownloadVideoTools;
    if not Downloaded then
      Result := 'The video tools (yt-dlp, Deno, FFmpeg) could not be downloaded from GitHub.';
  end;
end;
