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
;
; Setup speaks the app's languages: it follows the Windows display language and asks only
; when that is not one of them. Inno Setup's own text comes from its bundled translations
; or languages\ (see the README there); this script's text comes from messages\<code>.isl.

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
ShowLanguageDialog=auto

[Languages]
; In the app's order (src\wx\localization.h). Portuguese also serves Brazilian Portuguese.
Name: "english"; MessagesFile: "compiler:Default.isl,messages\en.isl"
Name: "chinesesimplified"; MessagesFile: "languages\ChineseSimplified.isl,messages\zh_CN.isl"
Name: "chinesetraditional"; MessagesFile: "languages\ChineseTraditional.isl,messages\zh_TW.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl,messages\de.isl"
Name: "vietnamese"; MessagesFile: "languages\Vietnamese.isl,messages\vi.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl,messages\ru.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl,messages\ja.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl,messages\ko.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl,messages\fr.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl,messages\it.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl,messages\es.isl"
Name: "portuguese"; MessagesFile: "compiler:Languages\Portuguese.isl,messages\pt.isl"
Name: "hindi"; MessagesFile: "languages\Hindi.islu,messages\hi.isl"

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

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
Filename: "{app}\skein-downloader.exe"; Description: "{cm:LaunchProgram,Skein Downloader}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; yt-dlp updates itself and the program can reinstall the tools, so this folder holds
; files Setup did not copy. Settings and history in %LOCALAPPDATA%\SkeinDownloader stay.
Type: filesandordirs; Name: "{app}\video-tools"

[Code]
var
  ProgressPage: TOutputProgressWizardPage;
  Downloaded: Boolean;

// The text below comes from messages\<code>.isl for the Setup language. Pascal Script reads
// an empty [] as a set, so FmtMessage is used only for messages with %1 arguments.

// What Setup downloads, for the first page. File names, addresses and license names stay
// as they are in every language.
function DownloadList: String;
var
  NewLine, Indent: String;
begin
  NewLine := #13#10;
  Indent := '    ';
  Result :=
    CustomMessage('ToolYtDlp') + NewLine +
    Indent + FmtMessage(CustomMessage('DownloadFrom'), ['https://github.com/yt-dlp/yt-dlp/releases/latest']) + NewLine +
    Indent + FmtMessage(CustomMessage('DownloadLicense'), [CustomMessage('LicenseYtDlp')]) + NewLine + NewLine +
    CustomMessage('ToolDeno') + NewLine +
    Indent + FmtMessage(CustomMessage('DownloadFrom'), ['https://github.com/denoland/deno/releases/latest']) + NewLine +
    Indent + FmtMessage(CustomMessage('DownloadLicense'), ['MIT']) + NewLine + NewLine +
    CustomMessage('ToolFFmpeg') + NewLine +
    Indent + FmtMessage(CustomMessage('DownloadFrom'), ['https://github.com/yt-dlp/FFmpeg-Builds/releases/latest']) + NewLine +
    Indent + FmtMessage(CustomMessage('DownloadLicense'), [CustomMessage('LicenseGpl3')]) + NewLine + NewLine +
    FmtMessage(CustomMessage('LicenseTexts'), ['https://raw.githubusercontent.com']) + NewLine + NewLine +
    CustomMessage('DownloadTotal');
end;

procedure InitializeWizard;
begin
  // Inno Setup 6.7 leaves too little room left of the task check boxes above 100% DPI,
  // so their left edge is cut off; this margin shows them whole.
  WizardForm.TasksList.Offset := ScaleX(4);
  CreateOutputMsgMemoPage(wpWelcome, CustomMessage('InternetTitle'), CustomMessage('InternetSubtitle'),
    CustomMessage('InternetText'), DownloadList);
  ProgressPage := CreateOutputProgressPage(CustomMessage('ProgressTitle'), CustomMessage('ProgressSubtitle'));
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then
    Result := Result + MemoTasksInfo + NewLine + NewLine;
  Result := Result + CustomMessage('ReadyDownloads') + NewLine +
    Space + FmtMessage(CustomMessage('ReadyFrom'), ['yt-dlp.exe', 'github.com/yt-dlp/yt-dlp']) + NewLine +
    Space + FmtMessage(CustomMessage('ReadyFrom'), ['deno-x86_64-pc-windows-msvc.zip', 'github.com/denoland/deno']) + NewLine +
    Space + FmtMessage(CustomMessage('ReadyFrom'), ['ffmpeg-master-latest-win64-gpl.zip', 'github.com/yt-dlp/FFmpeg-Builds']);
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
  ProgressPage.SetText(CustomMessage('Connecting'), '');
  ProgressPage.SetProgress(0, 10);
  ProgressPage.Show;
  try
    if not Exec(ExpandConstant('{cmd}'),
        '/c ""' + ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe') +
        '" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + Script +
        '" > "' + Log + '" 2>&1"', '', SW_HIDE, ewNoWait, ResultCode) then begin
      SuppressibleMsgBox(CustomMessage('PowerShellFailed') + #13#10#13#10 +
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
        // The script's own lines (which file, how large) are shown as it writes them, in English.
        ProgressPage.SetText(Line, CustomMessage('ChecksumNote'));
      end;
      ProgressPage.SetProgress(Step, 10);
      if Pos('Video tools installed', Line) = 1 then begin
        Result := FileExists(ExpandConstant('{tmp}\video-tools\current.txt'));
        if not Result then
          SuppressibleMsgBox(CustomMessage('DownloadIncomplete'), mbError, MB_OK, IDOK);
        Exit;
      end;
      if Pos('Installation failed', Line) = 1 then begin
        SuppressibleMsgBox(FmtMessage(CustomMessage('DownloadFailed'), [Line]), mbError, MB_OK, IDOK);
        Exit;
      end;
    until Ticks > 30 * 60 * 4;
    SuppressibleMsgBox(CustomMessage('DownloadTimeout'), mbError, MB_OK, IDOK);
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
      Result := CustomMessage('DownloadNotPossible');
  end;
end;
