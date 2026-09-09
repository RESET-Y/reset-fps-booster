; Inno Setup script for RESET FPS BOOSTER
; Builds a single Setup.exe that installs the self-contained published app,
; creates Start Menu + optional Desktop shortcuts, and registers a proper
; uninstaller under Windows "Apps & Features".

#define MyAppName "RESET FPS BOOSTER"
#define MyAppPublisher "Lukas Reschke"
#define MyAppExeName "ResetFpsBooster.exe"
#define MyPublishDir "..\publish\ResetFpsBooster"
#define MyIconFile "..\src\ResetFpsBooster\Assets\app.ico"
; Read the version straight from the published exe's file version instead of hand-editing it
; here on every release — the two can no longer silently drift apart.
#define MyAppVersion GetVersionNumbersString(MyPublishDir + "\" + MyAppExeName)

[Setup]
AppId={{B7B7B6C0-4B0E-4A6C-9E7A-6B3B1E6D2F10}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppVerName={#MyAppName} {#MyAppVersion}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
OutputDir=..\dist
OutputBaseFilename=RESET_FPS_BOOSTER_Setup
SetupIconFile={#MyIconFile}
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesInstallIn64BitMode=x64compatible
DisableWelcomePage=no
SetupLogging=yes
; Lets the installer close a running RESET FPS BOOSTER (e.g. via the in-app updater) instead of
; failing because the single-file exe is locked, and optionally relaunch it afterwards.
CloseApplications=yes
RestartApplications=yes
CloseApplicationsFilter=ResetFpsBooster.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#MyPublishDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
