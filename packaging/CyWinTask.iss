#define AppName "CyWinTask English"
#ifndef AppVersion
  #define AppVersion "0.7.0"
#endif
#ifndef PublishDir
  #error PublishDir must point to the self-contained publish directory.
#endif
#ifndef ReleaseDir
  #error ReleaseDir must point to the release output directory.
#endif

[Setup]
AppId={{472B87AE-A613-4F21-9D02-A3CA9C10FDF8}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=CyWinTask
AppPublisherURL=https://github.com/HammerheadFistpunch/CyWinTask
DefaultDirName={localappdata}\Programs\CyWinTask English
DefaultGroupName=CyWinTask English
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#PublishDir}\LICENSE.txt
OutputDir={#ReleaseDir}
OutputBaseFilename=CyWinTask-{#AppVersion}-win-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\Assets\cywintask.ico
UninstallDisplayIcon={app}\CyWinTask.exe
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "{#PublishDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\CyWinTask"; Filename: "{app}\CyWinTask.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\CyWinTask"; Filename: "{app}\CyWinTask.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\CyWinTask.exe"; Description: "Launch CyWinTask"; Flags: nowait postinstall skipifsilent
