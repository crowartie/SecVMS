; Inno Setup script — установщик SecVMS (Program Files x86, x64-приложение)
; Компиляция:  ISCC.exe SecVMS.iss   -> installer\Output\SecVMS-Setup-<версия>.exe

#define AppName "SecVMS"
#define AppVersion "0.13.0"
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
; По требованию: ставим в "Program Files (x86)" даже для x64-приложения
DefaultDirName={commonpf32}\{#AppName}
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
; Закрыть работающий SecVMS перед обновлением/удалением
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
