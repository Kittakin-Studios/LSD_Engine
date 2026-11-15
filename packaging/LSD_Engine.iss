; LSD_Engine installer (Inno Setup 6). Installs the folder packaging/make_portable.ps1 assembles,
; so run that first. From the repository root:
;   powershell -ExecutionPolicy Bypass -File packaging/make_portable.ps1
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.0 packaging\LSD_Engine.iss
; Output: build\package\LSD_Engine_Setup.exe

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#define AppName "LSD Engine"
#define PortableDir "..\build\package\LSD_Engine"

[Setup]
AppId={{6F1C2B7E-4A3D-4E8B-9C51-2D7A0B8E4F13}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Kittakin Studios
AppPublisherURL=https://github.com/Kittakin-Studios/LSD_Engine
AppSupportURL=https://github.com/Kittakin-Studios/LSD_Engine/issues
AppUpdatesURL=https://github.com/Kittakin-Studios/LSD_Engine/releases
; Per user: the editor writes Saved/, DerivedData/ and new Content/ next to itself (its folder is the project),
; so it must live where the user can write without administrator rights.
DefaultDirName={localappdata}\Programs\LSD_Engine
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile={#PortableDir}\LICENSE
OutputDir=..\build\package
OutputBaseFilename=LSD_Engine_Setup
UninstallDisplayIcon={app}\lsd_editor.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

[Types]
Name: "full"; Description: "Editor, sample project and documentation"
Name: "compact"; Description: "Editor and sample project"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "main"; Description: "LSD Engine editor and sample project"; Types: full compact custom; Flags: fixed
Name: "docs"; Description: "Documentation (docs folder, Markdown with pictures)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PortableDir}\*"; DestDir: "{app}"; Excludes: "\docs"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: main
Source: "{#PortableDir}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: docs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\lsd_editor.exe"; WorkingDir: "{app}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\lsd_editor.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\lsd_editor.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; What the editor made while it ran (caches, logs, settings); the user's own Content/ changes are left alone.
Type: filesandordirs; Name: "{app}\DerivedData"
Type: filesandordirs; Name: "{app}\Saved"
Type: filesandordirs; Name: "{app}\Intermediate"
