# ============================================================
#  Pinyin-Plus 输入法注册脚本
#  功能：注册 DLL 为系统 TSF 输入法（COM + Profile + Category）
#  用法：
#     register.ps1                          # 注册 bin\PinyinPlusIme.dll
#     register.ps1 -Dll bin\PinyinPlusIme-20260812-0900.dll   # 注册指定版本
#  说明：脚本会自动请求管理员权限并保证以 64 位执行
# ============================================================

param(
    [string]$Dll = ""
)

# --- 0. 自动请求管理员权限（UAC）---------------------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$is64bit = [Environment]::Is64BitProcess

if (-not $isAdmin -or -not $is64bit) {
    $argStr = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ($Dll) { $argStr += " -Dll `"$Dll`"" }

    if (-not $is64bit) {
        $native = Join-Path $env:WINDIR 'SysNative\WindowsPowerShell\v1.0\powershell.exe'
        if (Test-Path $native) {
            if ($isAdmin) { Start-Process $native -ArgumentList $argStr -WindowStyle Hidden; exit }
            Start-Process $native -Verb RunAs -ArgumentList $argStr
            exit
        }
    }

    $exe = if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh' } else { 'powershell' }
    Start-Process $exe -Verb RunAs -ArgumentList $argStr
    exit
}

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'

# DLL 路径：指定则用之，否则用默认 PinyinPlusIme.dll；若默认不存在则用最新版本文件
if ($Dll) {
    $dll = if ([System.IO.Path]::IsPathRooted($Dll)) { $Dll } else { Join-Path $root $Dll }
} else {
    $dll = Join-Path $bin 'PinyinPlusIme.dll'
    if (-not (Test-Path $dll)) {
        $latest = Get-ChildItem $bin -Filter 'PinyinPlusIme-*.dll' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($latest) { $dll = $latest.FullName }
    }
}

# --- 0. 前置检查 ------------------------------------------------------------
if (-not (Test-Path $dll)) {
    throw "未找到 $dll ，请先执行构建命令：`nMSBuild src\ime\SampleIME.vcxproj /p:Configuration=Release /p:Platform=x64"
}

$regsvr32 = Join-Path $env:WINDIR 'System32\regsvr32.exe'

$clsid  = '{D2291A80-84D8-4641-9AB2-BDD1472C846B}'

Write-Host ("环境: 注册进程为 " + $(if ([Environment]::Is64BitProcess) { '64位' } else { '32位' }) + " | regsvr32: $regsvr32") -ForegroundColor DarkGray

# --- 1. 注册 COM + TSF Profile + Category（官方 API）-------------------------
# 注意：不再先 regsvr32 /u 卸载旧注册。卸载-注册之间存在"CLSID 空窗期"，
# Chromium/Electron(如 Trae)对 TSF 输入法注册表变化敏感，空窗期内新建输入法
# 实例会 CoCreateInstance 失败，可能触发宿主退出。regsvr32 /s 直接注册会
# 覆盖 InprocServer32 路径，等效"无缝切换"。
Write-Host "[1/2] 注册 PinyinPlusIme.dll ..." -ForegroundColor Cyan
$out = & $regsvr32 /s $dll 2>&1
$code = $LASTEXITCODE

if ($null -eq $code) {
    Write-Host $out -ForegroundColor Yellow
    throw "regsvr32 未正常执行（无退出码），可能是 32 位环境或路径问题。上方为 regsvr32 输出。"
}
if ($code -ne 0) {
    Write-Host $out -ForegroundColor Yellow
    throw "regsvr32 失败（退出码 0x$('{0:X}' -f $code)），请检查上方 regsvr32 输出或 DLL 是否为 x64 版本"
}

Start-Sleep -Milliseconds 500

# --- 2. 验证注册结果 ---------------------------------------------------------
Write-Host "[2/2] 验证注册结果..." -ForegroundColor Cyan
$hkcr = Get-ItemProperty "Registry::HKEY_CLASSES_ROOT\CLSID\$clsid\InprocServer32" -ErrorAction SilentlyContinue
$tipHkcu = Test-Path "Registry::HKCU\Software\Microsoft\CTF\TIP\$clsid"
$tipHklm = Test-Path "Registry::HKLM\SOFTWARE\Microsoft\CTF\TIP\$clsid"

Write-Host "   InprocServer32: $($hkcr.'(default)')" -ForegroundColor Gray
Write-Host "   TIP Profile: HKCU=$tipHkcu  HKLM=$tipHklm" -ForegroundColor Gray
foreach ($hive in @("Registry::HKCU\Software\Microsoft\CTF\TIP\$clsid", "Registry::HKLM\SOFTWARE\Microsoft\CTF\TIP\$clsid")) {
    if (Test-Path $hive) {
        Write-Host "   -- $hive" -ForegroundColor DarkGray
        Get-ChildItem $hive -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host "      $($_.Name)" -ForegroundColor DarkGray
        }
    }
}

if ($hkcr -and ($tipHkcu -or $tipHklm)) {
    Write-Host "`n✅ 注册成功！" -ForegroundColor Green
    Write-Host "`n接下来："
    Write-Host "  1. 打开  设置 → 时间和语言 → 语言和区域 → 中文(简体，中国)"
    Write-Host "  2. 点击  ⋮ → 语言选项 → 添加键盘 → 选择 [Sample IME]"
    Write-Host "  3. 或在任意输入框按  Win + Space 切换输入法"
    Write-Host "  4. 输入拼音（如 ni hao），候选窗会出现对应汉字"
} else {
    Write-Host "`n❌ 注册不完整：" -ForegroundColor Red
    Write-Host "   CLSID 注册: $($null -ne $hkcr)"
    Write-Host "   TIP Profile: HKCU=$tipHkcu  HKLM=$tipHklm"
    Write-Host "请检查上方输出；TIP 注册通常位于 HKCU，若提权环境可能写入 HKLM。"
}
Read-Host "`n按回车键关闭窗口"
