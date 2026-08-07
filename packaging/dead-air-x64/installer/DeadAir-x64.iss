#ifndef RepoRoot
  #error RepoRoot must point to the DeadAir-x64 repository.
#endif
#ifndef PortVersion
  #define PortVersion "1.2.3"
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
Type: dirifempty; Name: "{app}\.dead-air-x64"
Type: dirifempty; Name: "{app}\database"
Type: dirifempty; Name: "{app}"

[Code]
const
  Scs32BitBinary = 0;
  Scs64BitBinary = 6;
  UninstallActionCancel = 0;
  UninstallActionRemove = 1;
  UninstallActionRollback = 2;
  SynchronizeAccess = $00100000;
  InfiniteTimeout = $FFFFFFFF;
  MaintenanceFooterHeight = 74;
  SmCxFrame = 32;
  SmCyCaption = 4;
  SmCxPaddedBorder = 92;

var
  BackupPage: TInputOptionWizardPage;
  RuntimeFiles: TArrayOfString;
  ManagedFiles: TArrayOfString;
  DeleteBackupsOnUninstall: Boolean;
  RestoreOriginalX86OnUninstall: Boolean;
  BackupDirectoryEdit: TNewEdit;
  BackupBrowseButton: TNewButton;
  RemovePatchRadio: TNewRadioButton;
  RollbackPatchRadio: TNewRadioButton;
  DeleteBackupsCheck: TNewCheckBox;
  SelectedBackupDirectory: String;
  SelectedDeleteBackups: Boolean;
  MaintenanceForm: TSetupForm;
  MaintenanceContentPanel: TPanel;
  MaintenanceFooterPanel: TPanel;
  MaintenanceOkButton: TNewButton;
  MaintenanceCancelButton: TNewButton;
  MaintenanceButtonWidth: Integer;

function GetCurrentProcessId: Cardinal;
  external 'GetCurrentProcessId@kernel32.dll stdcall';

