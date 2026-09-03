; Inno Setup script — установщик SecVMS (Program Files x86, x64-приложение)
; Сборка:  installer\build.ps1   (версию берёт из CMakeLists.txt)
;   или:   ISCC.exe /DAppVersion=0.13.0 SecVMS.iss
;
; ОБНОВЛЕНИЕ: AppId фиксирован, поэтому запуск нового установщика поверх старой версии
; ставит её в ту же папку (UsePreviousAppDir), заменяет файлы, обновляет запись в
; «Программах и компонентах». Настройки в %APPDATA% не затрагиваются. Работающий SecVMS
; перед заменой файлов закрывается (см. [Code]): сначала мягко (WM_CLOSE — приложение
; успевает сохранить сессию), через 6 с — принудительно.

#ifndef AppVersion
  #define AppVersion "0.13.0"
#endif
#define AppName "SecVMS"
#define AppPublisher "crowartie"
#define AppURL "https://github.com/crowartie/SecVMS"
#define AppExe "SecVMS.exe"

[Setup]
AppId={{B2A4E6F0-1C3D-4A5B-9E7F-0A1B2C3D4E5F}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
VersionInfoVersion={#AppVersion}
; По требованию: ставим в "Program Files (x86)" даже для x64-приложения
DefaultDirName={commonpf32}\{#AppName}
; при обновлении — в ту папку, куда ставили раньше, с теми же задачами (ярлыки)
UsePreviousAppDir=yes
UsePreviousTasks=yes
DirExistsWarning=no
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=Output
OutputBaseFilename=SecVMS-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
; Запись в Program Files требует прав администратора (UAC)
PrivilegesRequired=admin
; Приложение 64-битное — не даём ставить на 32-битную Windows
ArchitecturesAllowed=x64compatible
SetupIconFile=..\dist\SecVMS\assets\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName} {#AppVersion}
WizardStyle=modern
LicenseFile=..\LICENSE
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "startupicon"; Description: "Запускать SecVMS при входе в Windows"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Весь самодостаточный дистрибутив (exe + Qt + FFmpeg + плагины + assets)
Source: "..\dist\SecVMS\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon
Name: "{autostartup}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: startupicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[Code]
const
  MutexName = 'Local\SecVMS_SingleInstance';   { тот же мьютекс, что в main.cpp }
  WM_CLOSE  = $0010;

function IsAppRunning(): Boolean;
begin
  Result := CheckForMutexes(MutexName);
end;

{ Закрыть работающий SecVMS перед копированием файлов (обновление поверх). }
procedure CloseRunningApp();
var
  Hwnd: HWND;
  I: Integer;
  ResultCode: Integer;
begin
  if not IsAppRunning() then exit;
  { 1) мягко: WM_CLOSE главному окну — приложение сохранит сессию и выйдет }
  Hwnd := FindWindowByWindowName('SecVMS');
  if Hwnd <> 0 then SendMessage(Hwnd, WM_CLOSE, 0, 0);
  for I := 1 to 30 do begin            { до 6 с }
    if not IsAppRunning() then exit;
    Sleep(200);
  end;
  { 2) принудительно — если не закрылось (например, ждёт подтверждения выхода) }
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM SecVMS.exe /F', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  Sleep(800);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseRunningApp();
  Result := '';
end;

{ Перед удалением тоже закрываем приложение }
function InitializeUninstall(): Boolean;
begin
  CloseRunningApp();
  Result := True;
end;
