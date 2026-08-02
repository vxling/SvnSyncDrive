; SvnSyncDrive installer
; Parameters: /DSourceDir=<staged layout> /DOutputDir=<output> /DVersion=<version>
;   e.g. ISCC.exe /DSourceDir=build\publish\SvnSyncDrive-0.2.0-win64 ^
;                /DOutputDir=build\publish /DVersion=0.2.0 svn_sync_drive.iss

#ifndef SourceDir
  #error "SourceDir not defined"
#endif
#ifndef OutputDir
  #error "OutputDir not defined"
#endif
#ifndef Version
  #error "Version not defined"
#endif

[Setup]
AppId={{D2A31A20-57B1-4E3B-9F5C-7D2E6C1A8B40}
AppName=SvnSyncDrive
AppVersion={#Version}
AppPublisher=vxling
DefaultDirName={userpf}\SvnSyncDrive
DefaultGroupName=SvnSyncDrive
UninstallDisplayIcon={app}\svnsyncdrive.exe
OutputDir={#OutputDir}
OutputBaseFilename=SvnSyncDrive-{#Version}-win64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupLogging=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; VC++ runtime redistributable bundled next to the app by windeployqt.
Source: "{#SourceDir}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\SvnSyncDrive"; Filename: "{app}\svnsyncdrive.exe"
Name: "{autodesktop}\SvnSyncDrive"; Filename: "{app}\svnsyncdrive.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\svnsyncdrive.exe"; Description: "{cm:LaunchProgram,SvnSyncDrive}"; Flags: nowait postinstall skipifsilent

; Install the VC++ runtime only when it is missing (2015-2022 x64 redist key).
[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: skipifsilent; Check: VcRedistMissing()

[Code]
function VcRedistMissing(): Boolean;
var
  Key: string;
begin
  Result := True;
  Key := 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  if RegKeyExists(HKLM, Key) or RegKeyExists(HKCU, Key) then
    Result := False;
end;
