; ASB installer (Inno Setup 6). Built by CI:  ISCC /DAppVersion=0.5.0 /DSrcDir=..\out installer\asb.iss
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SrcDir
  #define SrcDir "..\out"
#endif

[Setup]
AppId={{A5B7C1E0-3B4D-4F10-9E2A-6C11A51B0D10}
AppName=ASB
AppVerName=ASB {#AppVersion}
AppVersion={#AppVersion}
AppPublisher=maai
AppPublisherURL=https://maai.lol
AppSupportURL=https://github.com/babymaaj/ASB1
AppUpdatesURL=https://github.com/babymaaj/ASB1/releases
VersionInfoCompany=maai
VersionInfoProductName=ASB
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\maai\ASB
DisableDirPage=yes
DefaultGroupName=maai\ASB
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=..\dist
OutputBaseFilename=ASB-Setup-{#AppVersion}
SetupIconFile=..\assets\asb.ico
UninstallDisplayIcon={app}\asb.ico
UninstallDisplayName=ASB (by maai)
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
; FL Studio keeps the plugin DLL open: offer to close it instead of failing on a locked file
CloseApplications=yes
RestartApplications=no

[Messages]
FinishedLabel=ASB is installed.%n%nIn FL Studio: Options > Manage plugins > Find plugins, then put ASB last on the Master track.

[InstallDelete]
; the plugin's old name (0.1 - 0.4): same plugin ID, so remove it to avoid duplicates in the DAW
Type: filesandordirs; Name: "{commoncf64}\VST3\ASIOStreamBridge.vst3"

[Files]
Source: "{#SrcDir}\ASB.vst3\*"; DestDir: "{commoncf64}\VST3\ASB.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\assets\asb.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\asb_probe.exe"; DestDir: "{app}\tools"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\ASB releases & updates"; Filename: "https://github.com/babymaaj/ASB1/releases"
Name: "{group}\Uninstall ASB"; Filename: "{uninstallexe}"

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\ASB.vst3"

[Run]
Filename: "https://vb-audio.com/Cable/"; Description: "Get VB-CABLE (free virtual cable for Discord / OBS)"; Flags: postinstall shellexec skipifsilent unchecked; Check: not VBCableInstalled

[Code]
function VBCableInstalled(): Boolean;
var
  R: TFindRec;
begin
  Result := FindFirst(ExpandConstant('{sys}\drivers\vbaudio_cable*.sys'), R);
  if Result then FindClose(R);
end;
