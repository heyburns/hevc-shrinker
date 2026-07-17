; HEVC Video Shrinker Inno Setup Script
; Generates a professional Windows Installer (.exe) for distribution.

[Setup]
AppName=HEVC Video Shrinker
AppVersion=2.0.2
AppPublisher=HEVC Video Shrinker
DefaultDirName={autopf}\HEVCVideoShrinker
DefaultGroupName=HEVC Video Shrinker
UninstallDisplayIcon={app}\hevc_shrinker.exe
Compression=lzma2/max
SolidCompression=yes
OutputDir=dist
OutputBaseFilename=hevc_shrinker_setup_2.0.2_win64
SetupIconFile=app_icon.ico
WizardStyle=modern
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

[Files]
; Main Executable and Icon
Source: "build\hevc_shrinker.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\app_icon.png"; DestDir: "{app}"; Flags: ignoreversion

; Deployed Qt & MinGW DLLs
Source: "build\*.dll"; DestDir: "{app}"; Flags: ignoreversion

; Deployed Dependency Directories (recursively)
Source: "build\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "build\sqldrivers\*"; DestDir: "{app}\sqldrivers"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "build\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "build\styles\*"; DestDir: "{app}\styles"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "build\translations\*"; DestDir: "{app}\translations"; Flags: ignoreversion recursesubdirs createallsubdirs

; Optional: Include FFmpeg and FFprobe inside the installer if they are placed in the build folder
Source: "build\ffmpeg.exe"; DestDir: "{app}"; Flags: skipifsourcedoesntexist ignoreversion
Source: "build\ffprobe.exe"; DestDir: "{app}"; Flags: skipifsourcedoesntexist ignoreversion

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
