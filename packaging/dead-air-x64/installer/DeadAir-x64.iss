#ifndef RepoRoot
  #error RepoRoot must point to the DeadAir-x64 repository.
#endif
#ifndef PortVersion
  #define PortVersion "1.4.0"
#endif
#ifndef OutputDirectory
  #define OutputDirectory AddBackslash(RepoRoot) + "artifacts\installer"
#endif
#ifndef LauncherPath
  #define LauncherPath AddBackslash(RepoRoot) + "build\installer\Uninstall Dead Air x64.exe"
#endif
#ifndef CompatibilityArchive
  #define CompatibilityArchive AddBackslash(RepoRoot) + "build\installer\xtra_dead_air_x64.xdb0"
#endif
#ifndef UpdaterPath
  #define UpdaterPath AddBackslash(RepoRoot) + "build\installer\DeadAirUpdater.exe"
#endif
#ifndef MaintenancePath
  #define MaintenancePath AddBackslash(RepoRoot) + "build\installer\maintenance\Dead-Air-Refined-Maintenance.exe"
#endif
#ifndef ApplicationId
  #define ApplicationId "{{9732DFF1-E40D-4B23-B215-6D28B1DD0DE0}"
#endif

; Content is never optional, so there is deliberately no default here: a build that forgot to
; pass these must fail loudly rather than quietly produce a contentless Setup.
#ifndef MaintenanceOnly
  #ifndef ContentBytes
    #error ContentBytes must be the total packed size of the content bundles.
  #endif
  #ifndef ContentManifestPath
    #error ContentManifestPath must point to the built content-manifest.txt.
  #endif
  #ifndef ContentFetcherPath
    #error ContentFetcherPath must point to the built DeadAirContent.exe.
  #endif
  ; The liveness probe opens the mutex the fetcher holds. Its name is read out of
  ; src\xrContentSync\ContentPaths.h by the build script and passed in here rather than
  ; retyped: the Setup first published for 1.4.0 carried a hand-copied spelling that did not
  ; match, and every
  ; download longer than the start grace was reported as interrupted.
  #ifndef ContentMutexName
    #error ContentMutexName must be the fetcher liveness mutex name from ContentPaths.h.
  #endif
#endif

#define ProductName "Dead Air: Refined"
#define ProductVersion PortVersion
#define RuntimeRoot AddBackslash(RepoRoot) + "bin\x64\Release"
#define InstallerRoot AddBackslash(RepoRoot) + "packaging\dead-air-x64\installer"

