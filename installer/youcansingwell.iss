; YouCanSingWell Inno Setup 스크립트. scripts\package.ps1 이 /DAppVersion /DStageDir /DOutDir 를 넘겨 컴파일한다.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\dist\YouCanSingWell"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif

[Setup]
AppId={{3F8A6B1D-2C4E-4F70-9A1B-6D5E4C3B2A10}
AppName=YouCanSingWell
AppVersion={#AppVersion}
AppVerName=YouCanSingWell v{#AppVersion}
AppPublisher=juzerokr
AppPublisherURL=https://github.com/JuZeroKR/YouCanSingWell
AppSupportURL=https://github.com/JuZeroKR/YouCanSingWell/issues
DefaultDirName={autopf}\YouCanSingWell
DefaultGroupName=YouCanSingWell
; 관리자 권한 없이 사용자 폴더(%LOCALAPPDATA%\Programs)에 설치
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir={#OutDir}
OutputBaseFilename=YouCanSingWell-Setup-v{#AppVersion}
SetupIconFile=..\assets\youcansingwell.ico
UninstallDisplayIcon={app}\YouCanSingWell.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE

[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\YouCanSingWell"; Filename: "{app}\YouCanSingWell.exe"
Name: "{group}\노래 · 연습 데이터 폴더"; Filename: "{localappdata}\YouCanSingWell"
Name: "{autodesktop}\YouCanSingWell"; Filename: "{app}\YouCanSingWell.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\YouCanSingWell.exe"; Description: "{cm:LaunchProgram,YouCanSingWell}"; Flags: nowait postinstall skipifsilent

; 제거해도 데이터(%LOCALAPPDATA%\YouCanSingWell)는 남긴다
