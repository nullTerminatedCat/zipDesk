; 日常解压安装脚本。在 installer 目录用 ISCC 编译。

#define AppName "日常解压"
#define AppVersion "0.1.0"
#define AppExe "ZipDesk.exe"

[Setup]
AppId={{A7B3E1C4-6D25-4F18-9C0A-2E8B7D4F1A63}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=ZipDesk
DefaultDirName={localappdata}\Programs\ZipDesk
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=ZipDesk-Setup
SetupIconFile=..\resources\app.ico
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加选项:"; Flags: checkedonce
Name: "associations"; Description: "关联 .zip / .7z，并加入资源管理器右键菜单"; GroupDescription: "附加选项:"; Flags: checkedonce

[Files]
Source: "..\build\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\7z.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\7z.sfx"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\build\Qt6Core.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Qt6Gui.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Qt6Network.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Qt6Widgets.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\d3dcompiler_47.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\dxcompiler.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\dxil.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\generic\*"; DestDir: "{app}\generic"; Flags: ignoreversion recursesubdirs
Source: "..\build\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs
Source: "..\build\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs
Source: "..\build\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs
Source: "..\build\styles\*"; DestDir: "{app}\styles"; Flags: ignoreversion recursesubdirs
Source: "..\build\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion recursesubdirs
Source: "..\build\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon


[Run]
Filename: "{app}\{#AppExe}"; Parameters: "--register"; StatusMsg: "正在写入文件关联和右键菜单..."; Flags: runhidden waituntilterminated; Tasks: associations
Filename: "{app}\{#AppExe}"; Description: "启动{#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\{#AppExe}"; Parameters: "--unregister"; Flags: runhidden waituntilterminated

[Code]
const
  SHCNE_ASSOCCHANGED = $08000000;
  SHCNF_IDLIST = $0000;

procedure SHChangeNotify(wEventId: Integer; uFlags: Cardinal; dwItem1, dwItem2: Integer);
  external 'SHChangeNotify@shell32.dll stdcall';

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    if Exec(ExpandConstant('{tmp}\vc_redist.x64.exe'), '/install /quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    begin
      if ResultCode = 3010 then
        ResultCode := 0;
    end;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0);
end;
