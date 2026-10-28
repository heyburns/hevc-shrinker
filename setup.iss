; HEVC Video Shrinker Inno Setup Script
; Generates a professional Windows Installer (.exe) for distribution.

[Setup]
AppName=HEVC Video Shrinker
AppVersion=2.0.3
AppPublisher=HEVC Video Shrinker
DefaultDirName={autopf}\HEVCVideoShrinker
DefaultGroupName=HEVC Video Shrinker
UninstallDisplayIcon={app}\hevc_shrinker.exe
Compression=lzma2/max
SolidCompression=yes
OutputDir=dist
OutputBaseFilename=hevc_shrinker_setup_2.0.3_win64
SetupIconFile=app_icon.ico
WizardStyle=modern
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

[Files]
; Run package_portable.py first; both distributions use the same deployed runtime.
Source: "deploy\windows\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Icons]
; Start Menu icon
Name: "{group}\HEVC Video Shrinker"; Filename: "{app}\hevc_shrinker.exe"
; Desktop Shortcut (Linked to desktopicon task, checked by default)
Name: "{autodesktop}\HEVC Video Shrinker"; Filename: "{app}\hevc_shrinker.exe"; Tasks: desktopicon

[Run]
; Option to launch app after installation completes
Description: "Launch HEVC Video Shrinker"; Filename: "{app}\hevc_shrinker.exe"; Flags: postinstall nowait skipifsilent
