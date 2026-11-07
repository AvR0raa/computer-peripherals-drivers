; 外设万能调试器 — Windows 安装包脚本 (Inno Setup 6)
; 编译: ISCC.exe installer\peripheraltool.iss

#define MyAppName "外设万能调试器"
#define MyAppNameEn "PeripheralTool"
#define MyAppVersion "0.6.2"
#define MyAppPublisher "AvR0raa"
#define MyAppURL "https://github.com/AvR0raa/computer-peripherals-drivers"
#define MyAppExeName "PeripheralTool.exe"

[Setup]
AppId={{8E4B2C7A-1F3D-4E5B-9A6C-PERIPHERAL00}
AppName={#MyAppName} ({#MyAppNameEn})
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppNameEn}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\使用说明.txt
OutputDir=..\dist
OutputBaseFilename=PeripheralTool-Setup-{#MyAppVersion}
SetupIconFile=app.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
UninstallDisplayName={#MyAppName} ({#MyAppNameEn})
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\Unofficial\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\dist\PeripheralTool\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
