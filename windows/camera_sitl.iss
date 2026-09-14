#ifndef MyAppVersion
#define MyAppVersion "0.1"
#endif
[Setup]
AppId=ArduPilot.CameraGimbalSITL
AppName=ArduPilot Camera/Gimbal SITL
AppVersion={#MyAppVersion}
DefaultDirName={localappdata}\Programs\ArduPilot Camera SITL
DefaultGroupName=ArduPilot Camera SITL
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=Output
OutputBaseFilename=CameraGimbalSITL-Setup
Compression=lzma2
SolidCompression=yes
SetupIconFile=..\assets\camera-gimbal.ico
UninstallDisplayIcon={app}\CameraGimbalSITL.exe
[Files]
Source: "..\dist\CameraGimbalSITL\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Icons]
Name: "{group}\Camera/Gimbal SITL"; Filename: "{app}\CameraGimbalSITL.exe"
Name: "{autodesktop}\Camera/Gimbal SITL"; Filename: "{app}\CameraGimbalSITL.exe"; Tasks: desktopicon
[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked
[Run]
Filename: "{app}\CameraGimbalSITL.exe"; Description: "Launch Camera/Gimbal SITL"; Flags: nowait postinstall skipifsilent
