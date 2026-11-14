; 外设万能调试器 (C++ 版) — Windows 安装包脚本 (Inno Setup 6)
; 编译: ISCC.exe installer\peripheraltool-cpp.iss
; 前置: 先跑 cpp\build-release.bat，再手工/脚本组装 dist\PeripheralTool\
;       （windeployqt + protocol_db/db.json，见 cpp/package.bat）
; 中文向导: ChineseSimplified.isl 需在 Inno Setup 的 Languages\Unofficial\（本机已装）

#define MyAppName "外设万能调试器"
#define MyAppNameEn "PeripheralTool"
#define MyAppVersion "0.9.5"
#define MyAppPublisher "AvR0raa"
#define MyAppURL "https://github.com/AvR0raa/computer-peripherals-drivers"
#define MyAppExeName "PeripheralTool.exe"

[Setup]
AppId={{8E4B2C7A-1F3D-4E5B-9A6C-52C99F000001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} v{#MyAppVersion} (C++ 原生版)
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
WizardImageFile=wizard_side.bmp
WizardSmallImageFile=wizard_small.bmp
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
UninstallDisplayName={#MyAppName} v{#MyAppVersion}
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\Unofficial\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式(&D)"; GroupDescription: "附加任务："
Name: "autostart"; Description: "开机自动启动（托盘常驻，推荐）(&S)"; GroupDescription: "附加任务："; Flags: unchecked

[Files]
; windeployqt 产物（exe + Qt DLL + 插件 + protocol_db/db.json）
Source: "..\dist\PeripheralTool\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{#MyAppName} CLI"; Filename: "{app}\cli.exe"; Parameters: "list"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{userstartup}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: autostart

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "立即运行 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; 卸载前先退出托盘常驻进程（避免文件占用）
Filename: "{sys}\taskkill.exe"; Parameters: "/IM PeripheralTool.exe /F"; Flags: runhidden; RunOnceId: "KillApp"
