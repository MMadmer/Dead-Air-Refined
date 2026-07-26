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

var
  ModePage: TInputOptionWizardPage;
  BackupPage: TInputOptionWizardPage;
  RuntimeFiles: TArrayOfString;
  RestoreUpgradeBackupOnUninstall: Boolean;

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

function RestoreBackupParameter: String;
begin
  Result := Lowercase(Trim(ExpandConstant('{param:RESTOREBACKUP|ask}')));
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

  if FileExists(AddBackslash(DirectoryName) + '.dead-air-x64\install-state.json') then
  begin
    Result :=
      'Здесь установлена ранняя PowerShell-версия Dead Air x64.' + #13#10 +
      'Удалите её с помощью прежнего Uninstall-DeadAir-x64.ps1, затем повторно запустите установщик.';
    exit;
  end;

  if IsOwnInstallation(DirectoryName, 'standalone') then
  begin
    Result := 'Эта папка уже используется самостоятельной установкой. Выберите режим самостоятельной установки.';
    exit;
  end;

  if not IsOwnInstallation(DirectoryName, 'upgrade') then
  begin
    if not GetBinaryType(AddBackslash(DirectoryName) + 'xrEngine.exe', BinaryType) or
       (BinaryType <> Scs32BitBinary) then
    begin
      Result := 'Режим обновления требует существующий 32-битный xrEngine.exe Dead Air 0.98b.';
    end;
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
    'Выберите один из двух вариантов. Содержимое существующей игры и её сохранения в режиме обновления не изменяются.',
    True,
    False);
  ModePage.Add('Обновить существующую Dead Air 0.98b / Dead Air Revolution II (x86 → x64)');
  ModePage.Add('Установить чистую самостоятельную Dead Air 0.98b x64');

  BackupPage := CreateInputOptionPage(
    wpSelectDir,
    'Резервная копия',
    'Сохранение исходного x86-движка',
    'Резервная копия позволяет восстановить 32-битный движок при удалении x64-порта. Без неё автоматическое восстановление будет невозможно.',
    False,
    False);
  BackupPage.Add('Создать резервную копию исходного x86-движка (рекомендуется)');
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
    (IsStandaloneMode or IsOwnInstallation(WizardDirValue, 'upgrade'));
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

function PrepareUpgradeBackup(DirectoryName: String): String;
var
  ControlDirectory: String;
  BackupDirectory: String;
  OriginalFileNames: TArrayOfString;
  Index: Integer;
  FileName: String;
  SourcePath: String;
  DestinationPath: String;
begin
  Result := '';
  ControlDirectory := AddBackslash(DirectoryName) + '.dead-air-x64';
  BackupDirectory := AddBackslash(ControlDirectory) + 'backup-x86';

  if IsOwnInstallation(DirectoryName, 'upgrade') then
    exit;

  if DirExists(ControlDirectory) then
  begin
    Result := 'Папка .dead-air-x64 уже существует, но не принадлежит этому установщику.';
    exit;
  end;

  if not ForceDirectories(BackupDirectory) then
  begin
    Result := 'Не удалось создать резервную копию исходного x86-движка.';
    exit;
  end;

  SetArrayLength(OriginalFileNames, 0);
  for Index := 0 to GetArrayLength(RuntimeFiles) - 1 do
  begin
    FileName := Trim(RuntimeFiles[Index]);
    SourcePath := AddBackslash(DirectoryName) + FileName;
    if FileExists(SourcePath) then
    begin
      DestinationPath := AddBackslash(BackupDirectory) + FileName;
      if not CopyFile(SourcePath, DestinationPath, False) then
      begin
        Result := 'Не удалось сохранить исходный файл: ' + FileName;
        exit;
      end;
      AppendString(OriginalFileNames, FileName);
    end;
  end;

  if not SaveStringsToFile(
    AddBackslash(ControlDirectory) + 'original-files.txt',
    OriginalFileNames,
    False) then
  begin
    Result := 'Не удалось записать список исходных x86-файлов.';
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ValidationError: String;
  ControlDirectory: String;
  ExistingUpgrade: Boolean;
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

  ExistingUpgrade := IsUpgradeMode and IsOwnInstallation(WizardDirValue, 'upgrade');
  ControlDirectory := AddBackslash(WizardDirValue) + '.dead-air-x64';
  if IsUpgradeMode and not ExistingUpgrade and DirExists(ControlDirectory) then
  begin
    Result := 'Папка .dead-air-x64 уже существует, но не принадлежит этому установщику.';
    exit;
  end;

  if IsUpgradeMode and not ExistingUpgrade and BackupPage.Values[0] then
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

  if not SaveStringToFile(
    AddBackslash(ControlDirectory) + 'port-version.txt',
    '{#ProductVersion}',
    False) then
  begin
    Result := 'Не удалось записать версию x64-порта.';
  end;
end;

function UpgradeBackupAvailable(DirectoryName: String): Boolean;
begin
  Result :=
    DirExists(AddBackslash(DirectoryName) + '.dead-air-x64\backup-x86') and
    FileExists(AddBackslash(DirectoryName) + '.dead-air-x64\original-files.txt');
end;

procedure RemoveControlMetadata(DirectoryName: String);
var
  ControlDirectory: String;
