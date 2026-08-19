# ============================================================
#  Nova 输入法（开发版）完整注销脚本
#
#  用途：彻底清除"开发期注册"（register.ps1 / deploy.ps1 写入的注册），
#        让机器回到干净状态，便于测试正式安装包。
#  用法：右键此文件 → 使用 PowerShell 运行（自动请求管理员权限）
#
#  清理内容：
#    1. regsvr32 /u（DllUnregisterServer：HKCR CLSID + TSF Profile + Category）
#    2. 兜底删除注册表残留（CLSID / TSF TIP，HKLM + HKCU 都清）
#    3. 删除开机自启动 Run 键（PinyinPlus.Server / NovaInput.Server）
#    4. 验证清理结果
#  注意：正在运行的应用仍持有旧 DLL，注销后建议注销/重启再测安装包。
# ============================================================

# --- 0. 自动请求管理员权限（UAC）---------------------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$is64bit = [Environment]::Is64BitProcess

if (-not $isAdmin -or -not $is64bit) {
    $argStr = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
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

$ErrorActionPreference = 'Continue'
$clsid = '{D2291A80-84D8-4641-9AB2-BDD1472C846B}'

Write-Host ''
Write-Host '========== Nova 输入法 开发版注销 ==========' -ForegroundColor Cyan

# --- 1. 停止输入法进程（引擎/服务器，避免占用与保活拉起）---
Write-Host '[1/4] 停止引擎与服务器进程...' -ForegroundColor Cyan
Get-Process PinyinPlus.Engine -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process PinyinPlus.Server -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

# --- 2. regsvr32 /u：从注册表读实际 DLL 路径再反注册 ---
Write-Host '[2/4] regsvr32 /u 反注册 DLL...' -ForegroundColor Cyan
$dll = (Get-ItemProperty "Registry::HKEY_CLASSES_ROOT\CLSID\$clsid\InprocServer32" -ErrorAction SilentlyContinue).'(default)'
if ($dll) {
    Write-Host "      实际注册的 DLL: $dll" -ForegroundColor DarkGray
    $out = & "$env:WINDIR\System32\regsvr32.exe" /u /s $dll 2>&1
    if ($LASTEXITCODE -eq 0) { Write-Host '      regsvr32 /u 完成。' -ForegroundColor DarkGray }
    else { Write-Host "      regsvr32 /u 退出码 $LASTEXITCODE（DLL 可能被占用，走第 3 步兜底删除）。" -ForegroundColor Yellow }
} else {
    Write-Host '      未发现 HKCR CLSID 注册，跳过。' -ForegroundColor DarkGray
}

# --- 3. 兜底删除注册表残留（reg.exe 直接删，比 regsvr32 更彻底）---
Write-Host '[3/4] 兜底删除注册表残留...' -ForegroundColor Cyan
reg delete "HKLM\SOFTWARE\Classes\CLSID\$clsid" /f 2>$null | Out-Null
reg delete "HKCU\SOFTWARE\Classes\CLSID\$clsid" /f 2>$null | Out-Null
reg delete "HKLM\SOFTWARE\Microsoft\CTF\TIP\$clsid" /f 2>$null | Out-Null
reg delete "HKCU\SOFTWARE\Microsoft\CTF\TIP\$clsid" /f 2>$null | Out-Null

# --- 4. 删除开机自启动 Run 键（HKCU + HKLM 都清）---
Write-Host '[4/4] 删除开机自启动 Run 键...' -ForegroundColor Cyan
foreach ($hive in @('HKCU', 'HKLM')) {
    reg delete "$hive\Software\Microsoft\Windows\CurrentVersion\Run" /v PinyinPlus.Server /f 2>$null | Out-Null
    reg delete "$hive\Software\Microsoft\Windows\CurrentVersion\Run" /v NovaInput.Server /f 2>$null | Out-Null
}

# --- 验证 ---
Write-Host ''
Write-Host '========== 验证清理结果 ==========' -ForegroundColor Cyan
$hkcr  = Test-Path "Registry::HKEY_CLASSES_ROOT\CLSID\$clsid"
$hkcuC = Test-Path "HKCU:\SOFTWARE\Classes\CLSID\$clsid"
$hkcuT = Test-Path "HKCU:\Software\Microsoft\CTF\TIP\$clsid"
$hklmT = Test-Path "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
$run   = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -ErrorAction SilentlyContinue).'PinyinPlus.Server'

Write-Host "  HKCR CLSID   : $(-not $hkcr)" -ForegroundColor Gray
Write-Host "  HKCU CLSID   : $(-not $hkcuC)" -ForegroundColor Gray
Write-Host "  TSF TIP HKCU : $(-not $hkcuT)" -ForegroundColor Gray
Write-Host "  TSF TIP HKLM : $(-not $hklmT)" -ForegroundColor Gray
Write-Host "  自启动 Run   : $($null -eq $run)" -ForegroundColor Gray

if (-not $hkcr -and -not $hkcuC -and -not $hkcuT -and -not $hklmT -and $null -eq $run) {
    Write-Host "`n✅ 开发版注册已全部清除。" -ForegroundColor Green
    Write-Host '   下一步：注销或重启系统后（释放旧 DLL），双击 NovaInput-Setup-1.0.0.exe 安装正式版。' -ForegroundColor Gray
} else {
    Write-Host "`n⚠ 仍有残留，请检查上方输出。" -ForegroundColor Yellow
}
Read-Host "`n按回车键关闭窗口"
