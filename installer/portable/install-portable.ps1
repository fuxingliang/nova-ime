# ============================================================
#  Nova 输入法（便携版）一键安装
#
#  用法：右键此文件 → 使用 PowerShell 运行（自动请求管理员权限）
#  说明：
#    · 本脚本把"便携版文件夹"注册为系统 TSF 输入法，等价于
#      安装包的注册/激活/自启动/启动服务步骤，但文件保留在你
#      解压的目录，不写入 %LocalAppData%。
#    · 用户数据（词库/配置）在 %AppData%\NovaInput（与安装包一致）。
#    · TSF 注册表只认 HKLM（与搜狗/微软拼音一致），故需一次 UAC。
#    · 绝不杀 explorer/浏览器/ctfmon 等外部进程（只停自己的
#      Server/Engine），避免 TSF 重载导致宿主卡死。
# ============================================================

# --- 0. 自动请求管理员权限 + 保证 64 位执行 ------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$is64bit = [Environment]::Is64BitProcess

if (-not $isAdmin -or -not $is64bit) {
    $argStr = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    $native = Join-Path $env:WINDIR 'SysNative\WindowsPowerShell\v1.0\powershell.exe'
    if (-not $is64bit -and (Test-Path $native)) {
        if ($isAdmin) { Start-Process $native -ArgumentList $argStr -WindowStyle Hidden; exit }
        Start-Process $native -Verb RunAs -ArgumentList $argStr
        exit
    }
    $exe = if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh' } else { 'powershell' }
    Start-Process $exe -Verb RunAs -ArgumentList $argStr
    exit
}

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$clsid   = '{D2291A80-84D8-4641-9AB2-BDD1472C846B}'
$profile = '{83955C0E-2C09-47A5-BCF3-F2B98E11EE8B}'
$langid  = '0x00000804'   # 中文(简体)

Write-Host ''
Write-Host '========== Nova 输入法（便携版）安装 ==========' -ForegroundColor Cyan
Write-Host ("便携目录: " + $root) -ForegroundColor DarkGray

# --- 1. 完整性检查 --------------------------------------------------------
Write-Host '[1/5] 检查便携版文件完整性...' -ForegroundColor Cyan
$required = @(
    'SampleIME.dll',
    'PinyinPlus.Engine.exe',
    'ImeActivate.exe',
    'server\PinyinPlus.Server.exe',
    'pinyin-plus.txt'
)
$missing = @()
foreach ($f in $required) {
    if (-not (Test-Path (Join-Path $root $f))) { $missing += $f }
}
if ($missing.Count -gt 0) {
    Write-Host "错误：便携版文件不完整，缺少：" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    Write-Host '请重新解压完整包（保持文件夹结构）。' -ForegroundColor Yellow
    Read-Host "`n按回车键关闭窗口"
    exit 1
}
if (-not (Test-Path (Join-Path $root 'pinyin-plus-big.txt'))) {
    Write-Host '      警告：未找到大字库 pinyin-plus-big.txt（设置面板"大字库模式"将不可用）。' -ForegroundColor Yellow
}
Write-Host '      文件完整。' -ForegroundColor DarkGray

# --- 2. 停止自己的进程（绝不碰外部进程）----------------------------------
Write-Host '[2/5] 停止旧的 Nova 输入法进程...' -ForegroundColor Cyan
taskkill /f /im PinyinPlus.Server.exe /t 2>$null | Out-Null
taskkill /f /im PinyinPlus.Engine.exe /t 2>$null | Out-Null
Start-Sleep -Milliseconds 500
Write-Host '      已停止。' -ForegroundColor DarkGray

# --- 3. 注册 TSF（regsvr32：HKLM 的 CLSID + Profile + Category）----------
# 与开发版 tools\register.ps1 一致：直接注册覆盖 InprocServer32 路径，
# 不先反注册（避免 CLSID 空窗期触发 Chromium/Electron 宿主异常）。
Write-Host '[3/5] 注册 SampleIME.dll 为系统 TSF 输入法（HKLM）...' -ForegroundColor Cyan
$regsvr32 = Join-Path $env:WINDIR 'System32\regsvr32.exe'
$out = & $regsvr32 /s (Join-Path $root 'SampleIME.dll') 2>&1
$code = $LASTEXITCODE
if ($null -eq $code -or $code -ne 0) {
    Write-Host $out -ForegroundColor Yellow
    throw "regsvr32 失败（退出码 $code），请检查上方输出或确认 DLL 为 x64 版本。"
}
Start-Sleep -Milliseconds 500
# HKLM Profile 启用位（Enable=1，与安装包一致）
$lhmProfKey = "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$clsid\LanguageProfile\$langid\$profile"
if (Test-Path $lhmProfKey) {
    Set-ItemProperty $lhmProfKey -Name 'Enable' -Value 1 -Type DWord
}
Write-Host '      注册完成。' -ForegroundColor DarkGray

