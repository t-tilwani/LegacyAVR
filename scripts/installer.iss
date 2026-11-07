; =====================================================================================================
;  LegacyAVR - Inno Setup script
;
;  Normally built by:   .\scripts\build.ps1 -Package
;  which builds Release|x64, runs the tests, stages the release files in out\package\LegacyAVR\ and then
;  compiles this script with ISCC, passing the version and dependency information as /D defines.
;
;  Compiling by hand (Inno Setup IDE or ISCC) also works once out\package\LegacyAVR\ exists.
;  Requires Inno Setup 6.3 or later (Inno Setup 7 is fine). Free for non-commercial use - see
;  https://jrsoftware.org/isorder.php.
;
;  Output: Github Releases\LegacyAVR_v<version>_Setup.exe
;  Keep this file ASCII-only (Inno Setup 7 rejects bytes that are invalid in the file's code page).
;  Never start a line with '#' unless it is a preprocessor directive: in the code section use the NL constant
;  (or end the previous line with the #13#10) - a line that starts with #13#10 stops the compiler.
; =====================================================================================================

#if VER < EncodeVer(6, 3, 0)
  #error Inno Setup 6.3 or later is required: https://jrsoftware.org/isdl.php
#endif

#define RepoRoot SourcePath + ".."

; ---------------------------------------------------------------- values normally passed by build.ps1
#ifndef StageDir
  #define StageDir RepoRoot + "\out\package\LegacyAVR"
#endif
#if !FileExists(StageDir + "\LegacyAVR.UI.exe")
  #error Staged release files not found. Run .\scripts\build.ps1 -Package (it stages bin\Release into out\package\LegacyAVR first).
#endif
#ifndef AppVersion
  #define AppVersion GetVersionNumbersString(StageDir + "\LegacyAVR.UI.exe")
#endif
; Minimum Visual C++ runtime version: the redistributable version of the MSVC toolset that built the engine.
#ifndef VCRedistMin
  #define VCRedistMin "14.0.0"
#endif
; Microsoft's Visual C++ Redistributable (x64) from the Visual Studio that built the engine. build.ps1 copies it
; to out\package\redist\. It is embedded in Setup (not installed as a file) and run only when needed.
; Empty = not embedded: Setup downloads the latest one from Microsoft instead.
#ifndef VCRedistExe
  #define VCRedistExe ""
#endif
#if VCRedistExe != ""
  #if !FileExists(VCRedistExe)
    #error VCRedistExe was given but the file does not exist.
  #endif
  #define BundledVC "True"
#else
  #define BundledVC "False"
#endif
; Direct link to the current .NET 10 Windows Desktop Runtime (x64) installer, resolved from Microsoft's
; release metadata at build time. Empty = setup cannot download it and opens the download page instead.
#ifndef DotNetRuntimeUrl
  #define DotNetRuntimeUrl ""
#endif

#define AppName        "LegacyAVR"
#define AppExe         "LegacyAVR.UI.exe"
#define RepoUrl        "https://github.com/T-Tilwani/LegacyAVR"
#define ServiceName    "LegacyAVREngine"
#define VCRedistUrl    "https://aka.ms/vc14/vc_redist.x64.exe"
#define DotNetPage     "https://dotnet.microsoft.com/download/dotnet/10.0"
#define VBCablePage    "https://vb-audio.com/Cable/"

[Setup]
; AppId identifies LegacyAVR for upgrades and uninstall. Never change it.
AppId={{1E6A9644-D1FE-454B-9CC4-AAEDBF7CD4FF}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=T-Tilwani
AppPublisherURL={#RepoUrl}
AppSupportURL={#RepoUrl}/issues
AppUpdatesURL={#RepoUrl}/releases
AppComments=Real-time Dolby Digital / DTS encoder for older AV receivers
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Program Files needs administrator rights. The app itself runs as the normal user.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 11 (build 22000) or later.
MinVersion=10.0.22000
OutputDir={#RepoRoot}\Github Releases
OutputBaseFilename={#AppName}_v{#AppVersion}_Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
; Close a running dashboard / engine (Restart Manager) if the code below could not.
CloseApplications=force
RestartApplications=no
SetupLogging=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "service"; Description: "Run the audio engine as a Windows service (advanced: starts at boot, before anyone signs in)"; GroupDescription: "Advanced:"; Flags: unchecked

[InstallDelete]
; FFmpeg DLL names contain their major version; remove older ones so only the current set remains.
Type: files; Name: "{app}\avcodec-*.dll"
Type: files; Name: "{app}\avutil-*.dll"
Type: files; Name: "{app}\swresample-*.dll"

[Files]
#if VCRedistExe != ""
; Embedded for Setup's own use only (dontcopy): extracted to {tmp} and run if the runtime is missing.
Source: "{#VCRedistExe}"; DestDir: "{tmp}"; Flags: dontcopy
#endif
; Everything build.ps1 staged: dashboard, engine, service, FFmpeg DLLs, licences, README, scripts.
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Comment: "LegacyAVR dashboard"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"; Comment: "Remove LegacyAVR from this PC"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Comment: "LegacyAVR dashboard"; Tasks: desktopicon

[Run]
; Optional service: re-register it so it points at this installation, then it starts.
Filename: "{app}\AVREngineService.exe"; Parameters: "--uninstall"; Flags: runhidden; Tasks: service; StatusMsg: "Configuring the LegacyAVR engine service..."
Filename: "{app}\AVREngineService.exe"; Parameters: "--install"; Flags: runhidden; Tasks: service; StatusMsg: "Configuring the LegacyAVR engine service..."
; A service installed earlier (and not re-registered above) was stopped for the upgrade: start it again.
Filename: "{sys}\net.exe"; Parameters: "start {#ServiceName}"; Flags: runhidden; Check: RestartServiceAfterInstall
; Finish page. runasoriginaluser: never start the dashboard with Setup's administrator rights.
Filename: "{app}\{#AppExe}"; Description: "Start LegacyAVR now"; Flags: nowait postinstall skipifsilent runasoriginaluser; Check: DotNetRuntimeInstalled

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM LegacyAVR.UI.exe"; Flags: runhidden; RunOnceId: "StopDashboard"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM AVREngine.exe"; Flags: runhidden; RunOnceId: "StopEngine"
Filename: "{app}\AVREngineService.exe"; Parameters: "--uninstall"; Flags: runhidden; RunOnceId: "RemoveService"

[Code]
const
  NL = #13#10;
  DotNetDesktopDir = '\shared\Microsoft.WindowsDesktop.App\';
  VCRuntimeKey = 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValueName = 'LegacyAVR';
  UIMutex = 'LegacyAVR.UI.SingleInstance';
  EngineMutex = 'Global\LegacyAVR.EngineInstance';

var
  NeedVC, NeedDotNet: Boolean;
  ServiceWasRunning, RestartNeeded: Boolean;
  PrereqPage: TInputOptionWizardPage;
  DownloadPage: TDownloadWizardPage;
  PrereqProgressPage: TOutputProgressWizardPage;
  NextStepsPage: TWizardPage;
  MissingLabel, CableLabel, SoundLabel: TNewStaticText;
  VCButton, DotNetButton, CableButton, SoundButton: TNewButton;

{ ------------------------------------------------------------------ dependency detection }

{ True if DotNetRoot\shared\Microsoft.WindowsDesktop.App\10.x exists. }
function DirHasVersion10(const DotNetRoot: String): Boolean;
var
  FindRec: TFindRec;
begin
  Result := False;
  if FindFirst(DotNetRoot + DotNetDesktopDir + '10.*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        begin
          Result := True;
          Break;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

{ .NET 10 Windows Desktop Runtime (x64): Microsoft documents detection by the shared-framework folders. }
function DotNetRuntimeInstalled: Boolean;
var
  Pf: String;
begin
  Pf := ExpandConstant('{commonpf64}');
  { On Arm64 Windows the x64 runtime lives in Program Files\dotnet\x64. }
  Result := DirHasVersion10(Pf + '\dotnet') or DirHasVersion10(Pf + '\dotnet\x64');
  if Result then
    Log('.NET 10 Desktop Runtime (x64): found')
  else
    Log('.NET 10 Desktop Runtime (x64): NOT found');
end;

{ Takes the next dot-separated number from S (e.g. '14.44.35112'). }
function TakeNumber(var S: String): Integer;
var
  P: Integer;
begin
  P := Pos('.', S);
  if P = 0 then
  begin
    Result := StrToIntDef(Trim(S), 0);
    S := '';
  end else
  begin
    Result := StrToIntDef(Trim(Copy(S, 1, P - 1)), 0);
    Delete(S, 1, P);
  end;
end;

function ReadRuntimeValue(const RootKey: Integer; const Name: String; var Value: Integer): Boolean;
var
  D: Cardinal;
begin
  Result := RegQueryDWordValue(RootKey, VCRuntimeKey, Name, D);
  if Result then
    Value := Integer(D)
  else
    Value := 0;
end;

function VCRuntimeOkIn(const RootKey: Integer): Boolean;
var
  Installed, Major, Minor, Bld, MinMajor, MinMinor, MinBld: Integer;
  S: String;
begin
  Result := False;
  if not ReadRuntimeValue(RootKey, 'Installed', Installed) then Exit;
  if Installed <> 1 then Exit;
  if not ReadRuntimeValue(RootKey, 'Major', Major) then Exit;
  ReadRuntimeValue(RootKey, 'Minor', Minor);
  ReadRuntimeValue(RootKey, 'Bld', Bld);
  S := '{#VCRedistMin}';
  MinMajor := TakeNumber(S);
  MinMinor := TakeNumber(S);
  MinBld := TakeNumber(S);
  Log(Format('Visual C++ runtime (x64) %d.%d.%d found, %d.%d.%d or newer required', [Major, Minor, Bld, MinMajor, MinMinor, MinBld]));
  if Major <> MinMajor then
    Result := Major > MinMajor
  else if Minor <> MinMinor then
    Result := Minor > MinMinor
  else
    Result := Bld >= MinBld;
end;

{ Microsoft Visual C++ v14 Redistributable (2015-2026), x64. The key can be in either registry view. }
function VCRuntimeInstalled: Boolean;
begin
  Result := VCRuntimeOkIn(HKLM64) or VCRuntimeOkIn(HKLM32);
  if not Result then
    Log('Visual C++ runtime (x64): missing or older than required');
end;

function HasBundledVC: Boolean;
begin
  Result := {#BundledVC};
end;

function CanDownloadDotNet: Boolean;
begin
  Result := '{#DotNetRuntimeUrl}' <> '';
end;

function AutoInstallPossible: Boolean;
begin
  Result := NeedVC or (NeedDotNet and CanDownloadDotNet);
end;

function MissingComponentsText: String;
begin
  Result := '';
  if NeedVC then
    Result := Result + '  - Microsoft Visual C++ Runtime (x64), needed by the audio engine' + NL;
  if NeedDotNet then
    Result := Result + '  - .NET 10 Desktop Runtime (x64), needed by the dashboard' + NL;
end;

{ ------------------------------------------------------------------ helpers }

procedure OpenUrl(const Url: String);
var
  ErrorCode: Integer;
begin
  { As the original (non-elevated) user, so the browser does not run as administrator. }
  if not ShellExecAsOriginalUser('open', Url, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode) then
    MsgBox('Could not open your web browser. Please visit:' + NL + NL + Url, mbInformation, MB_OK);
end;

{ True if the file has a valid Authenticode signature from Microsoft Corporation. }
function IsMicrosoftSigned(const FileName: String): Boolean;
var
  F, Params: String;
  ResultCode: Integer;
begin
  F := FileName;
  StringChangeEx(F, '''', '''''', True);
  Params := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "' +
    '$s = Get-AuthenticodeSignature -LiteralPath ''' + F + '''; ' +
    'if ($s.Status -eq ''Valid'' -and $s.SignerCertificate.Subject -match ''O=Microsoft Corporation'') { exit 0 } else { exit 1 }"';
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Params, '',
                 SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
  if Result then
    Log('Signature OK (Microsoft): ' + FileName)
  else
    Log('Signature check FAILED: ' + FileName);
end;

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  if Progress = ProgressMax then
    Log(Format('Downloaded %s', [FileName]));
  Result := True;
end;

{ Runs one downloaded Microsoft installer silently. }
procedure RunPrerequisite(const Title, FileName: String; const Step, Steps: Integer; const CheckSignature: Boolean);
var
  ResultCode: Integer;
begin
  PrereqProgressPage.SetText('Installing ' + Title + '...', 'This can take a minute or two. Please wait.');
  PrereqProgressPage.SetProgress(Step, Steps);
  if CheckSignature and not IsMicrosoftSigned(FileName) then
  begin
    SuppressibleMsgBox('The downloaded ' + Title + ' installer could not be verified as signed by Microsoft, ' +
      'so Setup did not run it.' + NL + NL +
      'LegacyAVR will still be installed. The last page of Setup has a button to download ' + Title +
      ' from Microsoft yourself.', mbError, MB_OK, IDOK);
    Exit;
  end;
  if not Exec(FileName, '/install /quiet /norestart', '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) then
    ResultCode := -1;
  Log(Format('%s installer exit code: %d', [Title, ResultCode]));
  case ResultCode of
    0, 1638:
      Log(Title + ' installed (or a newer version is already present).');
    1641, 3010:
      RestartNeeded := True;
  else
    SuppressibleMsgBox(Title + ' could not be installed automatically (error code ' + IntToStr(ResultCode) + ').' +
      NL + NL + 'LegacyAVR will still be installed. The last page of Setup has a button to download ' +
      Title + ' from Microsoft yourself.', mbError, MB_OK, IDOK);
  end;
end;

procedure DownloadAndInstallPrerequisites;
var
  VCFile, DotNetFile: String;
  Steps, Step: Integer;
  ToDownload: Integer;
begin
  VCFile := ExpandConstant('{tmp}\vc_redist.x64.exe');
  DotNetFile := ExpandConstant('{tmp}\windowsdesktop-runtime-win-x64.exe');

  { The Visual C++ runtime installer is embedded in Setup when build.ps1 found it; otherwise download it. }
  if NeedVC and HasBundledVC then
    ExtractTemporaryFile('vc_redist.x64.exe');

  ToDownload := 0;
  DownloadPage.Clear;
  if NeedVC and not HasBundledVC then
  begin
    DownloadPage.Add('{#VCRedistUrl}', 'vc_redist.x64.exe', '');
    ToDownload := ToDownload + 1;
  end;
  if NeedDotNet and CanDownloadDotNet then
  begin
    DownloadPage.Add('{#DotNetRuntimeUrl}', 'windowsdesktop-runtime-win-x64.exe', '');
    ToDownload := ToDownload + 1;
  end;
  if ToDownload > 0 then
  begin
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
      except
        if DownloadPage.AbortedByUser then
          Log('Download cancelled by the user.')
        else
          SuppressibleMsgBox('Setup could not download the Microsoft components:' + NL + GetExceptionMessage + NL + NL +
            'LegacyAVR will still be installed. The last page of Setup has buttons to download ' +
            'the missing components yourself.', mbError, MB_OK, IDOK);
      end;
    finally
      DownloadPage.Hide;
    end;
  end;

  Steps := 0;
  if NeedVC and FileExists(VCFile) then Steps := Steps + 1;
  if NeedDotNet and FileExists(DotNetFile) then Steps := Steps + 1;
  if Steps = 0 then Exit;

  Step := 0;
  PrereqProgressPage.Show;
  try
    if NeedVC and FileExists(VCFile) then
    begin
      { The embedded copy comes from this setup itself; a downloaded one must carry Microsoft's signature. }
      RunPrerequisite('Microsoft Visual C++ Runtime (x64)', VCFile, Step, Steps, not HasBundledVC);
      Step := Step + 1;
    end;
    if NeedDotNet and FileExists(DotNetFile) then
      RunPrerequisite('.NET 10 Desktop Runtime (x64)', DotNetFile, Step, Steps, True);
  finally
    PrereqProgressPage.Hide;
  end;

  NeedVC := not VCRuntimeInstalled;
  NeedDotNet := not DotNetRuntimeInstalled;
end;

{ ------------------------------------------------------------------ "Next steps" page }

procedure CableButtonClick(Sender: TObject);
begin
  OpenUrl('{#VBCablePage}');
end;

procedure SoundButtonClick(Sender: TObject);
var
  ErrorCode: Integer;
begin
  if not ShellExecAsOriginalUser('', ExpandConstant('{sys}\control.exe'), 'mmsys.cpl', '', SW_SHOWNORMAL, ewNoWait, ErrorCode) then
    MsgBox('Press Win + R, type mmsys.cpl and press Enter to open the Sound control panel.', mbInformation, MB_OK);
end;

procedure VCButtonClick(Sender: TObject);
begin
  OpenUrl('{#VCRedistUrl}');
end;

procedure DotNetButtonClick(Sender: TObject);
begin
  if CanDownloadDotNet then
    OpenUrl('{#DotNetRuntimeUrl}')
  else
    OpenUrl('{#DotNetPage}');
end;

function NewLabel(const Caption: String): TNewStaticText;
begin
  Result := TNewStaticText.Create(NextStepsPage);
  Result.Parent := NextStepsPage.Surface;
  Result.AutoSize := False;
  Result.WordWrap := True;
  Result.Left := 0;
  Result.Width := NextStepsPage.SurfaceWidth;
  Result.Caption := Caption;
end;

function NewButton(const Caption: String; const Width: Integer; const OnClick: TNotifyEvent): TNewButton;
begin
  Result := TNewButton.Create(NextStepsPage);
  Result.Parent := NextStepsPage.Surface;
  Result.Caption := Caption;
  Result.Left := 0;
  Result.Width := Width;
  Result.Height := ScaleY(23);
  Result.OnClick := OnClick;
end;

procedure PlaceLabel(const L: TNewStaticText; var Y: Integer);
begin
  L.Top := Y;
  L.AdjustHeight;
  Y := Y + L.Height + ScaleY(6);
end;

procedure CreateNextStepsPage;
begin
  NextStepsPage := CreateCustomPage(wpInfoAfter, 'One more step: VB-CABLE',
    'LegacyAVR captures your PC''s sound from the free VB-CABLE virtual audio device.');

  MissingLabel := NewLabel('');
  MissingLabel.Font.Color := clRed;
  VCButton := NewButton('Get Visual C++ Runtime', ScaleX(190), @VCButtonClick);
  DotNetButton := NewButton('Get .NET 10 Desktop Runtime', ScaleX(190), @DotNetButtonClick);

  CableLabel := NewLabel('1. Install VB-CABLE (free / donationware from VB-Audio Software) and restart your PC ' +
    'if its installer asks you to. Skip this if VB-CABLE is already installed.');
  CableButton := NewButton('Open the VB-CABLE download page', ScaleX(260), @CableButtonClick);

  SoundLabel := NewLabel('2. In the Sound control panel, configure CABLE Input as 7.1 Surround (or 5.1) and set it ' +
    'as the default playback device. Then start LegacyAVR and choose your HDMI / optical output. ' +
    'The Setup guide tab in LegacyAVR walks you through every step.');
  SoundButton := NewButton('Open the Sound control panel', ScaleX(260), @SoundButtonClick);
end;

procedure LayoutNextStepsPage;
var
  Y: Integer;
  Missing: String;
begin
  Y := 0;
  Missing := '';
  if NeedVC then Missing := Missing + 'Microsoft Visual C++ Runtime (x64)';
  if NeedDotNet then
  begin
    if Missing <> '' then Missing := Missing + ' and ';
    Missing := Missing + '.NET 10 Desktop Runtime (x64)';
  end;

  MissingLabel.Visible := Missing <> '';
  VCButton.Visible := NeedVC;
  DotNetButton.Visible := NeedDotNet;
  if Missing <> '' then
  begin
    MissingLabel.Caption := 'Still missing: ' + Missing + '. LegacyAVR will not start until this is installed:';
    PlaceLabel(MissingLabel, Y);
    VCButton.Top := Y;
    DotNetButton.Top := Y;
    if NeedVC and NeedDotNet then
      DotNetButton.Left := VCButton.Width + ScaleX(8)
    else
      DotNetButton.Left := 0;
    Y := Y + VCButton.Height + ScaleY(14);
  end;

  PlaceLabel(CableLabel, Y);
  CableButton.Top := Y;
  Y := Y + CableButton.Height + ScaleY(14);
  PlaceLabel(SoundLabel, Y);
  SoundButton.Top := Y;
end;

{ ------------------------------------------------------------------ wizard events }

procedure InitializeWizard;
var
  Info: String;
begin
  NeedVC := not VCRuntimeInstalled;
  NeedDotNet := not DotNetRuntimeInstalled;

  Info := 'LegacyAVR needs these free Microsoft components, which are not installed on this PC:' + NL +
          MissingComponentsText + NL;
  if NeedDotNet and not CanDownloadDotNet then
    Info := Info + 'The .NET 10 Desktop Runtime cannot be downloaded automatically by this setup; ' +
            'a download button is shown at the end.' + NL + NL;
  if NeedVC and HasBundledVC and not NeedDotNet then
    Info := Info + 'Setup includes this component and can install it for you (no internet connection needed).'
  else
    Info := Info + 'Setup can install them for you. Anything not included in this setup is downloaded from ' +
            'Microsoft (internet connection needed).';

  PrereqPage := CreateInputOptionPage(wpSelectTasks, 'Required Microsoft components',
    'Setup found components that LegacyAVR needs.', Info, True, False);
  PrereqPage.Add('Install them now (recommended)');
  PrereqPage.Add('Skip - I will install them myself (download buttons are shown at the end of Setup)');
  PrereqPage.SelectedValueIndex := 0;

  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), 'Downloading the Microsoft components LegacyAVR needs...', @OnDownloadProgress);
  PrereqProgressPage := CreateOutputProgressPage('Installing Microsoft components', 'Please wait while Setup installs the components LegacyAVR needs.');
  CreateNextStepsPage;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = PrereqPage.ID then
    Result := not AutoInstallPossible;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = NextStepsPage.ID then
    LayoutNextStepsPage;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine;
  if MemoTasksInfo <> '' then
    Result := Result + NewLine + MemoTasksInfo + NewLine;
  if AutoInstallPossible and (PrereqPage.SelectedValueIndex = 0) then
    Result := Result + NewLine + 'Microsoft components to install:' + NewLine + MissingComponentsText;
  Result := Result + NewLine + 'If LegacyAVR is running, Setup will close it first.';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpReady) and AutoInstallPossible and (PrereqPage.SelectedValueIndex = 0) then
    DownloadAndInstallPrerequisites;
end;

{ Called on the "Preparing to install" step, before any file is copied. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Result := '';
  if CheckForMutexes(UIMutex + ',' + EngineMutex) then
  begin
    if SuppressibleMsgBox('LegacyAVR is running. Setup will close it to continue (sound through LegacyAVR stops ' +
         'until you start it again).', mbConfirmation, MB_OKCANCEL, IDOK) = IDCANCEL then
    begin
      Result := 'LegacyAVR is still running. Close it (tray icon -> Exit) and run Setup again.';
      Exit;
    end;
  end;
  { "net stop" waits until the service has stopped; exit code 0 means it was running. }
  if Exec(ExpandConstant('{sys}\net.exe'), 'stop {#ServiceName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
  begin
    ServiceWasRunning := True;
    Log('Stopped the {#ServiceName} service for the upgrade.');
  end;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM LegacyAVR.UI.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM AVREngine.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(500);
end;

function RestartServiceAfterInstall: Boolean;
begin
  Result := ServiceWasRunning and not WizardIsTaskSelected('service');
end;

function NeedRestart: Boolean;
begin
  Result := RestartNeeded;
end;

{ ------------------------------------------------------------------ uninstall }

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  RunValue: String;
begin
  if CurUninstallStep <> usPostUninstall then Exit;

  { "Start LegacyAVR with Windows" entry of this user, if it points at this installation. }
  if RegQueryStringValue(HKCU, RunKey, RunValueName, RunValue) and
     (Pos(Lowercase(ExpandConstant('{app}')), Lowercase(RunValue)) > 0) then
    RegDeleteValue(HKCU, RunKey, RunValueName);

  if not UninstallSilent then
    if MsgBox('Also delete your LegacyAVR settings, profiles and logs?' + NL + NL +
              'Choose No to keep them for a later reinstall.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    begin
      DelTree(ExpandConstant('{userappdata}\LegacyAVR'), True, True, True);
      DelTree(ExpandConstant('{localappdata}\LegacyAVR'), True, True, True);
      DelTree(ExpandConstant('{commonappdata}\LegacyAVR'), True, True, True);
    end;
end;