[Setup]
AppId={#ApplicationId}
AppName={#ProductName}
AppVerName={#ProductName} {#PortVersion}
AppVersion={#ProductVersion}
AppPublisher=Dead Air: Refined
VersionInfoVersion={#PortVersion}.0
VersionInfoDescription={#ProductName} installer
DefaultDirName={code:GetDefaultDirName}
DefaultGroupName=Dead Air Refined
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupArchitecture=x64
WizardStyle=modern
WizardSizePercent=110
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
; The running game creates this mutex (ContentPaths::GameMutexName, created in
; src/xr_3da/entry_point.cpp), so install and uninstall
; refuse while it has files open. Session-local rather than Global\: creating a global mutex
; needs a privilege a standard user does not reliably hold, and setup stays in the same session
; even when it elevates.
; Covers both install and uninstall - Inno records the name in the uninstall data.
AppMutex=Local\DeadAirRefined.Game
UsePreviousAppDir=no
UsePreviousGroup=no
UsePreviousTasks=no
Uninstallable=yes
UninstallFilesDir={app}\.dead-air-x64
UninstallLogMode=overwrite
UninstallDisplayName={#ProductName} {#PortVersion}
UninstallDisplayIcon={app}\xrEngine.exe
OutputDir={#OutputDirectory}
#ifdef MaintenanceOnly
OutputBaseFilename=Dead-Air-Refined-Maintenance
#else
OutputBaseFilename=Dead-Air-Refined-{#PortVersion}-Setup
#endif
Compression=none
SolidCompression=no
DiskSpanning=no
CreateAppDir=yes
DirExistsWarning=no
#ifndef MaintenanceOnly
; A display value and a second net only - Inno evaluates it at the transition into the install,
; which is after the content page has already spent the bandwidth. The gate that actually runs
; before the download is the free-space check at wpSelectDir.
ExtraDiskSpaceRequired={#ContentBytes}
#endif

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "Создать ярлык на рабочем столе"; GroupDescription: "Дополнительные ярлыки:"; Flags: unchecked

[Files]
#ifndef MaintenanceOnly
Source: "{#RuntimeRoot}\*.exe"; DestDir: "{app}"; Flags: ignoreversion uninsneveruninstall
Source: "{#RuntimeRoot}\*.dll"; DestDir: "{app}"; Flags: ignoreversion uninsneveruninstall
Source: "{#CompatibilityArchive}"; DestDir: "{app}\database"; Flags: ignoreversion
Source: "{#LauncherPath}"; DestDir: "{app}"; DestName: "Uninstall Dead Air Refined.exe"; Flags: ignoreversion
Source: "{#UpdaterPath}"; DestDir: "{app}"; DestName: "DeadAirUpdater.exe"; Flags: ignoreversion
Source: "{#MaintenancePath}"; DestDir: "{app}\.dead-air-x64"; DestName: "Dead-Air-Refined-Maintenance.exe"; Flags: ignoreversion
; dontcopy extracts under the SOURCE basename, so these two files must already be named
; DeadAirContent.exe and content-manifest.txt on disk.
Source: "{#ContentFetcherPath}"; Flags: dontcopy
Source: "{#ContentManifestPath}"; Flags: dontcopy
; The trust root. It arrives inside the hash-verified Setup payload and is what the commit and
; every later launch measure the installation against.
Source: "{#ContentManifestPath}"; DestDir: "{app}\.dead-air-x64"; DestName: "content-manifest.txt"; Flags: ignoreversion
#endif
Source: "{#InstallerRoot}\runtime-files.txt"; Flags: dontcopy
Source: "{#InstallerRoot}\runtime-files.txt"; DestDir: "{app}\.dead-air-x64"; Flags: ignoreversion

[Dirs]
Name: "{app}\appdata\savedgames"; Flags: uninsneveruninstall

[Icons]
Name: "{group}\Dead Air Refined"; Filename: "{app}\xrEngine.exe"; WorkingDir: "{app}"
Name: "{group}\Удалить Dead Air Refined"; Filename: "{app}\Uninstall Dead Air Refined.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Dead Air Refined"; Filename: "{app}\xrEngine.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
#ifndef MaintenanceOnly
Filename: "{app}\xrEngine.exe"; Description: "Запустить Dead Air: Refined"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent
#endif

[InstallDelete]
Type: files; Name: "{app}\Uninstall Dead Air x64.exe"
Type: files; Name: "{app}\.dead-air-x64\maintenance-small.bmp"

[UninstallDelete]
Type: files; Name: "{app}\.dead-air-x64\maintenance-small.png"
; Deliberately no database\xtra_dead_air_x64_content_*.xdb0 entry: Inno's wildcard deleter has no
; reparse-point check and would follow a junctioned database\ into another installation.
; RemoveContentBundles is the sole delete authority for that directory.
Type: filesandordirs; Name: "{app}\.dead-air-x64\content-cache"
Type: files; Name: "{app}\appdata\vfs-index-*.cache"
Type: dirifempty; Name: "{app}\.dead-air-x64"
Type: dirifempty; Name: "{app}\database"
Type: dirifempty; Name: "{app}"

[Code]
const
  Scs32BitBinary = 0;
  Scs64BitBinary = 6;
  UninstallActionCancel = 0;
  UninstallActionRemove = 1;
  // The access mask OpenMutexW needs to probe whether the content fetcher is still alive.
  SynchronizeAccess = $00100000;
  FileAttributeReparsePoint = $00000400;
  InvalidFileAttributes = $FFFFFFFF;
  MoveFileDelayUntilReboot = $00000004;
  MaintenanceFooterHeight = 74;
  SmCxFrame = 32;
  SmCyCaption = 4;
  SmCxPaddedBorder = 92;
#ifndef MaintenanceOnly
  ContentMutexName = '{#ContentMutexName}';
  ContentPollIntervalMs = 200;
  // The fetcher creates its mutex a moment after it starts. Without a grace window the first
  // poll would report a crash that has not happened.
  ContentStartGraceMs = 10000;
  // Generous on purpose: the fetcher's own retry ladder plus its idle timeout can legitimately
  // stay quiet for well over two minutes, and killing a healthy slow download is worse than
  // waiting out a dead one.
  ContentHeartbeatTimeoutMs = 240000;
  // How long a fetcher left over from an earlier attempt gets to stop before a new one is
  // refused. Its workers check the cancel flag between one-megabyte reads.
  ContentStaleStopMs = 60000;
  ContentProgressSteps = 1000;
#endif

var
  RuntimeFiles: TArrayOfString;
  ManagedFiles: TArrayOfString;
  RestoreOriginalX86OnUninstall: Boolean;
#ifndef MaintenanceOnly
  ContentProgressPage: TOutputProgressWizardPage;
  ContentFetchRunning: Boolean;
  ContentFetchCancelled: Boolean;
  ContentLastHeartbeat: TFileTime;
  ContentLastHeartbeatTick: Cardinal;
#endif
  MaintenanceForm: TSetupForm;
  MaintenanceContentPanel: TPanel;
  MaintenanceFooterPanel: TPanel;
  MaintenanceOkButton: TNewButton;
  MaintenanceCancelButton: TNewButton;
  MaintenanceButtonWidth: Integer;

function OpenMutexW(DesiredAccess: Cardinal; InheritHandle: Boolean; Name: String): THandle;
  external 'OpenMutexW@kernel32.dll stdcall';

// Closes the mutex probe handle from the content-fetch poll.
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function GetDiskFreeSpaceEx(DirectoryName: String;
  var FreeBytesAvailable, TotalNumberOfBytes, TotalNumberOfFreeBytes: Int64): Boolean;
  external 'GetDiskFreeSpaceExW@kernel32.dll stdcall';

function GetFileAttributesW(FileName: String): Cardinal;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function GetTickCount: Cardinal;
  external 'GetTickCount@kernel32.dll stdcall';

// A second name over the same export, with the destination typed as Cardinal: a delayed delete
// needs a NULL destination and Pascal Script cannot pass nil for a String parameter.
function MoveFileExDeleteW(ExistingFileName: String; NewFileName: Cardinal; Flags: Cardinal): Boolean;
  external 'MoveFileExW@kernel32.dll stdcall';

function GetBinaryType(ApplicationName: String; var BinaryType: Cardinal): Boolean;
  external 'GetBinaryTypeW@kernel32.dll stdcall';

function GetDpiForWindow(Window: HWND): Cardinal;
  external 'GetDpiForWindow@user32.dll stdcall';

function GetSystemMetricsForDpi(Index: Integer; Dpi: Cardinal): Integer;
  external 'GetSystemMetricsForDpi@user32.dll stdcall';

function TargetParameter: String;
begin
  Result := RemoveBackslashUnlessRoot(Trim(ExpandConstant('{param:TARGET|}')));
end;

function CommandLineParameter(Name: String; DefaultValue: String): String;
begin
  Result := ExpandConstant('{param:' + Name + '|' + DefaultValue + '}');
end;

// Moved up from the uninstall section: Pascal Script is single-pass and the content fetch
// launcher below needs it.
function QuoteParameter(Value: String): String;
begin
  Result := '"' + Value + '"';
end;

#ifndef MaintenanceOnly
function ContentCacheDirectory(GameDirectory: String): String;
begin
  Result := AddBackslash(GameDirectory) + '.dead-air-x64\content-cache';
end;

// The only disk check that runs BEFORE the download. ExtraDiskSpaceRequired fires at the
// transition into the install, by which point the bandwidth has already been spent.
function ContentFreeSpaceOk(GameDirectory: String; var FreeBytes: Int64): Boolean;
var
  Total: Int64;
  TotalFree: Int64;
  Root: String;
begin
  FreeBytes := 0;
  Root := AddBackslash(GameDirectory);
  if not GetDiskFreeSpaceEx(Root, FreeBytes, Total, TotalFree) then
  begin
    // The volume would not answer. That is "unknown", not "full", and refusing the install on
    // it would be worse than letting the fetcher's own check decide.
    Result := True;
    exit;
  end;
  Result := FreeBytes >= {#ContentBytes};
end;

// The fetcher rewrites this file through a temporary, so a torn read is not expected - but a
// failed parse still has to leave the caller's previous values alone rather than jump the bar
// back to zero.
function ContentReadProgress(GameDirectory: String; var Done, Total: Int64): Boolean;
var
  Lines: TArrayOfString;
  Separator: Integer;
  Line: String;
begin
  Result := False;
  if not LoadStringsFromFile(AddBackslash(ContentCacheDirectory(GameDirectory)) +
    'content-fetch-progress.txt', Lines) then
    exit;
  if GetArrayLength(Lines) = 0 then
    exit;

  Line := Trim(Lines[0]);
  Separator := Pos(#9, Line);
  if Separator <= 1 then
    exit;

  Done := StrToInt64Def(Copy(Line, 1, Separator - 1), -1);
  Total := StrToInt64Def(Copy(Line, Separator + 1, Length(Line) - Separator), -1);
  Result := (Done >= 0) and (Total > 0);
end;

// Alive means the fetcher still holds its mutex. A progress file alone cannot tell a working
// download from a process that died without writing a result.
function ContentFetcherAlive: Boolean;
var
  Handle: THandle;
begin
  Handle := OpenMutexW(SynchronizeAccess, False, ContentMutexName);
  Result := Handle <> 0;
  if Result then
    CloseHandle(Handle);
end;

function ContentHeartbeatStalled(GameDirectory: String): Boolean;
var
  Records: TFindRec;
  Changed: Boolean;
begin
  Result := False;
  Changed := True;
  if FindFirst(AddBackslash(ContentCacheDirectory(GameDirectory)) +
    'content-fetch-progress.txt', Records) then
  begin
    Changed := (Records.LastWriteTime.dwLowDateTime <> ContentLastHeartbeat.dwLowDateTime) or
      (Records.LastWriteTime.dwHighDateTime <> ContentLastHeartbeat.dwHighDateTime);
    if Changed then
      ContentLastHeartbeat := Records.LastWriteTime;
    FindClose(Records);
  end;

  if Changed then
    ContentLastHeartbeatTick := GetTickCount
  else
    Result := (GetTickCount - ContentLastHeartbeatTick) > ContentHeartbeatTimeoutMs;
end;

// Stops a fetcher left over from an earlier attempt and waits for it to let go of the cache.
// A wizard closed at an error page never told its fetcher to stop, and two fetchers on one
// cache collide on the part files. True when no fetcher holds the mutex any more.
function StopStaleContentFetcher(CacheDirectory: String): Boolean;
var
  Started: Cardinal;
begin
  Result := not ContentFetcherAlive;
  if Result then
    exit;

  Log('A content fetcher from an earlier attempt is still running; asking it to stop.');
  SaveStringToFile(AddBackslash(CacheDirectory) + 'content-fetch-cancel.txt', 'cancel', False);
  Started := GetTickCount;
  while (GetTickCount - Started) < ContentStaleStopMs do
  begin
    Sleep(ContentPollIntervalMs);
    if not ContentFetcherAlive then
    begin
      Log('The earlier content fetcher has stopped.');
      Result := True;
      exit;
    end;
  end;
  Log('The earlier content fetcher did not stop within the wait.');
end;

function ContentResultValue(Lines: TArrayOfString; Key: String): String;
var
  Index: Integer;
  Line: String;
begin
  Result := '';
  for Index := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[Index]);
    if Pos(Key, Line) = 1 then
    begin
      Result := Copy(Line, Length(Key) + 1, Length(Line) - Length(Key));
      exit;
    end;
  end;
end;

// Drives one fetch to completion. Returns '' on success and a message otherwise.
//
// Interactive mode drives the standard output progress page, whose SetProgress pumps the
// message queue - without that the wizard would be frozen for the whole download and the
// Cancel button could not even be clicked. Silent mode simply blocks: there is no page to
// update and nobody to answer.
function RunContentFetch(GameDirectory: String; Interactive: Boolean): String;
var
  CacheDirectory: String;
  ResultFile: String;
  ResultLines: TArrayOfString;
  ExitText: String;
  MessageText: String;
  ResultCode: Integer;
  Done: Int64;
  Total: Int64;
  Started: Cardinal;
  Position: Integer;
begin
  Result := '';
  CacheDirectory := ContentCacheDirectory(GameDirectory);
  if not ForceDirectories(CacheDirectory) then
  begin
    Result := 'Не удалось создать папку для загрузки контента.';
    exit;
  end;

  SaveStringToFile(AddBackslash(CacheDirectory) + 'README.txt',
    'Здесь хранится загруженный контент Dead Air: Refined.' + Chr(13) + Chr(10) +
    'Папку можно удалить, когда игра не запущена - файлы будут загружены заново.' +
    Chr(13) + Chr(10), False);

  if not StopStaleContentFetcher(CacheDirectory) then
  begin
    Result := 'Предыдущая загрузка контента ещё не остановилась. Подождите минуту и повторите попытку.';
    exit;
  end;

  ResultFile := AddBackslash(CacheDirectory) + 'content-fetch-result.txt';
  DeleteFile(ResultFile);
  DeleteFile(AddBackslash(CacheDirectory) + 'content-fetch-progress.txt');
  DeleteFile(AddBackslash(CacheDirectory) + 'content-fetch-cancel.txt');

  // Both come out of the hash-verified Setup payload. The manifest is staged at a fixed path
  // the fetcher knows: it is never named on a command line, so nothing external can nominate
  // what "complete" means for this installation.
  try
    ExtractTemporaryFile('DeadAirContent.exe');
    ExtractTemporaryFile('content-manifest.txt');
  except
    Result := 'Не удалось распаковать средство загрузки контента.';
    exit;
  end;

  if not FileCopy(ExpandConstant('{tmp}\content-manifest.txt'),
    AddBackslash(CacheDirectory) + 'pending-manifest.txt', False) then
  begin
    Result := 'Не удалось подготовить список контента.';
    exit;
  end;

  ContentFetchCancelled := False;
  ContentLastHeartbeatTick := GetTickCount;
  ContentLastHeartbeat.dwLowDateTime := 0;
  ContentLastHeartbeat.dwHighDateTime := 0;

  if not Exec(ExpandConstant('{tmp}\DeadAirContent.exe'),
    '--content-fetch --game-dir ' + QuoteParameter(GameDirectory) +
    ' --cancel-flag ' + QuoteParameter(AddBackslash(CacheDirectory) + 'content-fetch-cancel.txt'),
    '', SW_HIDE, ewNoWait, ResultCode) then
  begin
    Result := 'Не удалось запустить загрузку контента.';
    exit;
  end;

  ContentFetchRunning := True;
  if Interactive then
  begin
    ContentProgressPage.SetProgress(0, ContentProgressSteps);
    ContentProgressPage.Show;
  end;

  try
    Started := GetTickCount;
    while True do
    begin
      Sleep(ContentPollIntervalMs);

      if FileExists(ResultFile) then
      begin
        if not LoadStringsFromFile(ResultFile, ResultLines) then
        begin
          Result := 'Не удалось прочитать результат загрузки контента.';
          exit;
        end;
        ExitText := ContentResultValue(ResultLines, 'exit=');
        MessageText := ContentResultValue(ResultLines, 'message=');
        if ExitText <> '0' then
          Result := 'Не удалось загрузить контент: ' + MessageText;
        exit;
      end;

      if ContentFetchCancelled then
      begin
        // The flag file already told the fetcher to stop. Whatever landed stays in the cache
        // and the next attempt resumes from it.
        Result := 'Загрузка контента отменена.';
        exit;
      end;

      // The grace window matters: the fetcher creates its mutex a moment after Exec returns,
      // so an eager first poll would report a crash that has not happened.
      if (GetTickCount - Started) > ContentStartGraceMs then
      begin
        if not ContentFetcherAlive then
        begin
          Result := 'Загрузка контента прервалась.';
          exit;
        end;
        if ContentHeartbeatStalled(GameDirectory) then
        begin
          // Still holding its mutex, so it is stuck rather than gone: tell it to stop, or the
          // next attempt finds its handles on the part files.
          SaveStringToFile(AddBackslash(CacheDirectory) + 'content-fetch-cancel.txt', 'cancel', False);
          Result := 'Загрузка контента не отвечает.';
          exit;
        end;
      end;

      if Interactive then
      begin
        Position := 0;
        if ContentReadProgress(GameDirectory, Done, Total) and (Total > 0) then
        begin
          Position := Integer((Done * ContentProgressSteps) / Total);
          if Position > ContentProgressSteps then
            Position := ContentProgressSteps;
          ContentProgressPage.SetText('Загружено ' + IntToStr(Done / 1048576) + ' из ' +
            IntToStr(Total / 1048576) + ' МБ', '');
        end;
        // Pumps the message queue as a side effect, which is what keeps Cancel clickable.
        ContentProgressPage.SetProgress(Position, ContentProgressSteps);
      end;
    end;
  finally
    ContentFetchRunning := False;
    if Interactive then
      ContentProgressPage.Hide;
  end;
end;
#endif

function UninstallActionParameter: String;
begin
  Result := Lowercase(Trim(CommandLineParameter('ACTION', 'ask')));
end;

function ExistingX64Directory: String;
begin
  Result := '';
  if not RegQueryStringValue(
    HKCU64,
    'Software\Microsoft\Windows\CurrentVersion\Uninstall\{9732DFF1-E40D-4B23-B215-6D28B1DD0DE0}_is1',
    'InstallLocation',
    Result) then
  begin
    RegQueryStringValue(
      HKLM64,
      'Software\Microsoft\Windows\CurrentVersion\Uninstall\{9732DFF1-E40D-4B23-B215-6D28B1DD0DE0}_is1',
      'InstallLocation',
      Result);
  end;

  Result := RemoveBackslashUnlessRoot(Result);
  if (Result <> '') and
     not FileExists(AddBackslash(Result) + '.dead-air-x64\install-mode.txt') then
  begin
    Result := '';
  end;
end;

function ExistingDeadAirDirectory: String;
begin
  Result := '';
  if not RegQueryStringValue(
    HKLM32,
    'Software\Microsoft\Windows\CurrentVersion\Uninstall\Dead Air_is1',
    'InstallLocation',
    Result) then
  begin
    RegQueryStringValue(
      HKCU32,
      'Software\Microsoft\Windows\CurrentVersion\Uninstall\Dead Air_is1',
      'InstallLocation',
      Result);
  end;

  Result := RemoveBackslashUnlessRoot(Result);
end;

function GetDefaultDirName(Param: String): String;
var
  X64Directory: String;
  ExistingDirectory: String;
begin
  X64Directory := ExistingX64Directory;
  if X64Directory <> '' then
  begin
    Result := X64Directory;
    exit;
  end;

  ExistingDirectory := ExistingDeadAirDirectory;
  if ExistingDirectory <> '' then
    Result := ExistingDirectory
  else
    Result := ExpandConstant('{sd}\Games\Dead Air');
end;

function ValidateUpgradeDirectory(DirectoryName: String): String;
var
  BinaryType: Cardinal;
begin
  Result := '';
  if not FileExists(AddBackslash(DirectoryName) + 'xrEngine.exe') or
     not FileExists(AddBackslash(DirectoryName) + 'fsgame.ltx') or
     not FileExists(AddBackslash(DirectoryName) + 'database\configs.xdb0') then
  begin
    Result :=
      'В выбранной папке не найдена установленная Dead Air 0.98b или Dead Air Revolution II.' + #13#10 +
      'Укажите корневую папку с xrEngine.exe, fsgame.ltx и database\configs.xdb0.';
    exit;
  end;

  if not GetBinaryType(AddBackslash(DirectoryName) + 'xrEngine.exe', BinaryType) or
     ((BinaryType <> Scs32BitBinary) and (BinaryType <> Scs64BitBinary)) then
  begin
    Result :=
      'Выбранная папка не содержит поддерживаемую версию Dead Air.';
  end;
end;

function ValidateSelectedDirectory: String;
begin
  Result := ValidateUpgradeDirectory(WizardDirValue);
end;

procedure InitializeWizard;
begin
#ifndef MaintenanceOnly
  // A standard output progress page rather than a custom one: SetProgress pumps the message
  // queue, which is the only way the wizard stays responsive - and the only way the Cancel
  // button can be clicked at all - while a script-driven download runs for tens of minutes.
  ContentProgressPage := CreateOutputProgressPage('Загрузка контента',
    'Установщик загружает игровой контент. Загрузку можно прервать и продолжить позже.');
#endif

  if TargetParameter <> '' then
    WizardForm.DirEdit.Text := TargetParameter;
end;

function NextButtonClick(CurrentPageId: Integer): Boolean;
var
  ValidationError: String;
#ifndef MaintenanceOnly
  FreeBytes: Int64;
#endif
begin
  Result := True;

  if CurrentPageId = wpSelectDir then
  begin
    ValidationError := ValidateSelectedDirectory;
    if ValidationError <> '' then
    begin
      SuppressibleMsgBox(ValidationError, mbError, MB_OK, IDOK);
      Result := False;
    end;

#ifndef MaintenanceOnly
    // Before the download, not after it.
    if Result and not ContentFreeSpaceOk(WizardDirValue, FreeBytes) then
    begin
      SuppressibleMsgBox('На выбранном диске недостаточно места. Нужно около ' +
        IntToStr({#ContentBytes} / 1073741824 + 1) + ' ГБ, доступно ' +
        IntToStr(FreeBytes / 1073741824) + ' ГБ.', mbError, MB_OK, IDOK);
      Result := False;
    end;
#endif
  end;

end;

function LoadRuntimeFiles(FileName: String): Boolean;
begin
  Result := LoadStringsFromFile(FileName, RuntimeFiles);
end;

procedure AppendString(var Values: TArrayOfString; Value: String);
var
  NewIndex: Integer;
begin
  NewIndex := GetArrayLength(Values);
  SetArrayLength(Values, NewIndex + 1);
  Values[NewIndex] := Value;
end;

function StringArrayContains(Values: TArrayOfString; Value: String): Boolean;
var
  Index: Integer;
begin
  Result := False;
  for Index := 0 to GetArrayLength(Values) - 1 do
  begin
    if CompareText(Values[Index], Value) = 0 then
    begin
      Result := True;
      exit;
    end;
  end;
end;

procedure AppendUniqueString(var Values: TArrayOfString; Value: String);
begin
  if not StringArrayContains(Values, Value) then
    AppendString(Values, Value);
end;

function LoadManagedFilesFromControl(DirectoryName: String; var Values: TArrayOfString): Boolean;
var
  ControlDirectory: String;
begin
  SetArrayLength(Values, 0);
  ControlDirectory := AddBackslash(DirectoryName) + '.dead-air-x64';
  Result := LoadStringsFromFile(AddBackslash(ControlDirectory) + 'managed-files.txt', Values);
  if not Result then
  begin
    Result := LoadStringsFromFile(AddBackslash(ControlDirectory) + 'runtime-files.txt', Values);
    if Result then
      AppendUniqueString(Values, 'database\xtra_dead_air_x64.xdb0');
  end;
end;

// The managed list decides what gets backed up, what gets deleted when it drops out of a new
// version, what the updater snapshots on every update, and what the uninstaller removes.
//
// Content bundles and the content cache are NEVER in it. Bundle names carry a content hash, so
// they change whenever the bytes do - a bundle in this list would be deleted from database\ the
// moment a version renamed it, outside the content commit, destroying the very file the commit
// wanted to keep in the cache. It would also be copied into every backup, at gigabytes a time.
// The uninstall path is the one consumer where a bundle here would appear to work, which is
// exactly why the mistake would survive review.
//
// content-manifest.txt is different and does belong: two kilobytes, and the record of what the
// installation is supposed to contain.
procedure BuildManagedFiles;
var
  Index: Integer;
begin
  SetArrayLength(ManagedFiles, 0);
  for Index := 0 to GetArrayLength(RuntimeFiles) - 1 do
    AppendUniqueString(ManagedFiles, Trim(RuntimeFiles[Index]));
  AppendUniqueString(ManagedFiles, 'database\xtra_dead_air_x64.xdb0');
  AppendUniqueString(ManagedFiles, 'Uninstall Dead Air Refined.exe');
  AppendUniqueString(ManagedFiles, 'DeadAirUpdater.exe');
  AppendUniqueString(ManagedFiles, '.dead-air-x64\Dead-Air-Refined-Maintenance.exe');
  AppendUniqueString(ManagedFiles, '.dead-air-x64\runtime-files.txt');
  AppendUniqueString(ManagedFiles, '.dead-air-x64\content-manifest.txt');
  AppendUniqueString(ManagedFiles, '.dead-air-x64\unins000.exe');
  AppendUniqueString(ManagedFiles, '.dead-air-x64\unins000.dat');
  AppendUniqueString(ManagedFiles, '.dead-air-x64\unins000.msg');
  AppendUniqueString(ManagedFiles, '.dead-air-x64\maintenance-small.png');
end;

// Files the previous version installed that the incoming one no longer ships (for example
// xrRender_GL.dll after the OpenGL renderer was dropped). Without this they would stay in the
// game folder forever: the new managed list does not mention them, so neither the upgrade nor a
// later uninstall would ever touch them again. A file that replaced an original x86 one is
// restored from the backup instead of being deleted.
procedure RemoveObsoleteManagedFiles(DirectoryName: String);
var
  PreviousManagedFiles: TArrayOfString;
  OriginalFiles: TArrayOfString;
  Index: Integer;
  FileName: String;
  TargetPath: String;
  BackupPath: String;
begin
  SetArrayLength(PreviousManagedFiles, 0);
  if not LoadManagedFilesFromControl(DirectoryName, PreviousManagedFiles) then
    exit;

  SetArrayLength(OriginalFiles, 0);
  LoadStringsFromFile(
    AddBackslash(DirectoryName) + '.dead-air-x64\original-files.txt',
    OriginalFiles);

  for Index := 0 to GetArrayLength(PreviousManagedFiles) - 1 do
  begin
    FileName := Trim(PreviousManagedFiles[Index]);
    if (FileName = '') or StringArrayContains(ManagedFiles, FileName) then
      continue;

    TargetPath := AddBackslash(DirectoryName) + FileName;
    BackupPath := AddBackslash(DirectoryName) + '.dead-air-x64\backup-x86\' + FileName;
    if StringArrayContains(OriginalFiles, FileName) and FileExists(BackupPath) then
      CopyFile(BackupPath, TargetPath, False)
    else if FileExists(TargetPath) then
      DeleteFile(TargetPath);
  end;
end;

function CurrentVersionName(DirectoryName: String): String;
var
  StoredVersion: AnsiString;
  BinaryType: Cardinal;
begin
  if LoadStringFromFile(
    AddBackslash(DirectoryName) + '.dead-air-x64\port-version.txt',
    StoredVersion) then
  begin
    Result := Trim(String(StoredVersion));
    exit;
  end;

  if GetBinaryType(AddBackslash(DirectoryName) + 'xrEngine.exe', BinaryType) and
     (BinaryType = Scs32BitBinary) then
    Result := 'original-x86'
  else
    Result := 'unknown-x64';
end;

function PrepareOriginalX86Backup(DirectoryName: String): String;
var
  ControlDirectory: String;
  BackupDirectory: String;
  OriginalFiles: TArrayOfString;
  Index: Integer;
  FileName: String;
  SourcePath: String;
  DestinationPath: String;
begin
  Result := '';
  ControlDirectory := AddBackslash(DirectoryName) + '.dead-air-x64';
  BackupDirectory := AddBackslash(ControlDirectory) + 'backup-x86';

  if DirExists(BackupDirectory) and
     FileExists(AddBackslash(ControlDirectory) + 'original-files.txt') then
  begin
    exit;
  end;

  if not ForceDirectories(BackupDirectory) then
  begin
    Result := 'Не удалось создать резервную копию исходной версии игры.';
    exit;
  end;

  SetArrayLength(OriginalFiles, 0);
  for Index := 0 to GetArrayLength(ManagedFiles) - 1 do
  begin
    FileName := Trim(ManagedFiles[Index]);
    SourcePath := AddBackslash(DirectoryName) + FileName;
    if FileExists(SourcePath) then
    begin
      DestinationPath := AddBackslash(BackupDirectory) + FileName;
      if not ForceDirectories(ExtractFileDir(DestinationPath)) or
         not CopyFile(SourcePath, DestinationPath, False) then
      begin
        Result := 'Не удалось сохранить исходный файл: ' + FileName;
        exit;
      end;
      AppendString(OriginalFiles, FileName);
    end;
  end;

  if not SaveStringsToFile(
    AddBackslash(ControlDirectory) + 'original-files.txt',
    OriginalFiles,
    False) then
  begin
    Result := 'Не удалось записать состав исходной версии игры.';
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ValidationError: String;
  ControlDirectory: String;
begin
  Result := '';
  if TargetParameter <> '' then
    WizardForm.DirEdit.Text := TargetParameter;

  ValidationError := ValidateSelectedDirectory;
  if ValidationError <> '' then
  begin
    Result := ValidationError;
    exit;
  end;

  ExtractTemporaryFile('runtime-files.txt');
  if not LoadRuntimeFiles(ExpandConstant('{tmp}\runtime-files.txt')) then
  begin
    Result := 'Не удалось прочитать список устанавливаемых файлов.';
    exit;
  end;
  BuildManagedFiles;

  ControlDirectory := AddBackslash(WizardDirValue) + '.dead-air-x64';
  if not ForceDirectories(ControlDirectory) then
  begin
    Result := 'Не удалось создать служебную папку .dead-air-x64.';
    exit;
  end;

#ifndef MaintenanceOnly
  // The fetch is the first thing that happens and the last thing that can fail harmlessly:
  // nothing below has run yet, so returning a message here aborts the install with the
  // installation untouched. It also means a silent Setup fetches exactly like an interactive
  // one - there is no wizard page to skip, so content cannot become optional by accident.
  Result := RunContentFetch(WizardDirValue, not WizardSilent);
  if Result <> '' then
    exit;

  // The point of no return, and the first line of it. Everything below mutates the
  // installation; from here on a crash must not be able to present as a healthy install. The
  // latch is deliberately NOT written on the wizard page: a player who lets the download
  // finish and then cancels at the ready page still has the installation they started with.
  SaveStringToFile(
    AddBackslash(ControlDirectory) + 'content-incomplete.txt',
    'schema=dead-air-refined.content-incomplete/1'#10 +
    'version={#ProductVersion}'#10 +
    'reason=install-commit'#10 +
    'time=0'#10,
    False);
#endif

  // The original x86 game is backed up unconditionally: it is the only restore target that
  // still exists, and the updater passes /BACKUP=no on every update, so an opt-in would mean
  // it never happened. CurrentVersionName reads port-version.txt first, so after the first
  // Refined install this never fires again.
  if CurrentVersionName(WizardDirValue) = 'original-x86' then
  begin
    Result := PrepareOriginalX86Backup(WizardDirValue);
    if Result <> '' then
      exit;
  end;

  if not SaveStringToFile(
    AddBackslash(ControlDirectory) + 'install-mode.txt',
    'upgrade',
    False) then
  begin
    Result := 'Не удалось записать режим установки.';
    exit;
  end;

  // Runs before the new list is written: it needs the previous composition to tell what was
  // dropped. The upgrade backup above already captured the union of both lists, so a rollback
  // still restores whatever is removed here.
  RemoveObsoleteManagedFiles(WizardDirValue);

  if not SaveStringsToFile(
    AddBackslash(ControlDirectory) + 'managed-files.txt',
    ManagedFiles,
    False) then
  begin
    Result := 'Не удалось записать состав установленной программы.';
    exit;
  end;

  if not SaveStringToFile(
    AddBackslash(ControlDirectory) + 'port-version.txt',
    '{#ProductVersion}',
    False) then
  begin
    Result := 'Не удалось записать версию программы.';
  end;
end;

procedure RemoveControlMetadata(DirectoryName: String);
var
  ControlDirectory: String;
  CacheDirectory: String;
  Records: TFindRec;
begin
  ControlDirectory := AddBackslash(DirectoryName) + '.dead-air-x64';
  DeleteFile(AddBackslash(ControlDirectory) + 'install-mode.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'original-files.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'port-version.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'managed-files.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'content-manifest.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'content-state.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'content-incomplete.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'rejected-deltas.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'patch-rejected.txt');

  // In code rather than only in [UninstallDelete]: those entries are baked into unins000.dat at
  // install time, so they never run for an installation made by an earlier Setup - and those are
  // exactly the ones carrying the accumulated index caches.
  CacheDirectory := AddBackslash(DirectoryName) + 'appdata';
  if FindFirst(AddBackslash(CacheDirectory) + 'vfs-index-*.cache', Records) then
  begin
    try
      repeat
        DeleteFile(AddBackslash(CacheDirectory) + Records.Name);
      until not FindNext(Records);
    finally
      FindClose(Records);
    end;
  end;
end;

function OriginalX86BackupAvailable(DirectoryName: String): Boolean;
begin
  Result :=
    DirExists(AddBackslash(DirectoryName) + '.dead-air-x64\backup-x86') and
    FileExists(AddBackslash(DirectoryName) + '.dead-air-x64\original-files.txt');
end;

function RestoreOriginalX86Runtime(DirectoryName: String): Boolean;
var
  CurrentManagedFiles: TArrayOfString;
  OriginalFiles: TArrayOfString;
  Index: Integer;
  FileName: String;
  TargetPath: String;
  BackupPath: String;
begin
  Result := False;
  if not LoadManagedFilesFromControl(DirectoryName, CurrentManagedFiles) or
     not LoadStringsFromFile(
       AddBackslash(DirectoryName) + '.dead-air-x64\original-files.txt',
       OriginalFiles) then
  begin
    exit;
  end;

  for Index := 0 to GetArrayLength(OriginalFiles) - 1 do
  begin
    FileName := Trim(OriginalFiles[Index]);
    BackupPath := AddBackslash(DirectoryName) + '.dead-air-x64\backup-x86\' + FileName;
    if not FileExists(BackupPath) then
      exit;
  end;

  for Index := 0 to GetArrayLength(CurrentManagedFiles) - 1 do
  begin
    FileName := Trim(CurrentManagedFiles[Index]);
    TargetPath := AddBackslash(DirectoryName) + FileName;
    if StringArrayContains(OriginalFiles, FileName) then
    begin
      BackupPath := AddBackslash(DirectoryName) + '.dead-air-x64\backup-x86\' + FileName;
      if not ForceDirectories(ExtractFileDir(TargetPath)) or
         not CopyFile(BackupPath, TargetPath, False) then
      begin
        exit;
      end;
    end
    else if FileExists(TargetPath) and not DeleteFile(TargetPath) then
    begin
      exit;
    end;
  end;

  Result := True;
end;

procedure RemoveX64Runtime(DirectoryName: String);
var
  Manifest: TArrayOfString;
  Index: Integer;
begin
  if LoadManagedFilesFromControl(DirectoryName, Manifest) then
  begin
    for Index := 0 to GetArrayLength(Manifest) - 1 do
      DeleteFile(AddBackslash(DirectoryName) + Trim(Manifest[Index]));
  end;
end;

function IsReparsePoint(Path: String): Boolean;
var
  Attributes: Cardinal;
begin
  Attributes := GetFileAttributesW(Path);
  Result := (Attributes <> InvalidFileAttributes) and
    ((Attributes and FileAttributeReparsePoint) <> 0);
end;

// Inno has no regular expressions, and this is a delete authority - a bare
// xtra_dead_air_x64_content_*.xdb0 wildcard would also match a third-party archive that happens
// to share the prefix. Every wildcard hit goes through this before it is deleted.
// Shape: xtra_dead_air_x64_content_<group>_<NN>_<16 lowercase hex>.xdb0
function ContentBundleNameValid(Name: String): Boolean;
var
  Prefix: String;
  Body: String;
  Index: Integer;
  Character: Char;
begin
  Result := False;
  Prefix := 'xtra_dead_air_x64_content_';
  if (Pos(Prefix, Name) <> 1) or (Length(Name) < Length(Prefix) + 24) then
    exit;
  if Lowercase(Copy(Name, Length(Name) - 4, 5)) <> '.xdb0' then
    exit;

  Body := Copy(Name, Length(Prefix) + 1, Length(Name) - Length(Prefix) - 5);
  // "<group>_<NN>_<16 hex>" - so at least one group character plus 20 fixed ones.
  if Length(Body) < 21 then
    exit;

  for Index := Length(Body) - 15 to Length(Body) do
  begin
    Character := Body[Index];
    if not (((Character >= '0') and (Character <= '9')) or
            ((Character >= 'a') and (Character <= 'f'))) then
      exit;
  end;
  if Body[Length(Body) - 16] <> '_' then
    exit;
  if not ((Body[Length(Body) - 18] >= '0') and (Body[Length(Body) - 18] <= '9')) then
    exit;
  if not ((Body[Length(Body) - 17] >= '0') and (Body[Length(Body) - 17] <= '9')) then
    exit;
  if Body[Length(Body) - 19] <> '_' then
    exit;

  for Index := 1 to Length(Body) - 19 do
  begin
    Character := Body[Index];
    if not (((Character >= 'a') and (Character <= 'z')) or
            ((Character >= '0') and (Character <= '9')) or (Character = '_')) then
      exit;
  end;
  Result := True;
end;

// Removes the game content. Runs BEFORE anything that deletes managed files, because
// content-manifest.txt is one of them and it is the only record of what should have been
// removed.
function RemoveContentBundles(GameDirectory: String; var Remaining: String): Boolean;
var
  DatabaseDirectory: String;
  ManifestLines: TArrayOfString;
  Line: String;
  Index: Integer;
  InBundles: Boolean;
  FirstTab: Integer;
  SecondTab: Integer;
  ThirdTab: Integer;
  BundleName: String;
  Records: TFindRec;
  Target: String;
begin
  Result := True;
  Remaining := '';
  DatabaseDirectory := AddBackslash(GameDirectory) + 'database';

  // If database\ is a junction, deleting through it would destroy another installation's
  // bundles. Refuse before touching anything rather than after.
  if IsReparsePoint(DatabaseDirectory) then
  begin
    Remaining := DatabaseDirectory + ' (символьная ссылка - содержимое не удалялось)';
    Result := False;
    exit;
  end;

  // The manifest names exactly what this installation was supposed to have.
  InBundles := False;
  if LoadStringsFromFile(AddBackslash(GameDirectory) + '.dead-air-x64\content-manifest.txt',
    ManifestLines) then
  begin
    for Index := 0 to GetArrayLength(ManifestLines) - 1 do
    begin
      Line := Trim(ManifestLines[Index]);
      if Line = '[bundles]' then
      begin
        InBundles := True;
        Continue;
      end;
      if (Line = '[deltas]') or (Copy(Line, 1, 1) = '[') then
      begin
        InBundles := False;
        Continue;
      end;
      if not InBundles or (Line = '') then
        Continue;

      FirstTab := Pos(#9, Line);
      if FirstTab <= 0 then
        Continue;
      SecondTab := FirstTab + Pos(#9, Copy(Line, FirstTab + 1, Length(Line) - FirstTab));
      if SecondTab <= FirstTab then
        Continue;
      ThirdTab := SecondTab + Pos(#9, Copy(Line, SecondTab + 1, Length(Line) - SecondTab));
      if ThirdTab <= SecondTab then
        ThirdTab := Length(Line) + 1;

      BundleName := Copy(Line, SecondTab + 1, ThirdTab - SecondTab - 1);
      if ContentBundleNameValid(BundleName) then
        DeleteFile(AddBackslash(DatabaseDirectory) + BundleName);
    end;
  end;

  // Then anything bundle-shaped the manifest did not name: a stale revision, or content from a
  // version this installation was updated away from.
  if FindFirst(AddBackslash(DatabaseDirectory) + 'xtra_dead_air_x64_content_*.xdb0', Records) then
  begin
    try
      repeat
        if (Records.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
          Continue;
        if not ContentBundleNameValid(Records.Name) then
          Continue;

        Target := AddBackslash(DatabaseDirectory) + Records.Name;
        if DeleteFile(Target) then
          Continue;

        // Held open by something. A delayed delete needs administrator rights, so its return
        // value decides whether this is reported as left behind - it is not an assumption.
        if not MoveFileExDeleteW(Target, 0, MoveFileDelayUntilReboot) then
        begin
          Remaining := Remaining + Target + #13#10;
          Result := False;
        end;
      until not FindNext(Records);
    finally
      FindClose(Records);
    end;
  end;

  DelTree(AddBackslash(GameDirectory) + '.dead-air-x64\content-cache', True, True, True);
end;

procedure LayoutMaintenanceForm(Sender: TObject);
var
  Dpi: Cardinal;
  BorderSize: Integer;
  ClientWidth: Integer;
  ClientHeight: Integer;
begin
  Dpi := GetDpiForWindow(MaintenanceForm.Handle);
  if Dpi = 0 then
    Dpi := 96;
  BorderSize := GetSystemMetricsForDpi(SmCxFrame, Dpi) +
    GetSystemMetricsForDpi(SmCxPaddedBorder, Dpi);
  ClientWidth := MaintenanceForm.ClientWidth - 2 * BorderSize;
  ClientHeight := MaintenanceForm.ClientHeight -
    GetSystemMetricsForDpi(SmCyCaption, Dpi) - 2 * BorderSize;

  MaintenanceContentPanel.SetBounds(
    0, 0, ClientWidth, ClientHeight - MaintenanceFooterHeight);
  MaintenanceFooterPanel.SetBounds(
    0, ClientHeight - MaintenanceFooterHeight, ClientWidth, MaintenanceFooterHeight);
  MaintenanceCancelButton.SetBounds(
    ClientWidth - MaintenanceButtonWidth - ScaleX(10),
    18, MaintenanceButtonWidth, 36);
  MaintenanceOkButton.SetBounds(
    MaintenanceCancelButton.Left - MaintenanceButtonWidth - ScaleX(6),
    18, MaintenanceButtonWidth, 36);
end;

function ShowUninstallActionDialog: Integer;
var
  ActionForm: TSetupForm;
  ContentPanel: TPanel;
  FooterPanel: TPanel;
  FooterSeparator: TBevel;
  HeaderImage: TBitmapImage;
  TitleLabel: TNewStaticText;
  SubtitleLabel: TNewStaticText;
  DescriptionLabel: TNewStaticText;
  ActionLabel: TNewStaticText;
  OkButton: TNewButton;
  CancelButton: TNewButton;
  ButtonWidth: Integer;
  ImagePath: String;
begin
  Result := UninstallActionCancel;
  ActionForm := CreateCustomForm(500, 253, False, True);
  try
    ActionForm.Caption := 'Удаление — Dead Air: Refined {#PortVersion}';
    ActionForm.Position := poScreenCenter;

    FooterPanel := TPanel.Create(ActionForm);
    FooterPanel.Parent := ActionForm;
    FooterPanel.BevelOuter := bvNone;
    FooterPanel.Color := clBtnFace;
    FooterPanel.ParentBackground := False;

    FooterSeparator := TBevel.Create(ActionForm);
    FooterSeparator.Parent := FooterPanel;
    FooterSeparator.Align := alTop;
    FooterSeparator.Height := ScaleY(2);
    FooterSeparator.Shape := bsTopLine;

    ContentPanel := TPanel.Create(ActionForm);
    ContentPanel.Parent := ActionForm;
    ContentPanel.BevelOuter := bvNone;
    ContentPanel.Color := clWhite;
    ContentPanel.ParentBackground := False;

    TitleLabel := TNewStaticText.Create(ActionForm);
    TitleLabel.Parent := ContentPanel;
    TitleLabel.SetBounds(ScaleX(18), ScaleY(11),
      ActionForm.ClientWidth - ScaleX(90), ScaleY(20));
    TitleLabel.AutoSize := False;
    TitleLabel.Color := clWhite;
    TitleLabel.Font.Style := [fsBold];
    TitleLabel.Caption := 'Выберите действие';

    SubtitleLabel := TNewStaticText.Create(ActionForm);
    SubtitleLabel.Parent := ContentPanel;
    SubtitleLabel.SetBounds(ScaleX(34), ScaleY(31),
      ActionForm.ClientWidth - ScaleX(106), ScaleY(20));
    SubtitleLabel.AutoSize := False;
    SubtitleLabel.Color := clWhite;
    SubtitleLabel.Caption := 'Удаление или восстановление Dead Air: Refined';

    ImagePath := ExpandConstant('{app}\.dead-air-x64\maintenance-small.png');
    if FileExists(ImagePath) then
    begin
      HeaderImage := TBitmapImage.Create(ActionForm);
      HeaderImage.Parent := ContentPanel;
      HeaderImage.SetBounds(ActionForm.ClientWidth - ScaleX(60), ScaleY(7),
        ScaleX(50), ScaleY(50));
      HeaderImage.BackColor := clNone;
      HeaderImage.Stretch := True;
      HeaderImage.PngImage.LoadFromFile(ImagePath);
    end;

    DescriptionLabel := TNewStaticText.Create(ActionForm);
    DescriptionLabel.Parent := ContentPanel;
    DescriptionLabel.SetBounds(ScaleX(34), ScaleY(76),
      ActionForm.ClientWidth - ScaleX(68), ScaleY(42));
    DescriptionLabel.AutoSize := False;
    DescriptionLabel.Color := clWhite;
    DescriptionLabel.WordWrap := True;
    DescriptionLabel.Caption :=
      'Dead Air: Refined {#PortVersion} будет удалён вместе с загруженным игровым контентом.';

    ActionLabel := TNewStaticText.Create(ActionForm);
    ActionLabel.Parent := ContentPanel;
    ActionLabel.SetBounds(ScaleX(34), ScaleY(128),
      ActionForm.ClientWidth - ScaleX(68), ScaleY(22));
    ActionLabel.AutoSize := False;
    ActionLabel.Color := clWhite;
    if OriginalX86BackupAvailable(ExpandConstant('{app}')) then
      ActionLabel.Caption := 'Будет восстановлена исходная версия игры.'
    else
      ActionLabel.Caption := 'Файлы Dead Air: Refined будут удалены.';

    OkButton := TNewButton.Create(ActionForm);
    OkButton.Parent := FooterPanel;
    OkButton.Caption := 'Далее >';
    OkButton.Default := True;
    OkButton.ModalResult := mrOk;

    CancelButton := TNewButton.Create(ActionForm);
    CancelButton.Parent := FooterPanel;
    CancelButton.Caption := 'Отмена';
    CancelButton.Cancel := True;
    CancelButton.ModalResult := mrCancel;

    ButtonWidth := ActionForm.CalculateButtonWidth([OkButton.Caption, CancelButton.Caption]);
    MaintenanceForm := ActionForm;
    MaintenanceContentPanel := ContentPanel;
    MaintenanceFooterPanel := FooterPanel;
    MaintenanceOkButton := OkButton;
    MaintenanceCancelButton := CancelButton;
    MaintenanceButtonWidth := ButtonWidth;
    ActionForm.OnShow := @LayoutMaintenanceForm;

    if ActionForm.ShowModal = mrOk then
      Result := UninstallActionRemove;
  finally
    ActionForm.Free;
  end;
end;

#ifndef MaintenanceOnly
// Cancel during the fetch. Inno's own handling would terminate Setup and orphan the fetcher,
// which would keep downloading into a directory nobody is going to use; the flag file stops it
// first. Whatever it managed to fetch stays: the cache is the resume state, so cancelling is a
// pause rather than a discard.
procedure CancelButtonClick(CurrentPageId: Integer; var Cancel, Confirm: Boolean);
begin
  if not ContentFetchRunning then
    exit;

  SaveStringToFile(AddBackslash(ContentCacheDirectory(WizardDirValue)) + 'content-fetch-cancel.txt',
    'cancel', False);
  ContentFetchCancelled := True;
end;
#endif

procedure CurStepChanged(CurrentStep: TSetupStep);
var
  ImagePath: String;
#ifndef MaintenanceOnly
  ResultCode: Integer;
#endif
begin
  if CurrentStep <> ssPostInstall then
    exit;

  ImagePath := ExpandConstant('{app}\.dead-air-x64\maintenance-small.png');
  WizardForm.WizardSmallBitmapImage.PngImage.SaveToFile(ImagePath);

#ifndef MaintenanceOnly
  // The payload is installed, so .dead-air-x64\content-manifest.txt is now this version's.
  // Move the downloaded bundles into database\ and clear the latch.
  //
  // An Inno install cannot be failed from here - CurStepChanged returns nothing. So a failed
  // commit is not a rolled-back install: the latch stays, the engine refuses to mount what it
  // cannot vouch for, and the first launch goes straight to repair. Say that plainly rather
  // than pretending anything was undone.
  if not Exec(ExpandConstant('{tmp}\DeadAirContent.exe'),
    '--content-commit --game-dir ' + QuoteParameter(ExpandConstant('{app}')),
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then
  begin
    SuppressibleMsgBox('Не удалось установить игровой контент.' + #13#10 +
      'Игра предложит восстановить его при первом запуске.', mbError, MB_OK, IDOK);
    exit;
  end;

  DeleteFile(AddBackslash(ContentCacheDirectory(ExpandConstant('{app}'))) + 'pending-manifest.txt');
#endif
end;

function InitializeUninstall: Boolean;
var
  ActionName: String;
  Action: Integer;
begin
  Result := False;
  RestoreOriginalX86OnUninstall := False;
  ActionName := UninstallActionParameter;
  Log('Maintenance action: ' + ActionName);

  // /ACTION=rollback is no longer a thing. It is not rejected either - a stale shortcut must
  // not produce an error dialog - it simply falls through to the removal prompt, which in a
  // silent context declines and does nothing.
  if ActionName = 'remove' then
    Action := UninstallActionRemove
  else
    Action := ShowUninstallActionDialog;

  if Action = UninstallActionCancel then
    exit;

  RestoreOriginalX86OnUninstall := OriginalX86BackupAvailable(ExpandConstant('{app}'));

  if ActionName <> 'ask' then
  begin
    Result := True;
    exit;
  end;

  if RestoreOriginalX86OnUninstall then
    Result := MsgBox(
      'Dead Air: Refined будет удалена. После удаления будет восстановлена исходная версия игры.' +
      '' + #13#10 + #13#10 + 'Продолжить?',
      mbConfirmation,
      MB_YESNO) = IDYES
  else
    Result := MsgBox(
      'Dead Air: Refined будет полностью удалена.' + #13#10 +
      'Ранее установленная версия восстанавливаться не будет.' +
      '' + #13#10 + #13#10 + 'Продолжить?',
      mbConfirmation,
      MB_YESNO) = IDYES;
end;

procedure CurUninstallStepChanged(CurrentStep: TUninstallStep);
var
  GameDirectory: String;
  ContentRemoved: Boolean;
  Remaining: String;
begin
  if CurrentStep <> usUninstall then
    exit;

  GameDirectory := ExpandConstant('{app}');

  // Content goes first, and the ordering is load-bearing: content-manifest.txt is a managed
  // file, so RemoveX64Runtime, RestoreOriginalX86Runtime and RemoveControlMetadata all delete
  // it. Losing it before the sweep would destroy the only record of what had to be removed and
  // leave gigabytes behind that nothing can name. The restore failure below exits early, and
  // that is harmless now precisely because content has already been dealt with.
  ContentRemoved := RemoveContentBundles(GameDirectory, Remaining);

  if RestoreOriginalX86OnUninstall then
  begin
    if not RestoreOriginalX86Runtime(GameDirectory) then
    begin
      MsgBox(
        'Не удалось восстановить исходную версию игры. Резервная копия сохранена в служебной папке программы.',
        mbError,
        MB_OK);
      exit;
    end;
    DelTree(AddBackslash(GameDirectory) + '.dead-air-x64\backup-x86', True, True, True);
  end
  else
    RemoveX64Runtime(GameDirectory);

  if not ContentRemoved then
  begin
    MsgBox('Часть файлов игрового контента удалить не удалось. Их можно удалить вручную:' +
      Chr(13) + Chr(10) + Chr(13) + Chr(10) + Remaining, mbError, MB_OK);
  end;

  RemoveControlMetadata(GameDirectory);

  // Unconditional: the updater deletes its own in-flight snapshot on success, so anything left
  // here is orphaned by definition.
  DelTree(AddBackslash(GameDirectory) + '.dead-air-x64\backups', True, True, True);
end;
