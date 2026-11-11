; 外设万能调试器 (C++ 版) — Windows 安装包脚本 (Inno Setup 6)
; 编译: ISCC.exe installer\peripheraltool-cpp.iss
; 前置: 先跑 cpp\build-release.bat + cpp\package.bat 的 dist 步骤
;       （dist\PeripheralTool\ 由 windeployqt 组装完成）
; 可选中文向导: 把 ChineseSimplified.isl 放入 Inno Setup 的 Languages\Unofficial\
;   并取消下方 [Languages] 中 chinesesimplified 一行的注释。

#define MyAppName "外设万能调试器"
#define MyAppNameEn "PeripheralTool"
#define MyAppVersion "0.9.0"
#define MyAppPublisher "AvR0raa"
#define MyAppURL "https://github.com/AvR0raa/computer-peripherals-drivers"
#define MyAppExeName "PeripheralTool.exe"

[Setup]
AppId={{8E4B2C7A-1F3D-4E5B-9A6C-52C99F000001}
AppName={#MyAppName} ({#MyAppNameEn} C++)
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppNameEn}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\使用说明.txt
OutputDir=..\dist
OutputBaseFilename=PeripheralTool-Setup-{#MyAppVersion}-cpp
SetupIconFile=app.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
UninstallDisplayName={#MyAppName} ({#MyAppNameEn} C++)
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
;Name: "chinesesimplified"; MessagesFile: "compiler:Languages\Unofficial\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; windeployqt 产物（exe + Qt DLL + 插件 + protocol_db）
Source: "..\dist\PeripheralTool\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{#MyAppName} CLI"; Filename: "{app}\cli.exe"; Parameters: "list"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