# --- 4. 降权回原用户执行（等价安装包 runasoriginaluser）--------------------
# 关键（2026-08-18 根因）：Server 若以提权（High 完整性）启动，其看门狗会
# 提权拉起引擎 → 引擎命名管道为 High 完整性 → 普通应用里的 DLL 连不上 →
# 只能打英文。必须降权回普通用户（Medium 完整性）启动。
# runas /trustlevel:0x00000000 = 同用户、默认（Medium）完整性。
Write-Host '[4/5] 激活输入法 + 设为默认 + 开机自启动 + 启动候选窗服务...' -ForegroundColor Cyan
$serverExe = Join-Path $root 'server\PinyinPlus.Server.exe'
$userCmd = @"
`$ErrorActionPreference = 'Continue'
`$profKey = "HKCU:\SOFTWARE\Microsoft\CTF\TIP\$clsid\LanguageProfile\$langid\$profile"
if (-not (Test-Path `$profKey)) { New-Item -Path `$profKey -Force | Out-Null }
Set-ItemProperty -Path `$profKey -Name 'Enable' -Value 1 -Type DWord -ErrorAction SilentlyContinue
& "$root\ImeActivate.exe" | Out-Null
New-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'NovaInput.Server' -Value "`"`"$serverExe`"`"" -PropertyType String -Force | Out-Null
Start-Process -FilePath "$serverExe"
"@
$tmp = Join-Path $env:TEMP 'novaport_install_user.ps1'
Set-Content -Path $tmp -Value $userCmd -Encoding UTF8
& runas /trustlevel:0x00000000 "powershell -NoProfile -ExecutionPolicy Bypass -File `"$tmp`"" 2>$null | Out-Null
Start-Sleep -Seconds 2
Remove-Item $tmp -Force -ErrorAction SilentlyContinue
Write-Host '      完成。' -ForegroundColor DarkGray

# --- 5. 验证 ----------------------------------------------------------------
Write-Host '[5/5] 验证安装结果...' -ForegroundColor Cyan
$inproc = (Get-ItemProperty "HKCR:\CLSID\$clsid\InprocServer32" -ErrorAction SilentlyContinue).'(default)'
$tipLm  = Test-Path "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
$tipCu  = Test-Path "HKCU:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
$runVal = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -ErrorAction SilentlyContinue).'NovaInput.Server'
Start-Sleep -Seconds 1
$svr = Get-Process PinyinPlus.Server -ErrorAction SilentlyContinue

Write-Host ''
Write-Host '  TSF CLSID (HKCR): ' -ForegroundColor Gray
Write-Host ("      " + $inproc) -ForegroundColor Gray
Write-Host "  TSF Profile: HKLM=$tipLm  HKCU=$tipCu" -ForegroundColor Gray
Write-Host "  开机自启动   : $([bool]$runVal)" -ForegroundColor Gray
Write-Host "  候选窗服务   : " + $(if ($svr) { "运行中 (PID $($svr.Id))" } else { '未检测到（DLL 保活线程会自动拉起引擎，服务缺失时托盘双击可手动启动）' }) -ForegroundColor Gray

Write-Host ''
if ($inproc -and ($tipLm -or $tipCu)) {
    Write-Host '✅ 便携版安装成功！' -ForegroundColor Green
    Write-Host ''
    Write-Host '接下来：' -ForegroundColor Cyan
    Write-Host '  1. 任意输入框按  Win + Space  切换到 [Nova 输入法] 即可打字' -ForegroundColor Gray
    Write-Host '  2. 若输入法列表没有它：设置 → 时间和语言 → 语言和区域 → 中文 → 键盘 → 添加键盘 → [Nova 输入法]' -ForegroundColor Gray
    Write-Host '  3. 托盘出现 Nova 图标 = 候选窗服务运行中（双击打开设置面板）' -ForegroundColor Gray
    Write-Host '  4. 不再使用时：右键 uninstall-portable.ps1 → 使用 PowerShell 运行，然后删除本文件夹' -ForegroundColor Gray
    Write-Host '     （用户词库/配置在 %AppData%\NovaInput，卸载不丢失）' -ForegroundColor Gray
} else {
    Write-Host '⚠ 注册不完整，请检查上方输出。' -ForegroundColor Yellow
}
Write-Host ''
Read-Host '按回车键关闭窗口'