begin
  ControlDirectory := AddBackslash(DirectoryName) + '.dead-air-x64';
  DeleteFile(AddBackslash(ControlDirectory) + 'install-mode.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'original-files.txt');
  DeleteFile(AddBackslash(ControlDirectory) + 'port-version.txt');
end;

function RestoreUpgradeRuntime(DirectoryName: String): Boolean;
var
  Manifest: TArrayOfString;
  OriginalFileNames: TArrayOfString;
  Index: Integer;
  FileName: String;
  TargetPath: String;
  BackupPath: String;
begin
  Result := False;
  if not LoadStringsFromFile(
    AddBackslash(DirectoryName) + '.dead-air-x64\runtime-files.txt',
    Manifest) then
  begin
    MsgBox('Не удалось прочитать список x64-файлов. Восстановление остановлено.', mbError, MB_OK);
    exit;
  end;

  if not LoadStringsFromFile(
    AddBackslash(DirectoryName) + '.dead-air-x64\original-files.txt',
    OriginalFileNames) then
  begin
    MsgBox('Не удалось прочитать список исходных x86-файлов. Восстановление остановлено.', mbError, MB_OK);
    exit;
  end;

  for Index := 0 to GetArrayLength(Manifest) - 1 do
  begin
    FileName := Trim(Manifest[Index]);
    TargetPath := AddBackslash(DirectoryName) + FileName;
    if StringArrayContains(OriginalFileNames, FileName) then
    begin
      BackupPath := AddBackslash(DirectoryName) + '.dead-air-x64\backup-x86\' + FileName;
      if not CopyFile(BackupPath, TargetPath, False) then
      begin
        MsgBox(
          'Не удалось восстановить исходный файл: ' + FileName + #13#10 +
          'Резервная копия сохранена в .dead-air-x64\backup-x86.',
          mbError,
          MB_OK);
        exit;
      end;
    end
    else
    begin
      if FileExists(TargetPath) and not DeleteFile(TargetPath) then
      begin
        MsgBox('Не удалось удалить x64-файл: ' + FileName, mbError, MB_OK);
        exit;
      end;
    end;
  end;

  Result := True;
end;

procedure RemoveX64Runtime(DirectoryName: String);
var
  Manifest: TArrayOfString;
  Index: Integer;
begin
  if LoadStringsFromFile(
    AddBackslash(DirectoryName) + '.dead-air-x64\runtime-files.txt',
    Manifest) then
  begin
    for Index := 0 to GetArrayLength(Manifest) - 1 do
      DeleteFile(AddBackslash(DirectoryName) + Trim(Manifest[Index]));
  end;
end;

function InitializeUninstall: Boolean;
var
  StoredMode: AnsiString;
  RestoreMode: String;
  Response: Integer;
begin
  Result := True;
  RestoreUpgradeBackupOnUninstall := False;

  if not LoadStringFromFile(
    ExpandConstant('{app}\.dead-air-x64\install-mode.txt'),
    StoredMode) or
    (CompareText(Trim(String(StoredMode)), 'upgrade') <> 0) then
  begin
    exit;
  end;

  RestoreMode := RestoreBackupParameter;
  if UpgradeBackupAvailable(ExpandConstant('{app}')) then
  begin
    if RestoreMode = 'yes' then
    begin
      RestoreUpgradeBackupOnUninstall := True;
      exit;
    end;

    if RestoreMode = 'no' then
      exit;

    Response := MsgBox(
      'Обнаружена резервная копия исходного x86-движка.' + #13#10 + #13#10 +
      'Восстановить 32-битный движок при удалении x64-порта?' + #13#10 +
      'После успешного восстановления резервная копия будет удалена.' + #13#10 + #13#10 +
      '«Да» — восстановить x86-движок.' + #13#10 +
      '«Нет» — удалить x64-порт без восстановления и сохранить резервную копию.' + #13#10 +
      '«Отмена» — прекратить удаление.',
      mbConfirmation,
      MB_YESNOCANCEL);

    if Response = IDCANCEL then
    begin
      Result := False;
      exit;
    end;

    RestoreUpgradeBackupOnUninstall := Response = IDYES;
  end
  else
  begin
    if RestoreMode = 'no' then
      exit;

    if RestoreMode = 'yes' then
    begin
      MsgBox(
        'Восстановление невозможно: резервная копия исходного x86-движка отсутствует.',
        mbError,
        MB_OK);
      Result := False;
      exit;
    end;

    Result := MsgBox(
      'Резервная копия исходного x86-движка отсутствует.' + #13#10 +
      'После удаления x64-порта 32-битный движок не будет восстановлен автоматически.' + #13#10 + #13#10 +
      'Продолжить удаление?',
      mbConfirmation,
      MB_YESNO) = IDYES;
  end;
end;

procedure CurUninstallStepChanged(CurrentStep: TUninstallStep);
var
  StoredMode: AnsiString;
  GameDirectory: String;
begin
  if CurrentStep <> usUninstall then
    exit;

  if not LoadStringFromFile(
    ExpandConstant('{app}\.dead-air-x64\install-mode.txt'),
    StoredMode) then
  begin
    MsgBox('Не удалось определить режим установки. Файлы движка не изменены.', mbError, MB_OK);
    exit;
  end;

  GameDirectory := ExpandConstant('{app}');
  if CompareText(Trim(String(StoredMode)), 'upgrade') = 0 then
  begin
    if RestoreUpgradeBackupOnUninstall then
    begin
      if RestoreUpgradeRuntime(GameDirectory) then
      begin
        DelTree(AddBackslash(GameDirectory) + '.dead-air-x64\backup-x86', True, True, True);
        RemoveControlMetadata(GameDirectory);
      end;
    end
    else
    begin
      RemoveX64Runtime(GameDirectory);
      if not UpgradeBackupAvailable(GameDirectory) then
        RemoveControlMetadata(GameDirectory);
    end;
  end
  else if CompareText(Trim(String(StoredMode)), 'standalone') = 0 then
  begin
    RemoveX64Runtime(GameDirectory);
    RemoveControlMetadata(GameDirectory);
  end;
end;
