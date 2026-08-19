# ============================================================
#  Nova 输入法（便携版）一键卸载
#
#  用法：右键此文件 → 使用 PowerShell 运行（自动请求管理员权限）
#
#  清理内容：
#    1. 停止 Nova 输入法进程（Server/引擎）
#    2. regsvr32 /u 反注册 DLL（HKCR CLSID + TSF Profile + Category）
#    3. 兜底删除注册表残留（CLSID / TSF TIP，HKLM + HKCU 都清）
#    4. 删除开机自启动 Run 键（HKCU 的 NovaInput.Server / PinyinPlus.Server）
#    5. 验证清理结果
#
#  之后手动删除本便携文件夹即可。
#  用户数据 %AppData%\NovaInput（词库/配置）刻意保留，不删除。
#  注意：正在运行的应用仍持有已加载的旧 DLL，删除文件夹若提示
#        文件占用，注销或重启后再删（不影响系统，注册已清除）。
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

$ErrorActionPreference = 'Continue'
$root  = $PSScriptRoot
$clsid = '{D2291A80-84D8-4641-9AB2-BDD1472C846B}'

Write-Host ''
Write-Host '========== Nova 输入法（便携版）卸载 ==========' -ForegroundColor Cyan

# --- 1. 停止自己的进程 -----------------------------------------------------
Write-Host '[1/4] 停止 Nova 输入法进程...' -ForegroundColor Cyan
taskkill /f /im PinyinPlus.Server.exe /t 2>$null | Out-Null
taskkill /f /im PinyinPlus.Engine.exe /t 2>$null | Out-Null
Start-Sleep -Milliseconds 500
Write-Host '      已停止。' -ForegroundColor DarkGray

# --- 2. regsvr32 /u 反注册（从注册表读实际 DLL 路径再反注册）-------------
Write-Host '[2/4] regsvr32 /u 反注册 DLL...' -ForegroundColor Cyan
$dll = (Get-ItemProperty "HKCR:\CLSID\$clsid\InprocServer32" -ErrorAction SilentlyContinue).'(default)'
if ($dll -and (Test-Path $dll)) {
    Write-Host ("      实际注册的 DLL: " + $dll) -ForegroundColor DarkGray
    $out = & "$env:WINDIR\System32\regsvr32.exe" /u /s $dll 2>&1
    if ($LASTEXITCODE -eq 0) { Write-Host '      反注册完成。' -ForegroundColor DarkGray }
    else { Write-Host "      regsvr32 /u 退出码 $LASTEXITCODE（DLL 可能被占用，走第 3 步兜底删除）。" -ForegroundColor Yellow }
} else {
    Write-Host '      未发现 HKCR CLSID 注册（或 DLL 已删除），跳过。' -ForegroundColor DarkGray
}

# --- 3. 兜底删除注册表残留（reg.exe 直接删，比 regsvr32 更彻底）----------
Write-Host '[3/4] 兜底删除注册表残留...' -ForegroundColor Cyan
reg delete "HKLM\SOFTWARE\Classes\CLSID\$clsid" /f 2>$null | Out-Null
reg delete "HKCU\SOFTWARE\Classes\CLSID\$clsid" /f 2>$null | Out-Null
reg delete "HKLM\SOFTWARE\Microsoft\CTF\TIP\$clsid" /f 2>$null | Out-Null
reg delete "HKCU\SOFTWARE\Microsoft\CTF\TIP\$clsid" /f 2>$null | Out-Null
Write-Host '      完成。' -ForegroundColor DarkGray

# --- 4. 删除开机自启动 Run 键（HKCU）--------------------------------------
Write-Host '[4/4] 删除开机自启动 Run 键...' -ForegroundColor Cyan
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v NovaInput.Server /f 2>$null | Out-Null
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v PinyinPlus.Server /f 2>$null | Out-Null
Write-Host '      完成。' -ForegroundColor DarkGray

# --- 验证 -------------------------------------------------------------------
Write-Host ''
Write-Host '========== 验证清理结果 ==========' -ForegroundColor Cyan
$hkr = Test-Path "HKCR:\CLSID\$clsid"
$hkc = Test-Path "HKCU:\SOFTWARE\Classes\CLSID\$clsid"
$tcu = Test-Path "HKCU:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
$tlm = Test-Path "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
$run = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -ErrorAction SilentlyContinue).'NovaInput.Server'

Write-Host "  TSF CLSID (HKCR): $($hkr ? '残留' : '已清除')" -ForegroundColor Gray
Write-Host "  TSF CLSID (HKCU): $($hkc ? '残留' : '已清除')" -ForegroundColor Gray
Write-Host "  TSF TIP HKCU    : $($tcu ? '残留' : '已清除')" -ForegroundColor Gray
Write-Host "  TSF TIP HKLM    : $($tlm ? '残留' : '已清除')" -ForegroundColor Gray
Write-Host "  开机自启动      : $($null -ne $run ? '残留' : '已清除')" -ForegroundColor Gray

Write-Host ''
if (-not $hkr -and -not $hkc -and -not $tcu -and -not $tlm -and $null -eq $run) {
    Write-Host '✅ 便携版已卸载（注册表已清除）。' -ForegroundColor Green
    Write-Host ''
    Write-Host '下一步：' -ForegroundColor Cyan
    Write-Host "  1. 删除便携文件夹：$root" -ForegroundColor Gray
    Write-Host '     （若提示文件占用，注销/重启后再删）' -ForegroundColor Gray
    Write-Host '  2. 用户词库/配置保留在 %AppData%\NovaInput（不需要可手动删除）' -ForegroundColor Gray
} else {
    Write-Host '⚠ 仍有残留（可能 DLL 被宿主进程占用），注销或重启后重新运行本脚本。' -ForegroundColor Yellow
}
Write-Host ''
Read-Host '按回车键关闭窗口'
