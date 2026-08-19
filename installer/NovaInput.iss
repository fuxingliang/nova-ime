; ============================================================
;  Nova 输入法 - 安装脚本（Inno Setup 6）
;
;  设计原则（专业输入法安装体验，装完即用）：
;    · 注册表   = TSF Profile/Category/COM 类写 HKLM（TSF 只认 HKLM，
;                与搜狗/微软拼音一致）→ 安装需一次 UAC
;    · 启用状态 = HKCU（每用户） + ImeActivate.exe 激活进 Win+Space
;    · 安装目录 = {localappdata}\NovaInput（用户可写，升级/重装无权限问题）
;    · 用户数据 = %AppData%\NovaInput（engine.conf / userdict.txt / config.json）
;                卸载时保留，用户词库与配置天然不丢
;    · 装完自动：激活输入法 + 设为默认 + 启动候选窗服务
;    · 注意：绝不杀 ctfmon.exe！TSF 枚举/切换列表对新进程即时生效，
;      强杀系统输入法服务会让 explorer/Trae 等持有 TSF 的进程重载卡死
;      （2026-08-15 安装事故根因，已移除该步骤）
;
;  用 ISCC.exe 编译（构建入口见 build_installer.ps1）
; ============================================================

#ifndef MyAppVersion
  #define MyAppVersion "1.0.2"
#endif

