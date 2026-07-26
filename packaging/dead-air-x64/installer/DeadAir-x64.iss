#ifndef RepoRoot
  #error RepoRoot must point to the DeadAir-x64 repository.
#endif
#ifndef StandaloneSource
  #error StandaloneSource must point to a clean Dead Air 0.98b source.
#endif
#ifndef PortVersion
  #define PortVersion "1.0.0"
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
#ifndef ApplicationId
  #define ApplicationId "{{9732DFF1-E40D-4B23-B215-6D28B1DD0DE0}"
#endif

#define ProductName "Dead Air 0.98b x64"
#define ProductVersion "0.98b-x64-" + PortVersion
#define RuntimeRoot AddBackslash(RepoRoot) + "bin\x64\Release"
#define InstallerRoot AddBackslash(RepoRoot) + "packaging\dead-air-x64\installer"

[Setup]
AppId={#ApplicationId}
AppName={#ProductName}
AppVerName={#ProductName} — порт {#PortVersion}
AppVersion={#ProductVersion}
AppPublisher=Dead Air x64 Project
VersionInfoVersion={#PortVersion}.0
VersionInfoDescription={#ProductName} installer
DefaultDirName={code:GetDefaultDirName}
DefaultGroupName={#ProductName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
WizardSizePercent=110
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
UsePreviousAppDir=no
UsePreviousGroup=yes
Uninstallable=yes
UninstallFilesDir={app}\.dead-air-x64
UninstallLogMode=append
UninstallDisplayName={#ProductName} — порт {#PortVersion}
UninstallDisplayIcon={app}\xrEngine.exe
OutputDir={#OutputDirectory}
OutputBaseFilename=Dead-Air-0.98b-x64-{#PortVersion}-Setup
Compression=none
SolidCompression=no
DiskSpanning=yes
DiskSliceSize=2000000000
SlicesPerDisk=1
CreateAppDir=yes
DirExistsWarning=no

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "Создать ярлык на рабочем столе"; GroupDescription: "Дополнительные ярлыки:"; Flags: unchecked

[Files]
Source: "{#RuntimeRoot}\*.exe"; DestDir: "{app}"; Flags: ignoreversion uninsneveruninstall
Source: "{#RuntimeRoot}\*.dll"; DestDir: "{app}"; Flags: ignoreversion uninsneveruninstall
Source: "{#CompatibilityArchive}"; DestDir: "{app}\database"; Flags: ignoreversion
Source: "{#LauncherPath}"; DestDir: "{app}"; DestName: "Uninstall Dead Air x64.exe"; Flags: ignoreversion
Source: "{#InstallerRoot}\runtime-files.txt"; Flags: dontcopy
Source: "{#InstallerRoot}\runtime-files.txt"; DestDir: "{app}\.dead-air-x64"; Flags: ignoreversion

#ifndef QuickTest
Source: "{#StandaloneSource}\credits.txt"; DestDir: "{app}"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\debug.cmd"; DestDir: "{app}"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\fsgame.ltx"; DestDir: "{app}"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\help.html"; DestDir: "{app}"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\readme.txt"; DestDir: "{app}"; Flags: ignoreversion; Check: IsStandaloneMode

Source: "{#StandaloneSource}\database\configs.xdb0"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\levels.xdb0"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\levels.xdb1"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\levels.xdb2"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\levels.xdb3"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\levels.xdb4"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\meshes.xdb0"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\movie.xdb0"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\sounds.xdb0"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\sounds.xdb1"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\textures.xdb0"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\textures.xdb1"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\textures.xdb2"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
Source: "{#StandaloneSource}\database\xtra.xdb0"; DestDir: "{app}\database"; Flags: ignoreversion; Check: IsStandaloneMode
#endif

[Icons]
Name: "{group}\Dead Air 0.98b x64"; Filename: "{app}\xrEngine.exe"; WorkingDir: "{app}"
Name: "{group}\Удалить Dead Air 0.98b x64"; Filename: "{app}\Uninstall Dead Air x64.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Dead Air 0.98b x64"; Filename: "{app}\xrEngine.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\xrEngine.exe"; Description: "Запустить Dead Air 0.98b x64"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: dirifempty; Name: "{app}\.dead-air-x64"
Type: dirifempty; Name: "{app}\database"
Type: dirifempty; Name: "{app}"

[Code]
const
  UpgradeMode = 0;
  StandaloneMode = 1;
  Scs32BitBinary = 0;
  Scs64BitBinary = 6;
  UninstallActionCancel = 0;
  UninstallActionRemove = 1;
  UninstallActionRollback = 2;

var
  ModePage: TInputOptionWizardPage;
  BackupPage: TInputOptionWizardPage;
  RuntimeFiles: TArrayOfString;
  ManagedFiles: TArrayOfString;
  DeleteBackupsOnUninstall: Boolean;
  BackupDirectoryEdit: TNewEdit;
  BackupBrowseButton: TNewButton;
  RemovePatchRadio: TNewRadioButton;
  RollbackPatchRadio: TNewRadioButton;
  DeleteBackupsCheck: TNewCheckBox;
  SelectedBackupDirectory: String;
  SelectedDeleteBackups: Boolean;

function GetBinaryType(ApplicationName: String; var BinaryType: Cardinal): Boolean;
  external 'GetBinaryTypeW@kernel32.dll stdcall';

function ModeParameter: String;
begin
  Result := Lowercase(Trim(ExpandConstant('{param:MODE|}')));
end;

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

function UninstallActionParameter: String;
begin
  Result := Lowercase(Trim(ExpandConstant('{param:ACTION|ask}')));
end;

function BackupDirectoryParameter: String;
begin
  Result := RemoveBackslashUnlessRoot(Trim(ExpandConstant('{param:BACKUPDIR|}')));
end;

function DeleteBackupsParameterEnabled: Boolean;
var
  Value: String;
begin
  Value := Lowercase(Trim(ExpandConstant('{param:DELETEBACKUPS|no}')));
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

  if ModeParameter = 'standalone' then
  begin
    Result := ExpandConstant('{sd}\Games\Dead Air 0.98b x64');
    exit;
  end;

  ExistingDirectory := ExistingDeadAirDirectory;
  if ExistingDirectory <> '' then
    Result := ExistingDirectory
  else
    Result := ExpandConstant('{sd}\Games\Dead Air');
end;

function IsStandaloneMode: Boolean;
begin
  Result := ModePage.SelectedValueIndex = StandaloneMode;
end;

function IsUpgradeMode: Boolean;
begin
  Result := not IsStandaloneMode;
end;

function InstallModeName: String;
begin
  if IsStandaloneMode then
    Result := 'standalone'
  else
    Result := 'upgrade';
end;

function IsDirectoryEmpty(DirectoryName: String): Boolean;
var
  FindRecord: TFindRec;
begin
  Result := True;
  if not DirExists(DirectoryName) then
    exit;

  if FindFirst(AddBackslash(DirectoryName) + '*', FindRecord) then
  begin
    try
      repeat
        if (FindRecord.Name <> '.') and (FindRecord.Name <> '..') then
        begin
          Result := False;
          exit;
        end;
      until not FindNext(FindRecord);
    finally
      FindClose(FindRecord);
    end;
  end;
end;

function IsOwnInstallation(DirectoryName: String; ExpectedMode: String): Boolean;
var
  StoredMode: AnsiString;
begin
  Result :=
    LoadStringFromFile(AddBackslash(DirectoryName) + '.dead-air-x64\install-mode.txt', StoredMode) and
    (CompareText(Trim(String(StoredMode)), ExpectedMode) = 0);
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
      'Выбранный xrEngine.exe не является поддерживаемым 32- или 64-разрядным движком Dead Air.';
  end;
end;

function ValidateStandaloneDirectory(DirectoryName: String): String;
begin
  Result := '';
  if IsOwnInstallation(DirectoryName, 'upgrade') then
  begin
    Result := 'Эта папка используется обновлённой x86-установкой. Выберите режим обновления существующей игры.';
    exit;
  end;

  if not IsOwnInstallation(DirectoryName, 'standalone') and
     not IsDirectoryEmpty(DirectoryName) then
  begin
    Result :=
      'Для самостоятельной версии требуется новая или пустая папка.' + #13#10 +
      'Установщик не объединяет чистую Dead Air 0.98b с файлами другой установки.';
  end;
end;

function ValidateSelectedDirectory: String;
begin
  if IsStandaloneMode then
    Result := ValidateStandaloneDirectory(WizardDirValue)
  else
    Result := ValidateUpgradeDirectory(WizardDirValue);
end;

procedure InitializeWizard;
begin
  ModePage := CreateInputOptionPage(
    wpWelcome,
    'Тип установки',
    'Как установить Dead Air 0.98b x64?',
    'Выберите один из двух вариантов. Режим обновления подходит как для оригинальной игры, так и для любой уже установленной версии x64-патча.',
    True,
    False);
  ModePage.Add('Обновить оригинальную Dead Air либо установленную версию x64-патча');
  ModePage.Add('Установить чистую самостоятельную Dead Air 0.98b x64');

  BackupPage := CreateInputOptionPage(
    wpSelectDir,
    'Резервная копия',
    'Сохранение текущей версии',
    'Перед заменой файлов установщик может создать независимый снимок текущего движка. Снимок подходит для последующего отката независимо от версии установщика.',
    False,
    False);
  BackupPage.Add('Создать резервную копию текущей версии (рекомендуется)');
  BackupPage.Values[0] := BackupParameterEnabled;

  if ModeParameter = 'standalone' then
    ModePage.SelectedValueIndex := StandaloneMode
  else
    ModePage.SelectedValueIndex := UpgradeMode;

  if TargetParameter <> '' then
    WizardForm.DirEdit.Text := TargetParameter;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result :=
    (PageID = BackupPage.ID) and
    IsStandaloneMode;
end;

function NextButtonClick(CurrentPageId: Integer): Boolean;
var
  ValidationError: String;
begin
  Result := True;

  if CurrentPageId = ModePage.ID then
  begin
    if IsStandaloneMode then
      WizardForm.DirEdit.Text := ExpandConstant('{sd}\Games\Dead Air 0.98b x64')
    else
      WizardForm.DirEdit.Text := GetDefaultDirName('');
  end;

  if CurrentPageId = wpSelectDir then
  begin
    ValidationError := ValidateSelectedDirectory;
    if ValidationError <> '' then
    begin
      MsgBox(ValidationError, mbError, MB_OK);
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
    Result := 'Не удалось прочитать список файлов x64-движка.';
    exit;
  end;
  BuildManagedFiles;

  ControlDirectory := AddBackslash(WizardDirValue) + '.dead-air-x64';
  if IsUpgradeMode and BackupPage.Values[0] then
  begin
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
    InstallModeName,
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
    Result := 'Не удалось записать список управляемых файлов x64-порта.';
    exit;
  end;

  if not SaveStringToFile(
    AddBackslash(ControlDirectory) + 'port-version.txt',
    '{#ProductVersion}',
    False) then
  begin
    Result := 'Не удалось записать версию x64-порта.';
  end;
end;

procedure RemoveControlMetadata(DirectoryName: String);
var
  ControlDirectory: String;
begin
  ControlDirectory := AddBackslash(DirectoryName) + '.dead-air-x64';
  DeleteFile(AddBackslash(ControlDirectory) + 'install-mode.txt');
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
begin
  Result := False;
  BackupDirectory := RemoveBackslashUnlessRoot(BackupDirectory);
  BackupFilesDirectory := AddBackslash(BackupDirectory) + 'files';

  if not DirExists(BackupFilesDirectory) or
     not LoadStringsFromFile(AddBackslash(BackupDirectory) + 'present-files.txt', PresentFiles) or
     not LoadStringsFromFile(AddBackslash(BackupDirectory) + 'restore-scope.txt', RestoreScope) or
     not LoadStringsFromFile(AddBackslash(BackupDirectory) + 'managed-files.txt', SnapshotManagedFiles) then
  begin
    MsgBox(
      'В выбранной папке не найден полный снимок Dead Air x64.' + #13#10 +
      'Требуются папка files и файлы present-files.txt, restore-scope.txt и managed-files.txt.',
      mbError,
      MB_OK);
    exit;
  end;

  if LoadStringFromFile(AddBackslash(BackupDirectory) + 'port-version.txt', StoredVersion) then
    SnapshotVersion := Trim(String(StoredVersion))
  else
    SnapshotVersion := 'restored-backup';

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

  // Remove both version scopes before restoring the snapshot's exact file set.
  SetArrayLength(FilesToRemove, 0);
  for Index := 0 to GetArrayLength(CurrentManagedFiles) - 1 do
    AppendUniqueString(FilesToRemove, Trim(CurrentManagedFiles[Index]));
  for Index := 0 to GetArrayLength(RestoreScope) - 1 do
    AppendUniqueString(FilesToRemove, Trim(RestoreScope[Index]));

  for Index := 0 to GetArrayLength(FilesToRemove) - 1 do
  begin
    FileName := Trim(FilesToRemove[Index]);
    TargetPath := AddBackslash(DirectoryName) + FileName;
    if FileExists(TargetPath) and not DeleteFile(TargetPath) then
    begin
      MsgBox('Не удалось заменить текущий файл: ' + FileName, mbError, MB_OK);
      exit;
    end;
  end;

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
      'Файлы восстановлены, но не удалось обновить служебные данные x64-порта.',
      mbError,
      MB_OK);
    exit;
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

function ShowUninstallActionDialog: Integer;
var
  ActionForm: TSetupForm;
  DescriptionLabel: TNewStaticText;
  BackupLabel: TNewStaticText;
  OkButton: TNewButton;
  CancelButton: TNewButton;
begin
  Result := UninstallActionCancel;
  ActionForm := CreateCustomForm(620, 330, True, True);
  try
    ActionForm.Caption := 'Dead Air 0.98b x64 — обслуживание';
    ActionForm.Position := poScreenCenter;

    DescriptionLabel := TNewStaticText.Create(ActionForm);
    DescriptionLabel.Parent := ActionForm;
    DescriptionLabel.Left := 24;
    DescriptionLabel.Top := 20;
    DescriptionLabel.Width := 572;
    DescriptionLabel.Height := 42;
    DescriptionLabel.AutoSize := False;
    DescriptionLabel.WordWrap := True;
    DescriptionLabel.Caption :=
      'Выберите полное удаление x64-патча либо откат файлов игры до выбранной резервной копии.';

    RemovePatchRadio := TNewRadioButton.Create(ActionForm);
    RemovePatchRadio.Parent := ActionForm;
    RemovePatchRadio.Left := 24;
    RemovePatchRadio.Top := 76;
    RemovePatchRadio.Width := 572;
    RemovePatchRadio.Caption := 'Удалить x64-патч';
    RemovePatchRadio.Checked := True;
    RemovePatchRadio.OnClick := @UpdateUninstallActionControls;

    DeleteBackupsCheck := TNewCheckBox.Create(ActionForm);
    DeleteBackupsCheck.Parent := ActionForm;
    DeleteBackupsCheck.Left := 48;
    DeleteBackupsCheck.Top := 108;
    DeleteBackupsCheck.Width := 548;
    DeleteBackupsCheck.Caption := 'Удалить также все резервные копии из папки игры';
    DeleteBackupsCheck.Checked := DeleteBackupsParameterEnabled;

    RollbackPatchRadio := TNewRadioButton.Create(ActionForm);
    RollbackPatchRadio.Parent := ActionForm;
    RollbackPatchRadio.Left := 24;
    RollbackPatchRadio.Top := 154;
    RollbackPatchRadio.Width := 572;
    RollbackPatchRadio.Caption := 'Откатить файлы игры до резервной копии';
    RollbackPatchRadio.OnClick := @UpdateUninstallActionControls;

    BackupLabel := TNewStaticText.Create(ActionForm);
    BackupLabel.Parent := ActionForm;
    BackupLabel.Left := 48;
    BackupLabel.Top := 187;
    BackupLabel.Width := 548;
    BackupLabel.Caption := 'Папка резервной копии:';

    BackupDirectoryEdit := TNewEdit.Create(ActionForm);
    BackupDirectoryEdit.Parent := ActionForm;
    BackupDirectoryEdit.Left := 48;
    BackupDirectoryEdit.Top := 210;
    BackupDirectoryEdit.Width := 454;
    BackupDirectoryEdit.Text := BackupDirectoryParameter;

    BackupBrowseButton := TNewButton.Create(ActionForm);
    BackupBrowseButton.Parent := ActionForm;
    BackupBrowseButton.Left := 510;
    BackupBrowseButton.Top := 208;
    BackupBrowseButton.Width := 86;
    BackupBrowseButton.Height := 25;
    BackupBrowseButton.Caption := 'Обзор...';
    BackupBrowseButton.OnClick := @BrowseBackupDirectory;

    OkButton := TNewButton.Create(ActionForm);
    OkButton.Parent := ActionForm;
    OkButton.Left := 404;
    OkButton.Top := 280;
    OkButton.Width := 92;
    OkButton.Height := 28;
    OkButton.Caption := 'Продолжить';
    OkButton.Default := True;
    OkButton.ModalResult := mrOk;

    CancelButton := TNewButton.Create(ActionForm);
    CancelButton.Parent := ActionForm;
    CancelButton.Left := 504;
    CancelButton.Top := 280;
    CancelButton.Width := 92;
    CancelButton.Height := 28;
    CancelButton.Caption := 'Отмена';
    CancelButton.Cancel := True;
    CancelButton.ModalResult := mrCancel;

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

function InitializeUninstall: Boolean;
var
  ActionName: String;
  Action: Integer;
  BackupDirectory: String;
begin
  Result := False;
  DeleteBackupsOnUninstall := False;
  SelectedBackupDirectory := '';
  SelectedDeleteBackups := False;
  ActionName := UninstallActionParameter;

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

    if BackupDirectory = '' then
    begin
      MsgBox('Выберите папку резервной копии.', mbError, MB_OK);
      exit;
    end;

    if RestoreBackupSnapshot(ExpandConstant('{app}'), BackupDirectory) then
      MsgBox(
        'Откат завершён. Управление установленной версией Dead Air x64 сохранено.',
        mbInformation,
        MB_OK);
    exit;
  end;

  DeleteBackupsOnUninstall := DeleteBackupsParameterEnabled;
  if ActionName = 'ask' then
    DeleteBackupsOnUninstall := SelectedDeleteBackups;

  Result := MsgBox(
    'x64-файлы будут удалены без автоматического восстановления другой версии.' + #13#10 +
    'Для восстановления версии следует отменить удаление и выбрать откат.' + #13#10 + #13#10 +
    'Продолжить полное удаление x64-патча?',
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
  RemoveX64Runtime(GameDirectory);
  RemoveControlMetadata(GameDirectory);

  if DeleteBackupsOnUninstall then
  begin
    DelTree(AddBackslash(GameDirectory) + '.dead-air-x64\backups', True, True, True);
    DelTree(AddBackslash(GameDirectory) + '.dead-air-x64\backup-x86', True, True, True);
  end;
end;
