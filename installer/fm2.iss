; FM2 installer (Inno Setup 6). Built by build-installer.cmd into dist\FM2-Setup-<version>.exe.
;
; What it does:
;  - asks for your own European Forza Motorsport 2 disc image (.iso) and the title update, and checks both
;    against this build (nothing from the game ships with FM2)
;  - optionally takes a folder with your DLC packages
;  - builds the game files from the disc into the install folder, unpacks the title update into
;    Documents\fm2\update, and copies the DLC into Documents\fm2\dlc (per user, no admin rights)
;  - the bundled Python (installer payload) does the disc and title update work; nothing needs installing

#define AppName "Forza Motorsport 2 ReGPU"
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#define Payload "payload"

[Setup]
AppId={{B2D6E4A1-5C7F-4E3B-9A18-3F0D7C2E6B91}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=zer0ducksgiven
DefaultDirName={localappdata}\FM2
DisableProgramGroupPage=yes
DisableDirPage=no
PrivilegesRequired=lowest
OutputDir=dist
OutputBaseFilename=FM2-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\build\tu\fm2.exe
CloseApplications=yes

[Messages]
WelcomeLabel2=This will install {#AppName} {#AppVersion}, a native PC port of Forza Motorsport 2.%n%nYou need your own copy of the European Forza Motorsport 2 disc image (.iso) and its title update. Nothing from the game is included in this installer.%n%nPlease close Forza Motorsport 2 before continuing.

[Tasks]
Name: "desktop"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
; The port
Source: "{#Payload}\build\tu\*"; DestDir: "{app}\build\tu"; Excludes: "nvngx_dlss.dll,xenos_dis.exe"; Flags: ignoreversion
Source: "{#Payload}\scripts\*"; DestDir: "{app}"; Flags: ignoreversion
; The helper and the Python that runs it (installed copy)
Source: "{#Payload}\py\*"; DestDir: "{app}\py"; Flags: ignoreversion recursesubdirs
; The same Python and helper, staged for the wizard's disc and title update checks
Source: "{#Payload}\py\*"; DestDir: "{tmp}"; Flags: dontcopy

[Icons]
Name: "{userprograms}\{#AppName}"; Filename: "{app}\play.cmd"; WorkingDir: "{app}"
Name: "{userdesktop}\{#AppName}"; Filename: "{app}\play.cmd"; WorkingDir: "{app}"; Tasks: desktop

[UninstallDelete]
Type: filesandordirs; Name: "{app}\py"
Type: filesandordirs; Name: "{app}\out"

[Code]
var
  IsoPage: TInputFileWizardPage;
  TuPage: TInputFileWizardPage;
  DlcPage: TInputDirWizardPage;
  Python: String;
  Helper: String;

function DocsDir: String;
begin
  Result := ExpandConstant('{userdocs}\fm2');
end;

function Quoted(S: String): String;
begin
  Result := '"' + S + '"';
end;

procedure InitializeWizard;
begin
  ExtractTemporaryFiles('*');
  Python := ExpandConstant('{tmp}\python.exe');
  Helper := ExpandConstant('{tmp}\setup_game.py');

  IsoPage := CreateInputFilePage(wpSelectDir,
    'Game disc', 'Choose your European disc image',
    'Select the .iso of Forza Motorsport 2 (European disc). It must be the same build as this installer.');
  IsoPage.Add('Disc image (.iso):', 'Disc image (*.iso)|*.iso|All files|*.*', '.iso');

  TuPage := CreateInputFilePage(IsoPage.ID,
    'Title update', 'Choose the title update file',
    'Select the title update file (named like TU_16L61VA_...). It must be the same build as this installer.');
  TuPage.Add('Title update:', 'All files|*.*', '');

  DlcPage := CreateInputDirPage(TuPage.ID,
    'DLC (optional)', 'Choose the folder with your DLC packages',
    'If you have no DLC, keep this folder as it is and the DLC step is skipped. Otherwise choose the folder with your DLC packages.', False, '');
  DlcPage.Add('');
  // A folder must be given. This default has no DLC in it, so it is skipped.
  DlcPage.Values[0] := ExpandConstant('{userdocs}\fm2\dlc');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ResultCode: Integer;
  Iso, Tu: String;
begin
  Result := True;
  if CurPageID = IsoPage.ID then
  begin
    Iso := IsoPage.Values[0];
    if not FileExists(Iso) then
    begin
      MsgBox('Choose your disc image (.iso).', mbError, MB_OK);
      Result := False;
      exit;
    end;
    Exec(Python, '"' + Helper + '" check-iso ' + Quoted(Iso), '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    if ResultCode <> 0 then
    begin
      MsgBox('This disc does not match this build (its default.xex is different). Choose the European Forza Motorsport 2 disc.', mbError, MB_OK);
      Result := False;
    end;
  end
  else if CurPageID = TuPage.ID then
  begin
    Tu := TuPage.Values[0];
    if not FileExists(Tu) then
    begin
      MsgBox('Choose the title update file.', mbError, MB_OK);
      Result := False;
      exit;
    end;
    Exec(Python, '"' + Helper + '" check-tu ' + Quoted(Tu), '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    if ResultCode <> 0 then
    begin
      MsgBox('This title update does not match this build.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  Dlc, Params: String;
begin
  if CurStep = ssPostInstall then
  begin
    Dlc := DlcPage.Values[0];
    if (Dlc = '') or (Dlc = ExpandConstant('{userdocs}\fm2\dlc')) then
      Dlc := '-';
    Params := '"' + ExpandConstant('{app}\py\setup_game.py') + '" install ' +
      Quoted(IsoPage.Values[0]) + ' ' + Quoted(ExpandConstant('{app}\game')) + ' ' +
      Quoted(TuPage.Values[0]) + ' ' + Quoted(Dlc) + ' ' + Quoted(DocsDir);
    WizardForm.StatusLabel.Caption := 'Building the game files from your disc (a few minutes)...';
    WizardForm.ProgressGauge.Style := npbstMarquee;
    if not Exec(ExpandConstant('{app}\py\python.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then
      MsgBox('The game files could not be built (code ' + IntToStr(ResultCode) + '). Run the installer again or check the disc.', mbError, MB_OK);
    WizardForm.ProgressGauge.Style := npbstNormal;
  end;
end;