function OpenProcess(DesiredAccess: Cardinal; InheritHandle: Boolean; ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';

function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';

function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

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

function BackupParameterEnabled: Boolean;
var
  Value: String;
begin
  Value := Lowercase(Trim(ExpandConstant('{param:BACKUP|yes}')));
  Result := (Value <> 'no') and (Value <> 'off') and (Value <> '0');
end;

function CommandLineParameter(Name: String; DefaultValue: String): String;
begin
  Result := ExpandConstant('{param:' + Name + '|' + DefaultValue + '}');
end;

function UninstallActionParameter: String;
begin
  Result := Lowercase(Trim(CommandLineParameter('ACTION', 'ask')));
end;

function BackupDirectoryParameter: String;
begin
  Result := RemoveBackslashUnlessRoot(Trim(CommandLineParameter('BACKUPDIR', '')));
end;

function DeleteBackupsParameterEnabled: Boolean;
var
  Value: String;
begin
  Value := Lowercase(Trim(CommandLineParameter('DELETEBACKUPS', 'no')));
  Result := (Value = 'yes') or (Value = 'on') or (Value = '1');
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
  BackupPage := CreateInputOptionPage(
    wpSelectDir,
    'Резервная копия',
    'Сохранение текущей версии',
    'Перед заменой файлов установщик может сохранить текущую версию для последующего восстановления.',
    False,
    False);
  BackupPage.Add('Создать резервную копию текущей версии');
  BackupPage.Values[0] := BackupParameterEnabled;

  if TargetParameter <> '' then
    WizardForm.DirEdit.Text := TargetParameter;
end;

function NextButtonClick(CurrentPageId: Integer): Boolean;
var
  ValidationError: String;
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
  AppendUniqueString(ManagedFiles, '.dead-air-x64\unins000.exe');
  AppendUniqueString(ManagedFiles, '.dead-air-x64\unins000.dat');
  AppendUniqueString(ManagedFiles, '.dead-air-x64\unins000.msg');
  AppendUniqueString(ManagedFiles, '.dead-air-x64\maintenance-small.png');
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

function NextBackupDirectory(DirectoryName: String; VersionName: String): String;
var
  BackupRoot: String;
  BaseName: String;
  Suffix: Integer;
begin
  BackupRoot := AddBackslash(DirectoryName) + '.dead-air-x64\backups';
  BaseName := GetDateTimeString('yyyy-mm-dd_hh-nn-ss', '-', ':') + '_' + VersionName;
  Result := AddBackslash(BackupRoot) + BaseName;
  Suffix := 1;
  while DirExists(Result) do
  begin
    Result := AddBackslash(BackupRoot) + BaseName + '_' + IntToStr(Suffix);
    Suffix := Suffix + 1;
  end;
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

function PrepareUpgradeBackup(DirectoryName: String): String;
var
  BackupDirectory: String;
  BackupFilesDirectory: String;
  CurrentManagedFiles: TArrayOfString;
  CaptureFiles: TArrayOfString;
  PresentFiles: TArrayOfString;
  Index: Integer;
  FileName: String;
  SourcePath: String;
  DestinationPath: String;
  VersionName: String;
begin
  Result := '';
  SetArrayLength(CurrentManagedFiles, 0);
  LoadManagedFilesFromControl(DirectoryName, CurrentManagedFiles);

  // Capture the union so upgrades also preserve files removed by the incoming version.
  SetArrayLength(CaptureFiles, 0);
  for Index := 0 to GetArrayLength(CurrentManagedFiles) - 1 do
    AppendUniqueString(CaptureFiles, Trim(CurrentManagedFiles[Index]));
  for Index := 0 to GetArrayLength(ManagedFiles) - 1 do
    AppendUniqueString(CaptureFiles, Trim(ManagedFiles[Index]));

  VersionName := CurrentVersionName(DirectoryName);
  BackupDirectory := NextBackupDirectory(DirectoryName, VersionName);
  BackupFilesDirectory := AddBackslash(BackupDirectory) + 'files';
  if not ForceDirectories(BackupFilesDirectory) then
  begin
    Result := 'Не удалось создать папку резервной копии.';
    exit;
  end;

  SetArrayLength(PresentFiles, 0);
  for Index := 0 to GetArrayLength(CaptureFiles) - 1 do
  begin
    FileName := Trim(CaptureFiles[Index]);
    SourcePath := AddBackslash(DirectoryName) + FileName;
    if FileExists(SourcePath) then
    begin
      DestinationPath := AddBackslash(BackupFilesDirectory) + FileName;
      if not ForceDirectories(ExtractFileDir(DestinationPath)) then
      begin
        Result := 'Не удалось создать структуру резервной копии: ' + FileName;
        exit;
      end;
      if not CopyFile(SourcePath, DestinationPath, False) then
      begin
        Result := 'Не удалось сохранить текущий файл: ' + FileName;
        exit;
      end;
      AppendString(PresentFiles, FileName);
    end;
  end;

  if not SaveStringsToFile(
    AddBackslash(BackupDirectory) + 'present-files.txt',
    PresentFiles,
    False) then
  begin
    Result := 'Не удалось записать список сохранённых файлов.';
    exit;
  end;

  if not SaveStringsToFile(
    AddBackslash(BackupDirectory) + 'restore-scope.txt',
    CaptureFiles,
    False) then
  begin
    Result := 'Не удалось записать область восстановления.';
    exit;
  end;

  if not SaveStringsToFile(
    AddBackslash(BackupDirectory) + 'managed-files.txt',
    CurrentManagedFiles,
    False) then
  begin
    Result := 'Не удалось записать состав сохранённой версии.';
    exit;
  end;

  if not SaveStringToFile(
    AddBackslash(BackupDirectory) + 'port-version.txt',
    VersionName,
    False) then
  begin
    Result := 'Не удалось записать версию резервной копии.';
    exit;
  end;

  if not SaveStringToFile(
    AddBackslash(BackupDirectory) + 'snapshot-kind.txt',
    'refined-version',
    False) then
  begin
    Result := 'Не удалось записать тип резервной копии.';
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
  if BackupPage.Values[0] then
  begin
    if CurrentVersionName(WizardDirValue) = 'original-x86' then
      Result := PrepareOriginalX86Backup(WizardDirValue)
    else
      Result := PrepareUpgradeBackup(WizardDirValue);
    if Result <> '' then
      exit;
  end;

  if not ForceDirectories(ControlDirectory) then
  begin
    Result := 'Не удалось создать служебную папку .dead-air-x64.';
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
begin
  ControlDirectory := AddBackslash(DirectoryName) + '.dead-air-x64';
  DeleteFile(AddBackslash(ControlDirectory) + 'install-mode.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'original-files.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'port-version.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'managed-files.txt');
end;

function LoadBackupSnapshot(
  BackupDirectory: String;
  var BackupFilesDirectory: String;
  var PresentFiles: TArrayOfString;
  var RestoreScope: TArrayOfString;
  var SnapshotManagedFiles: TArrayOfString;
  var SnapshotVersion: String): Boolean;
var
  StoredVersion: AnsiString;
  SnapshotKind: AnsiString;
begin
  Result := False;
  BackupDirectory := RemoveBackslashUnlessRoot(BackupDirectory);
  BackupFilesDirectory := AddBackslash(BackupDirectory) + 'files';

  if not DirExists(BackupFilesDirectory) or
     not LoadStringsFromFile(AddBackslash(BackupDirectory) + 'present-files.txt', PresentFiles) or
     not LoadStringsFromFile(AddBackslash(BackupDirectory) + 'restore-scope.txt', RestoreScope) or
     not LoadStringsFromFile(AddBackslash(BackupDirectory) + 'managed-files.txt', SnapshotManagedFiles) or
     not LoadStringFromFile(AddBackslash(BackupDirectory) + 'snapshot-kind.txt', SnapshotKind) or
     (CompareText(Trim(String(SnapshotKind)), 'refined-version') <> 0) then
  begin
    MsgBox(
      'В выбранной папке не найдена резервная копия ранее установленной версии Dead Air: Refined.' + #13#10 +
      'Выберите резервную копию, созданную при обновлении программы.',
      mbError,
      MB_OK);
    exit;
  end;

  if LoadStringFromFile(AddBackslash(BackupDirectory) + 'port-version.txt', StoredVersion) then
    SnapshotVersion := Trim(String(StoredVersion))
  else
  begin
    MsgBox('Не удалось определить версию выбранной резервной копии.', mbError, MB_OK);
    exit;
  end;

  Result := True;
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

function ValidateBackupFiles(
  BackupFilesDirectory: String;
  PresentFiles: TArrayOfString): Boolean;
var
  Index: Integer;
  FileName: String;
  BackupPath: String;
begin
  Result := True;
  for Index := 0 to GetArrayLength(PresentFiles) - 1 do
  begin
    FileName := Trim(PresentFiles[Index]);
    BackupPath := AddBackslash(BackupFilesDirectory) + FileName;
    if not FileExists(BackupPath) then
    begin
      MsgBox(
        'Резервная копия неполна. Отсутствует файл:' + #13#10 + FileName,
        mbError,
        MB_OK);
      Result := False;
      exit;
    end;
  end;
end;

function RestoreBackupSnapshot(DirectoryName: String; BackupDirectory: String): Boolean;
var
  BackupFilesDirectory: String;
  PresentFiles: TArrayOfString;
  RestoreScope: TArrayOfString;
  SnapshotManagedFiles: TArrayOfString;
  CurrentManagedFiles: TArrayOfString;
  FilesToRemove: TArrayOfString;
  SnapshotVersion: String;
  ControlDirectory: String;
  Index: Integer;
  FileName: String;
  TargetPath: String;
  BackupPath: String;
begin
  Result := False;
  if not LoadBackupSnapshot(
    BackupDirectory,
    BackupFilesDirectory,
    PresentFiles,
    RestoreScope,
    SnapshotManagedFiles,
    SnapshotVersion) then
  begin
    exit;
  end;

  if not ValidateBackupFiles(BackupFilesDirectory, PresentFiles) then
    exit;

  SetArrayLength(CurrentManagedFiles, 0);
  LoadManagedFilesFromControl(DirectoryName, CurrentManagedFiles);

  // Build the union so files absent from the selected snapshot can be removed after restoration.
  SetArrayLength(FilesToRemove, 0);
  for Index := 0 to GetArrayLength(CurrentManagedFiles) - 1 do
    AppendUniqueString(FilesToRemove, Trim(CurrentManagedFiles[Index]));
  for Index := 0 to GetArrayLength(RestoreScope) - 1 do
    AppendUniqueString(FilesToRemove, Trim(RestoreScope[Index]));

  // Overwrite snapshot files without deleting the working installation first.
  for Index := 0 to GetArrayLength(PresentFiles) - 1 do
  begin
    FileName := Trim(PresentFiles[Index]);
    BackupPath := AddBackslash(BackupFilesDirectory) + FileName;
    TargetPath := AddBackslash(DirectoryName) + FileName;
    if not ForceDirectories(ExtractFileDir(TargetPath)) or
       not CopyFile(BackupPath, TargetPath, False) then
    begin
      MsgBox(
        'Не удалось восстановить файл: ' + FileName + #13#10 +
        'Выбранная резервная копия не изменена.',
        mbError,
        MB_OK);
      exit;
    end;
  end;

  for Index := 0 to GetArrayLength(FilesToRemove) - 1 do
  begin
    FileName := Trim(FilesToRemove[Index]);
    if StringArrayContains(PresentFiles, FileName) then
      continue;

    TargetPath := AddBackslash(DirectoryName) + FileName;
    if FileExists(TargetPath) and not DeleteFile(TargetPath) then
    begin
      MsgBox('Не удалось удалить файл текущей версии: ' + FileName, mbError, MB_OK);
      exit;
    end;
  end;

  ControlDirectory := AddBackslash(DirectoryName) + '.dead-air-x64';
  if not ForceDirectories(ControlDirectory) or
     not SaveStringsToFile(
       AddBackslash(ControlDirectory) + 'managed-files.txt',
       SnapshotManagedFiles,
       False) or
     not SaveStringToFile(
       AddBackslash(ControlDirectory) + 'port-version.txt',
       SnapshotVersion,
       False) then
  begin
    MsgBox(
      'Файлы восстановлены, но не удалось обновить сведения об установленной версии.',
      mbError,
      MB_OK);
    exit;
  end;

  Result := True;
end;

function QuoteParameter(Value: String): String;
begin
  Result := '"' + Value + '"';
end;

function WaitForProcess(ProcessId: Cardinal): Boolean;
var
  ProcessHandle: THandle;
begin
  Result := True;
  if ProcessId = 0 then
    exit;

  ProcessHandle := OpenProcess(SynchronizeAccess, False, ProcessId);
  if ProcessHandle = 0 then
    exit;

  Result := WaitForSingleObject(ProcessHandle, InfiniteTimeout) = 0;
  CloseHandle(ProcessHandle);
end;

procedure ScheduleRollbackCleanup(BackupDirectory: String);
var
  Parameters: String;
  ResultCode: Integer;
begin
  // The setup bootstrap owns its source file until this process has exited.
  Parameters := '/D /Q /C ping 127.0.0.1 -n 3 >NUL & del /F /Q ' +
    QuoteParameter(AddBackslash(BackupDirectory) + 'Dead-Air-Refined-Rollback.exe') + ' ' +
    QuoteParameter(AddBackslash(BackupDirectory) + 'rollback-helper.log');
  Exec(ExpandConstant('{cmd}'), Parameters, '', SW_HIDE, ewNoWait, ResultCode);
end;

function StartRollbackHelper(BackupDirectory: String; Quiet: Boolean): Boolean;
var
  InstalledHelper: String;
  TemporaryHelper: String;
  Parameters: String;
  ResultCode: Integer;
begin
  Result := False;
  InstalledHelper := ExpandConstant('{app}\.dead-air-x64\Dead-Air-Refined-Maintenance.exe');
  TemporaryHelper := AddBackslash(BackupDirectory) + 'Dead-Air-Refined-Rollback.exe';

  if not FileExists(InstalledHelper) or
     not CopyFile(InstalledHelper, TemporaryHelper, False) then
  begin
    MsgBox('Не удалось подготовить восстановление выбранной версии.', mbError, MB_OK);
    exit;
  end;

  Parameters := '/CURRENTUSER /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DELETEAFTERINSTALL' +
    ' /ROLLBACKTARGET=' + QuoteParameter(ExpandConstant('{app}')) +
    ' /BACKUPDIR=' + QuoteParameter(BackupDirectory) +
    ' /WAITPID=' + IntToStr(GetCurrentProcessId) +
    ' /LOG=' + QuoteParameter(AddBackslash(BackupDirectory) + 'rollback-helper.log');
  if Quiet then
    Parameters := Parameters + ' /ROLLBACKQUIET=yes';
  Result := Exec(TemporaryHelper, Parameters, '', SW_HIDE, ewNoWait, ResultCode);
  if not Result then
  begin
    DeleteFile(TemporaryHelper);
    MsgBox('Не удалось запустить восстановление выбранной версии.', mbError, MB_OK);
  end;
end;

function InitializeSetup: Boolean;
var
  RollbackTarget: String;
  RollbackBackup: String;
  WaitProcessId: Cardinal;
  Quiet: Boolean;
begin
  Result := True;
  RollbackTarget := RemoveBackslashUnlessRoot(
    Trim(CommandLineParameter('ROLLBACKTARGET', '')));
  if RollbackTarget = '' then
    exit;

  Result := False;
  RollbackBackup := RemoveBackslashUnlessRoot(
    Trim(CommandLineParameter('BACKUPDIR', '')));
  WaitProcessId := StrToIntDef(CommandLineParameter('WAITPID', '0'), 0);
  Quiet := CompareText(CommandLineParameter('ROLLBACKQUIET', 'no'), 'yes') = 0;
  if (RollbackBackup = '') or not WaitForProcess(WaitProcessId) then
  begin
    MsgBox('Не удалось подготовить восстановление выбранной версии.', mbError, MB_OK);
    exit;
  end;

  if RestoreBackupSnapshot(RollbackTarget, RollbackBackup) then
  begin
    if not Quiet then
      MsgBox(
        'Ранее установленная версия Dead Air: Refined успешно восстановлена.',
        mbInformation,
        MB_OK);
    ScheduleRollbackCleanup(RollbackBackup);
  end;
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

procedure UpdateUninstallActionControls(Sender: TObject);
begin
  BackupDirectoryEdit.Enabled := RollbackPatchRadio.Checked;
  BackupBrowseButton.Enabled := RollbackPatchRadio.Checked;
  DeleteBackupsCheck.Enabled := RemovePatchRadio.Checked;
end;

procedure BrowseBackupDirectory(Sender: TObject);
var
  SelectedDirectory: String;
begin
  SelectedDirectory := BackupDirectoryEdit.Text;
  if BrowseForFolder('Выберите папку резервной копии', SelectedDirectory, False) then
    BackupDirectoryEdit.Text := SelectedDirectory;
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
  BackupLabel: TNewStaticText;
  OkButton: TNewButton;
  CancelButton: TNewButton;
  ButtonWidth: Integer;
  ImagePath: String;
begin
  Result := UninstallActionCancel;
  ActionForm := CreateCustomForm(500, 377, False, True);
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
      'Выберите удаление Dead Air: Refined {#PortVersion} или восстановление ранее установленной версии.';

    RemovePatchRadio := TNewRadioButton.Create(ActionForm);
    RemovePatchRadio.Parent := ContentPanel;
    RemovePatchRadio.SetBounds(ScaleX(34), ScaleY(128),
      ActionForm.ClientWidth - ScaleX(68), ScaleY(22));
    if OriginalX86BackupAvailable(ExpandConstant('{app}')) then
      RemovePatchRadio.Caption := 'Удалить программу и восстановить исходную версию игры'
    else
      RemovePatchRadio.Caption := 'Удалить Dead Air: Refined';
    RemovePatchRadio.Checked := True;
    RemovePatchRadio.OnClick := @UpdateUninstallActionControls;

    DeleteBackupsCheck := TNewCheckBox.Create(ActionForm);
    DeleteBackupsCheck.Parent := ContentPanel;
    DeleteBackupsCheck.SetBounds(ScaleX(50), ScaleY(157),
      ActionForm.ClientWidth - ScaleX(84), ScaleY(22));
    DeleteBackupsCheck.Caption := 'Удалить также сохранённые версии программы';
    DeleteBackupsCheck.Checked := DeleteBackupsParameterEnabled;

    RollbackPatchRadio := TNewRadioButton.Create(ActionForm);
    RollbackPatchRadio.Parent := ContentPanel;
    RollbackPatchRadio.SetBounds(ScaleX(34), ScaleY(199),
      ActionForm.ClientWidth - ScaleX(68), ScaleY(22));
    RollbackPatchRadio.Caption := 'Восстановить ранее установленную версию';
    RollbackPatchRadio.OnClick := @UpdateUninstallActionControls;

    BackupLabel := TNewStaticText.Create(ActionForm);
    BackupLabel.Parent := ContentPanel;
    BackupLabel.SetBounds(ScaleX(50), ScaleY(228),
      ActionForm.ClientWidth - ScaleX(84), ScaleY(19));
    BackupLabel.AutoSize := False;
    BackupLabel.Color := clWhite;
    BackupLabel.Caption := 'Папка резервной копии:';

    BackupDirectoryEdit := TNewEdit.Create(ActionForm);
    BackupDirectoryEdit.Parent := ContentPanel;
    BackupDirectoryEdit.SetBounds(ScaleX(50), ScaleY(250),
      ActionForm.ClientWidth - ScaleX(150), ScaleY(23));
    BackupDirectoryEdit.Text := BackupDirectoryParameter;

    BackupBrowseButton := TNewButton.Create(ActionForm);
    BackupBrowseButton.Parent := ContentPanel;
    BackupBrowseButton.SetBounds(ActionForm.ClientWidth - ScaleX(92), ScaleY(249),
      ScaleX(74), ScaleY(25));
    BackupBrowseButton.Caption := 'Обзор...';
    BackupBrowseButton.OnClick := @BrowseBackupDirectory;

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

    UpdateUninstallActionControls(nil);
    if ActionForm.ShowModal = mrOk then
    begin
      SelectedBackupDirectory := BackupDirectoryEdit.Text;
      SelectedDeleteBackups := DeleteBackupsCheck.Checked;
      if RollbackPatchRadio.Checked then
        Result := UninstallActionRollback
      else
        Result := UninstallActionRemove;
    end;
  finally
    ActionForm.Free;
  end;
end;

procedure CurStepChanged(CurrentStep: TSetupStep);
var
  ImagePath: String;
begin
  if CurrentStep <> ssPostInstall then
    exit;

  ImagePath := ExpandConstant('{app}\.dead-air-x64\maintenance-small.png');
  WizardForm.WizardSmallBitmapImage.PngImage.SaveToFile(ImagePath);
end;

function InitializeUninstall: Boolean;
var
  ActionName: String;
  Action: Integer;
  BackupDirectory: String;
begin
  Result := False;
  DeleteBackupsOnUninstall := False;
  RestoreOriginalX86OnUninstall := False;
  SelectedBackupDirectory := '';
  SelectedDeleteBackups := False;
  ActionName := UninstallActionParameter;
  Log('Maintenance action: ' + ActionName);

  if ActionName = 'remove' then
    Action := UninstallActionRemove
  else if ActionName = 'rollback' then
    Action := UninstallActionRollback
  else
    Action := ShowUninstallActionDialog;

  if Action = UninstallActionCancel then
    exit;

  if Action = UninstallActionRollback then
  begin
    BackupDirectory := BackupDirectoryParameter;
    if BackupDirectory = '' then
      BackupDirectory := SelectedBackupDirectory;
    Log('Rollback backup: ' + BackupDirectory);

    if BackupDirectory = '' then
    begin
      MsgBox('Выберите папку резервной копии.', mbError, MB_OK);
      exit;
    end;

    StartRollbackHelper(BackupDirectory, ActionName <> 'ask');
    exit;
  end;

  DeleteBackupsOnUninstall := DeleteBackupsParameterEnabled;
  if ActionName = 'ask' then
    DeleteBackupsOnUninstall := SelectedDeleteBackups;

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
begin
  if CurrentStep <> usUninstall then
    exit;

  GameDirectory := ExpandConstant('{app}');
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

  RemoveControlMetadata(GameDirectory);

  if DeleteBackupsOnUninstall then
  begin
    DelTree(AddBackslash(GameDirectory) + '.dead-air-x64\backups', True, True, True);
  end;
end;
