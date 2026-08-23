#define MyAppName "ACECode"
#ifndef MyAppVersion
#define MyAppVersion "0.9.0"
#endif
#define MyAppPublisher "ACECode"
#define MyAppURL "https://github.com/tmoonlight/acecode"
#define MyAppExeName "acecode-desktop.exe"
#define MyCliExeName "acecode.exe"

[Setup]
AppId={{7C3E2A91-5B64-4F0E-9C2A-ACEC0DE00001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={localappdata}\Programs\ACECode
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=lowest
OutputDir=output
OutputBaseFilename=ACECode-{#MyAppVersion}-windows-x64-setup
SetupIconFile=..\..\assets\windows\acecode.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern dynamic
WizardSizePercent=120
WizardImageFile=images\wizard-side.bmp,images\wizard-side@2x.bmp
WizardSmallImageFile=images\wizard-small.bmp,images\wizard-small@2x.bmp
WizardImageStretch=yes
WizardImageBackColor=$301608
WizardImageBackColorDynamicDark=$301608
WizardBackColor=$F7F4F0
WizardBackColorDynamicDark=$301608
SetupLogging=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
CloseApplications=yes
RestartApplications=no
UsedUserAreasWarning=no
ShowLanguageDialog=no

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "addtopath"; Description: "将 ACECode 命令行加入用户 PATH"; GroupDescription: "附加选项:"

[Files]
Source: "staging\acecode.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "staging\acecode-desktop.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "staging\README.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "staging\README_CN.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "staging\share\*"; DestDir: "{app}\share"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "seed_acemodel.ps1"; DestDir: "{app}\installer"; Flags: ignoreversion
Source: "..\..\assets\windows\acecode.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\ACECode"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\acecode.ico"
Name: "{group}\ACECode 终端"; Filename: "{app}\{#MyCliExeName}"; IconFilename: "{app}\acecode.ico"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\ACECode"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\acecode.ico"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}'))

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 ACECode"; Flags: nowait postinstall skipifsilent unchecked

[UninstallDelete]
Type: filesandordirs; Name: "{app}\installer"

[Code]
var
  AceModelPage: TWizardPage;
  AceModelIntro: TNewStaticText;
  AceModelHint: TNewStaticText;
  AceModelEdit: TPasswordEdit;
  AceModelSkip: TNewCheckBox;
  SeedError: String;

procedure AceModelSkipClick(Sender: TObject); forward;

function NeedsAddPath(Param: string): Boolean;
var
  OrigPath: String;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath) then
  begin
    Result := True;
    Exit;
  end;
  Result := Pos(';' + UpperCase(Param) + ';', ';' + UpperCase(OrigPath) + ';') = 0;
end;

procedure InitializeWizard;
begin
  AceModelPage := CreateCustomPage(wpInstalling,
    '配置 ACEModel',
    '可选：填写自营模型密钥，安装完成后即可直接使用。');

  AceModelIntro := TNewStaticText.Create(AceModelPage);
  AceModelIntro.Parent := AceModelPage.Surface;
  AceModelIntro.AutoSize := True;
  AceModelIntro.WordWrap := True;
  AceModelIntro.Width := AceModelPage.SurfaceWidth;
  AceModelIntro.Caption :=
    '如果你已经有 ACEModel API Key，填写后安装程序会把 moonlight、starrylight 和 aurora 写入当前用户的 ACECode 配置。' + #13#10 +
    '也可以跳过这一步，稍后在 ACECode「新增模型」里自行添加。';

  AceModelEdit := TPasswordEdit.Create(AceModelPage);
  AceModelEdit.Parent := AceModelPage.Surface;
  AceModelEdit.Top := AceModelIntro.Top + AceModelIntro.Height + ScaleY(16);
  AceModelEdit.Left := 0;
  AceModelEdit.Width := AceModelPage.SurfaceWidth;
  AceModelEdit.Height := ScaleY(23);

  AceModelHint := TNewStaticText.Create(AceModelPage);
  AceModelHint.Parent := AceModelPage.Surface;
  AceModelHint.AutoSize := True;
  AceModelHint.WordWrap := True;
  AceModelHint.Top := AceModelEdit.Top + AceModelEdit.Height + ScaleY(10);
  AceModelHint.Width := AceModelPage.SurfaceWidth;
  AceModelHint.Caption :=
    '密钥仅写入当前用户的 ACECode 配置，不会上传。' + #13#10 +
    '已存在的同名预设会被更新为同一把密钥。';

  AceModelSkip := TNewCheckBox.Create(AceModelPage);
  AceModelSkip.Parent := AceModelPage.Surface;
  AceModelSkip.Top := AceModelHint.Top + AceModelHint.Height + ScaleY(16);
  AceModelSkip.Left := 0;
  AceModelSkip.Width := AceModelPage.SurfaceWidth;
  AceModelSkip.Height := ScaleY(24);
  AceModelSkip.Caption := '跳过，稍后再配置 ACEModel';
  AceModelSkip.Checked := False;
  AceModelSkip.OnClick := @AceModelSkipClick;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = AceModelPage.ID then
  begin
    WizardForm.NextButton.Caption := '完成配置';
    if AceModelSkip.Checked then
      WizardForm.NextButton.Caption := '跳过并完成';
  end;
end;

procedure AceModelSkipClick(Sender: TObject);
begin
  AceModelEdit.Enabled := not AceModelSkip.Checked;
  if AceModelSkip.Checked then
    WizardForm.NextButton.Caption := '跳过并完成'
  else
    WizardForm.NextButton.Caption := '完成配置';
end;

function ShouldSeedAceModel: Boolean;
begin
  Result := (not AceModelSkip.Checked) and (Trim(AceModelEdit.Text) <> '');
end;

function SeedAceModelProfiles: Boolean;
var
  KeyFile: String;
  ScriptFile: String;
  ConfigPath: String;
  ResultCode: Integer;
  Params: String;
begin
  Result := True;
  SeedError := '';
  if not ShouldSeedAceModel then
    Exit;

  KeyFile := ExpandConstant('{tmp}\acemodel.key');
  ScriptFile := ExpandConstant('{app}\installer\seed_acemodel.ps1');
  ConfigPath := ExpandConstant('{%USERPROFILE}\.acecode\config.json');
  if not SaveStringToFile(KeyFile, Trim(AceModelEdit.Text), False) then
  begin
    SeedError := '无法写入临时密钥文件。';
    Result := False;
    Exit;
  end;

  Params :=
    '-NoProfile -ExecutionPolicy Bypass -File "' + ScriptFile +
    '" -KeyFile "' + KeyFile +
    '" -ConfigPath "' + ConfigPath + '"';
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
       Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    SeedError := '无法启动 PowerShell 写入 ACEModel 配置。';
    Result := False;
  end
  else if ResultCode <> 0 then
  begin
    SeedError := '写入 ACEModel 配置失败（退出码 ' + IntToStr(ResultCode) + '）。你可以稍后在 ACECode 中手动添加模型。';
    Result := False;
  end;

  DeleteFile(KeyFile);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = AceModelPage.ID then
  begin
    if (not AceModelSkip.Checked) and (Trim(AceModelEdit.Text) = '') then
    begin
      if MsgBox('还没有填写 ACEModel Key。要跳过这一步吗？', mbConfirmation, MB_YESNO) = IDNO then
      begin
        Result := False;
        Exit;
      end;
      AceModelSkip.Checked := True;
    end;
    if not SeedAceModelProfiles then
    begin
      MsgBox(SeedError, mbError, MB_OK);
      Result := False;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssDone then
    DeleteFile(ExpandConstant('{tmp}\acemodel.key'));
end;