#define MyAppName "Nova 输入法"
#define MyAppExeName "PinyinPlus.Server.exe"
#define MyAppId "{{B3C1E2A0-5F4D-4E6B-9A8C-2D1F3E5A7B9C}"

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Nova Input
DefaultDirName={localappdata}\NovaInput
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=NovaInput-Setup-{#MyAppVersion}
SetupIconFile=..\src\server\Assets\Nova.ico
UninstallDisplayIcon={app}\SampleIME.dll
UninstallDisplayName={#MyAppName}
; TSF Profile 注册必须在 HKLM（TSF 枚举只读 HKLM），安装需一次 UAC；
; 安装目录仍放 {localappdata}（每用户可写，升级无权限问题）
PrivilegesRequired=admin
; x64 输入法：安装器以 64 位运行，保证注册表视图一致（Inno 6.3+）
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
; 压缩（词库 60MB+，用最优压缩；LZMA2 对文本词库压缩率很高）
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; 升级时覆盖旧版本（AppId 相同即走升级路径，用户数据在 %AppData% 不受影响）
AppVerName={#MyAppName} {#MyAppVersion}
; ★★★ 绝不自动关闭/重启占用文件的程序（2026-08-15 黑屏事故根因修复）★★★
; Inno 默认 CloseApplications=yes：安装前检测"正在使用安装文件的程序"并强杀它们。
; 输入法 DLL 被注入到 explorer/浏览器等进程，被强杀后重载 TSF 输入法栈 → 黑屏无响应。
; 改为 no：任何外部进程都不碰；DLL 被占用时走"延迟替换 + 重启生效"（见 [Code] InstallImeDll）。
CloseApplications=no
RestartApplications=no

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Default.isl"

[Files]
; ---- 输入法壳（TSF DLL，固定文件名）----
; dontcopy：不直接解压安装。由 [Code] InstallImeDll 在 ssInstall 阶段处理——
;   DLL 未被占用 → 直接复制（装完即用）；
;   DLL 被 explorer/浏览器等占用 → 复制为 .new + 延迟替换（重启后生效，绝不杀进程）。
Source: "..\src\ime\x64\Release\SampleIME.dll"; Flags: dontcopy
; ---- 拼音引擎 ----
Source: "..\src\engine\x64\Release\PinyinPlus.Engine.exe"; DestDir: "{app}"; Flags: ignoreversion
; ---- 词库 ----
Source: "..\bin\pinyin-plus.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\bin\pinyin-plus-big.txt"; DestDir: "{app}"; Flags: ignoreversion
; ---- 预生成词库缓存（安装后引擎首次启动直接命中，无需解析 29MB 文本 + 聚合字频排序）----
Source: "..\bin\pinyin-plus-big.txt.bin"; DestDir: "{app}"; Flags: ignoreversion
; ---- 简繁转换表（OpenCC，Apache-2.0）----
Source: "..\tools\data\STCharacters.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\tools\data\STPhrases.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\tools\data\TSCharacters.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\tools\data\TSPhrases.txt"; DestDir: "{app}"; Flags: ignoreversion
; ---- 符号面板数据 ----
Source: "..\tools\data\symbols.txt"; DestDir: "{app}"; Flags: ignoreversion
; ---- 激活工具（EnableLanguageProfile + 设为默认输入法）----
Source: "..\src\activate\x64\Release\ImeActivate.exe"; DestDir: "{app}"; Flags: ignoreversion
; ---- 候选窗服务器（self-contained 发布，免装 .NET 运行时）----
Source: "..\dist\server\*"; DestDir: "{app}\server"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
// ============================================================
// TSF 注册（Profile/Category/COM 类写 HKLM，启用状态写 HKCU）。
// 用 Pascal 脚本写注册表：GUID 含花括号，与 Inno 的 {常量} 语法
// 冲突，[Registry] 段无法表达，故在此写入/卸载。
// 结构与 regsvr32 注册结果一致（ITfInputProcessorProfileMgr /
// ITfCategoryMgr 实际落盘位置），TSF 枚举只读 HKLM——
// 这也是输入法"装完即用"的关键（搜狗/微软拼音同款）。
// ============================================================
const
  CLSID   = '{D2291A80-84D8-4641-9AB2-BDD1472C846B}';
  PROFILE = '{83955C0E-2C09-47A5-BCF3-F2B98E11EE8B}';
  LANGID  = '0x00000804';            // TEXTSERVICE_LANGID：中文(简体)
  ICONIDX = $FFFFFFF4;               // TEXTSERVICE_ICON_INDEX = -12

  // TSF 支持类别 GUID（ITfCategoryMgr 注册的 8 个标准 Category）
  CAT_KEYBOARD         = '{49D2F9CE-1F5E-11D7-A6D3-00065B84435C}';
  CAT_DISPLAYATTR      = '{046B8C80-1647-40F7-9B21-B93B81AABC1B}';
  CAT_UIELEMENTENABLED = '{13A016DF-560B-46CD-947A-4C3AF1E0E35D}';
  CAT_SECUREMODE       = '{25504FB4-7BAB-4BC1-9C69-CF81890F0EF5}';
  CAT_COMLESS          = '{CCF05DD7-4A87-11D7-A6E2-00065B84435C}';
  CAT_INPUTMODECOMPART = '{34745C63-B2F0-4784-8B67-5E12C8701A31}';
  CAT_IMMERSIVESUPPORT = '{49D2F9CF-1F5E-11D7-A6D3-00065B84435C}';
  CAT_SYSTRAYSUPPORT   = '{364215D9-75BC-11D7-A6EF-00065B84435C}';

  // ---- 版本化 DLL（搜狗专利 CN101510157A 同款）用到的 Win32 常量 ----
  // 注：FILE_ATTRIBUTE_NORMAL/SW_HIDE 等 Inno 已内置，勿重复声明
  GENERIC_WRITE            = $40000000;
  CREATE_ALWAYS            = 2;
  INVALID_HANDLE_VALUE     = -1;

function CreateFileW(lpFileName: string; dwDesiredAccess: DWORD; dwShareMode: DWORD;
  lpSecurityAttributes: Integer; dwCreationDisposition: DWORD;
  dwFlagsAndAttributes: DWORD; hTemplateFile: Integer): LongWord;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseHandle(hObject: LongWord): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';

var
  // 实际安装的 DLL 文件名。首次安装/未被占用 = SampleIME.dll；
  // 升级时旧 DLL 被 explorer/浏览器占用 → 版本化文件名（搜狗专利 CN101510157A 同款）：
  //   新 DLL 独立文件 + 注册表指向新版 → 新进程装完即用新版，无需重启；
  //   旧进程继续用已加载的旧 DLL，新旧共存互不干扰。
  g_ImeDllName: string;

// 只停我们自己的进程（Server/引擎）。绝不碰 explorer/浏览器/ctfmon 等外部进程！
procedure KillOurProcesses;
var
  ResultCode: Integer;
begin
  Exec('taskkill.exe', '/f /im PinyinPlus.Server.exe /t', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec('taskkill.exe', '/f /im PinyinPlus.Engine.exe /t', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

// ---- 安装抑制标志（2026-08-18 修复"文件写保护"中止）----
// 杀引擎后，注入在 explorer 等宿主里的 DLL 保活线程会立刻重新拉起引擎，
// 新引擎随即加载词库 → 锁定词库文件 → [Files] 复制词库时报"文件写保护"而中止。
// 这里先立 flag，DLL 看到 flag 即暂停拉起引擎；DeinitializeSetup 里无条件清除。
// 数据目录 %AppData%\NovaInput 与 DLL 的 GetDataPath 一致。
procedure SetInstallFlag;
begin
  SaveStringToFile(ExpandConstant('{userappdata}\NovaInput\installing.flag'), '1', False);
end;

procedure ClearInstallFlag;
begin
  DeleteFile(ExpandConstant('{userappdata}\NovaInput\installing.flag'));
end;

// 安装文件复制前准备：提前终止我们自己的进程（Server/引擎）。
// ssInstall 阶段的 KillOurProcesses 时机偏晚（文件复制期间才杀），若进程正在运行，
// 其 exe/词库/bin 仍被占用 → [Files] 覆盖时报"文件写保护，无法写入"。
// PrepareToInstall 在复制文件之前调用（官方推荐），杀完再短暂等待句柄释放，杜绝写保护。
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  SetInstallFlag;   // ★ 先立 flag 再杀进程，DLL 保活线程不会再拉起引擎
  KillOurProcesses;
  Sleep(400);
end;

// 无论安装成功/失败/中止，Setup 退出前都调用：清除抑制 flag，
// 避免残留 flag 永久抑制引擎拉起（输入法失联）。
procedure DeinitializeSetup();
begin
  ClearInstallFlag;
end;

// 安装 TSF DLL（[Files] 已标记 dontcopy，由这里负责落盘）：
//   1. 目标未被占用（首次安装/无进程加载）→ 直接覆盖 SampleIME.dll，装完即用；
//   2. 目标被 explorer/浏览器等占用（升级场景）→ 复制为 SampleIME_v<版本>.dll，
//      注册表（RegisterIme）指向新版文件 → 新进程立即用新版（无需重启），
//      旧进程继续用已加载的旧 DLL。绝不终止外部进程——"安装黑屏"事故的根治。
procedure InstallImeDll;
var
  TmpDll, DestDll: string;
  hFile: LongWord;
begin
  ExtractTemporaryFile('SampleIME.dll');
  TmpDll := ExpandConstant('{tmp}\SampleIME.dll');
  DestDll := ExpandConstant('{app}\SampleIME.dll');
  g_ImeDllName := 'SampleIME.dll';

  // 独占创建目标：成功 = 无进程占用，可直接覆盖
  hFile := CreateFileW(DestDll, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if hFile <> INVALID_HANDLE_VALUE then
  begin
    CloseHandle(hFile);
    if not CopyFile(TmpDll, DestDll, False) then
      Log('InstallImeDll: CopyFile failed (not locked)');
  end
  else
  begin
    // 被占用 → 版本化文件名（新旧 DLL 共存，注册表指向新版，无需重启）
    g_ImeDllName := 'SampleIME_v{#MyAppVersion}.dll';
    if not CopyFile(TmpDll, ExpandConstant('{app}\' + g_ImeDllName), False) then
      Log('InstallImeDll: versioned copy failed');
  end;
end;

// 删除 {app}\SampleIME_v*.dll 中非当前版本的文件（升级多次不堆积残留）
procedure CleanupOldVersionedDlls;
var
  FindRec: TFindRec;
  CurDll: string;
begin
  CurDll := 'SampleIME_v{#MyAppVersion}.dll';
  if FindFirst(ExpandConstant('{app}\SampleIME_v*.dll'), FindRec) then
  begin
    try
      repeat
        if FindRec.Name <> CurDll then
          DeleteFile(ExpandConstant('{app}\') + FindRec.Name);
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

procedure RegisterIme;
var
  I: Integer;
  ClsRoot: string;
  TipRoot: string;
  ProfKey: string;
  CatBase: string;
  Cats: array[0..7] of string;
begin
  Cats[0] := CAT_KEYBOARD;
  Cats[1] := CAT_DISPLAYATTR;
  Cats[2] := CAT_UIELEMENTENABLED;
  Cats[3] := CAT_SECUREMODE;
  Cats[4] := CAT_COMLESS;
  Cats[5] := CAT_INPUTMODECOMPART;
  Cats[6] := CAT_IMMERSIVESUPPORT;
  Cats[7] := CAT_SYSTRAYSUPPORT;
  // COM 类（HKLM：机器级，TSF/COM 全局可见）。
  // InprocServer32 指向 g_ImeDllName（升级被占用时 = SampleIME_v<版本>.dll，
  // 搜狗专利 CN101510157A 同款：新进程装完即用新版，无需重启）。
  ClsRoot := 'SOFTWARE\Classes\CLSID\' + CLSID;
  RegWriteStringValue(HKLM, ClsRoot, '', '{#MyAppName}');
  RegWriteStringValue(HKLM, ClsRoot + '\InprocServer32', '',
    ExpandConstant('{app}\' + g_ImeDllName));
  RegWriteStringValue(HKLM, ClsRoot + '\InprocServer32', 'ThreadingModel',
    'Apartment');

  // TSF Profile（HKLM：TSF 输入法枚举只读 HKLM，注册到 HKLM 系统才识别）
  TipRoot := 'SOFTWARE\Microsoft\CTF\TIP\' + CLSID;
  ProfKey := TipRoot + '\LanguageProfile\' + LANGID + '\' + PROFILE;
  RegWriteStringValue(HKLM, ProfKey, 'Description', '{#MyAppName}');
  RegWriteStringValue(HKLM, ProfKey, 'IconFile',
    ExpandConstant('{app}\' + g_ImeDllName));
  RegWriteDWordValue(HKLM, ProfKey, 'IconIndex', ICONIDX);
  RegWriteDWordValue(HKLM, ProfKey, 'Enable', 1);

  // TSF Category（HKLM 正反双索引，与 ITfCategoryMgr 结构一致）
  CatBase := TipRoot + '\Category\Category';
  for I := 0 to 7 do
    RegWriteStringValue(HKLM, CatBase + '\' + CATS[I] + '\' + CLSID, '', '');
  CatBase := TipRoot + '\Category\Item';
  for I := 0 to 7 do
    RegWriteStringValue(HKLM, CatBase + '\' + CLSID + '\' + CATS[I], '', '');

  // 每用户启用状态（HKCU，EnableLanguageProfile 亦写此处）
  RegWriteDWordValue(HKCU, ProfKey, 'Enable', 1);

  // 开机自启动（HKCU，每用户）：登录后拉起候选窗服务器（Server 有引擎保活线程）
  RegWriteStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run',
    'NovaInput.Server',
    '"' + ExpandConstant('{app}\server\PinyinPlus.Server.exe') + '"');
end;

procedure UnregisterIme;
var
  RunKey: string;
begin
  RegDeleteKeyIncludingSubkeys(HKLM, 'SOFTWARE\Classes\CLSID\' + CLSID);
  RegDeleteKeyIncludingSubkeys(HKCU, 'SOFTWARE\Classes\CLSID\' + CLSID);
  RegDeleteKeyIncludingSubkeys(HKLM, 'SOFTWARE\Microsoft\CTF\TIP\' + CLSID);
  RegDeleteKeyIncludingSubkeys(HKCU, 'SOFTWARE\Microsoft\CTF\TIP\' + CLSID);
  RunKey := 'Software\Microsoft\Windows\CurrentVersion\Run';
  if RegValueExists(HKCU, RunKey, 'NovaInput.Server') then
    RegDeleteValue(HKCU, RunKey, 'NovaInput.Server');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    // 先停自己的进程（Server/引擎）：它们会锁住 {app} 里的文件，且被杀无任何副作用。
    // 绝不终止 explorer/浏览器/ctfmon 等外部进程（黑屏事故教训）。
    KillOurProcesses;
    // TSF DLL 特殊安装：占用 → 版本化文件名 + 注册表指向新版（装完即用，无需重启）
    InstallImeDll;
    // 清理历史版本化 DLL（仅保留本次版本；占用删不掉则忽略）
    CleanupOldVersionedDlls;
  end;
  if CurStep = ssPostInstall then
    RegisterIme;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    UnregisterIme;
    // 清理升级时可能产生的版本化 DLL（占用删不掉则忽略：Inno 删目录时残留，可手动删）
    DeleteFile(ExpandConstant('{app}\SampleIME_v{#MyAppVersion}.dll'));
  end;
end;

[Run]
; 1. 激活输入法（进 Win+Space 列表）+ 设为默认输入法
;    runasoriginaluser：降权回普通用户执行（激活走 HKCU，无需管理员）
Filename: "{app}\ImeActivate.exe"; Flags: nowait runhidden skipifsilent runasoriginaluser
; 2. 启动候选窗服务（可选勾选）
;    注意：此处绝不执行 taskkill ctfmon / 杀 explorer——2026-08-15 事故证明
;    强杀系统输入法服务会让持有 TSF 的进程（explorer/IDE）重载卡死。
;    新注册的输入法对新打开的窗口/进程即时生效，无需重启系统服务。
;    runasoriginaluser：★关键★ 若以提权启动 Server，其看门狗会提权拉起引擎，
;    引擎创建的命名管道为 High 完整性，普通应用里的 DLL 连不上 → 只能打英文
;    （2026-08-18 根因）。必须降权回普通用户，管道才保持 Medium 完整性。
Filename: "{app}\server\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent runasoriginaluser

; ============================================================
; 卸载：删除安装目录（Inno 默认）+ 上面已标记的 HKCU 注册表。
; %AppData%\NovaInput（用户词库/配置）刻意不删，保留数据。
; ============================================================
