# ============================================================
#  Nova 输入法 - 一键部署脚本
#
#  运行方法：右键此文件 → 使用 PowerShell 运行
#            （第一次会弹出 UAC 提权窗口，点"是"即可）
#
#  功能：无论你改了哪部分代码（输入法 DLL / 拼音引擎 / 候选窗服务器），
#        运行本脚本都会全部重新编译、部署并注册，一条龙搞定。
#
#  过程一共 5 步：
#    1. 停止旧的输入法进程
#    2. 重新编译（DLL + 引擎 + 服务器）
#    3. 复制新文件到 bin 目录
#    4. 注册输入法 + 设置开机自启动
#    5. 启动输入法服务
# ============================================================

# ---------- 项目目录 ----------
$root = 'g:\pinyin-plus'

Write-Host ''
Write-Host '========== Nova 输入法 一键部署 ==========' -ForegroundColor Cyan
Write-Host ''

# ---------- 第 1 步：停止旧进程 ----------
Write-Host '[1/5] 停止旧的输入法进程...' -ForegroundColor Cyan
Get-Process PinyinPlus.Engine -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process PinyinPlus.Server -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Write-Host '      已停止。' -ForegroundColor DarkGray

# ---------- 第 2 步：重新编译 ----------
Write-Host '[2/5] 重新编译（DLL + 引擎 + 服务器）...' -ForegroundColor Cyan

# 找到 Visual Studio 自带的 MSBuild 编译器
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -Last 1
if (-not $msbuild) {
    Write-Host '错误：没有找到 MSBuild（需要安装 Visual Studio）。' -ForegroundColor Red
    exit 1
}

# 编译输入法 DLL
Write-Host '      编译输入法 DLL ...' -ForegroundColor DarkGray
& $msbuild "$root\src\ime\SampleIME.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
if ($LASTEXITCODE -ne 0) { Write-Host '错误：DLL 编译失败。' -ForegroundColor Red; exit 1 }

# 编译拼音引擎
Write-Host '      编译拼音引擎 ...' -ForegroundColor DarkGray
& $msbuild "$root\src\engine\PinyinPlus.Engine.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
if ($LASTEXITCODE -ne 0) { Write-Host '错误：引擎编译失败。' -ForegroundColor Red; exit 1 }

# 编译候选窗服务器
Write-Host '      编译候选窗服务器 ...' -ForegroundColor DarkGray
dotnet build "$root\src\server\PinyinPlus.Server.csproj" -c Release --nologo
if ($LASTEXITCODE -ne 0) { Write-Host '错误：服务器编译失败。' -ForegroundColor Red; exit 1 }

Write-Host '      编译完成。' -ForegroundColor DarkGray

# ---------- 第 3 步：复制文件 ----------
Write-Host '[3/5] 复制新文件到 bin 目录...' -ForegroundColor Cyan

# DLL 用"带时间戳的文件名"：正在运行的程序不会锁住它，所以不用重启电脑/注销，
# 新打开的程序就会用新版本。
$ver = Get-Date -Format 'yyyyMMdd-HHmmss'
$newDll = "$root\bin\PinyinPlusIme-$ver.dll"
Copy-Item "$root\src\ime\x64\Release\SampleIME.dll" $newDll

# 引擎复制到 bin（引擎 exe 固定文件名，进程已停所以可以直接覆盖）。
# 注意：第 1 步杀掉的引擎会被 DLL 保活线程在 ~2 秒内重新拉起，可能占用 exe
# 导致复制失败（部署后引擎仍是旧代码）。因此复制前再清一次引擎进程。
Get-Process PinyinPlus.Engine -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Copy-Item "$root\src\engine\x64\Release\PinyinPlus.Engine.exe" "$root\bin\PinyinPlus.Engine.exe" -Force
# 万一仍被占用，重试最多 3 次（每次等 1 秒，给保活线程让出文件）
for ($i = 1; $i -le 3 -and -not $?) { Start-Sleep -Seconds 1; Copy-Item "$root\src\engine\x64\Release\PinyinPlus.Engine.exe" "$root\bin\PinyinPlus.Engine.exe" -Force }
if (-not $?) { Write-Host '      警告：引擎 exe 复制失败（文件被占用），部署后引擎可能仍是旧版。' -ForegroundColor Yellow }

# 简繁转换表（OpenCC，Apache-2.0）→ bin（引擎繁体输出运行期读取）
Copy-Item "$root\tools\data\STCharacters.txt", "$root\tools\data\STPhrases.txt", `
    "$root\tools\data\TSCharacters.txt", "$root\tools\data\TSPhrases.txt" `
    "$root\bin\" -Force

# 顺手清理 7 天前的旧版本 DLL（正在被占用的会自动跳过）
Get-ChildItem "$root\bin\PinyinPlusIme-*.dll" -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -lt (Get-Date).AddDays(-7) } |
    ForEach-Object { try { Remove-Item $_.FullName -Force -ErrorAction Stop } catch { } }

Write-Host "      新 DLL: $newDll" -ForegroundColor DarkGray

# ---------- 第 4 步：注册输入法 ----------
Write-Host '[4/5] 注册输入法（可能弹出 UAC 确认）...' -ForegroundColor Cyan
& "$root\tools\register.ps1" -Dll $newDll
if ($LASTEXITCODE -ne 0) { Write-Host '错误：注册失败。' -ForegroundColor Red; exit 1 }

# 设置开机自启动：登录后自动启动候选窗服务器
$serverExe = "$root\src\server\bin\Release\net9.0-windows\PinyinPlus.Server.exe"
New-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' `
    -Name 'PinyinPlus.Server' -Value $serverExe -PropertyType String -Force | Out-Null
Write-Host '      已注册，开机自启动已设置。' -ForegroundColor DarkGray

# ---------- 第 5 步：启动 ----------
Write-Host '[5/5] 启动输入法服务...' -ForegroundColor Cyan
# 关键：第 1 步杀掉引擎后，DLL 的保活线程会在编译期间把"旧版引擎"重新拉起，
# 若不再次清理，下面启动的新引擎会被单实例互斥挡掉 → 部署后引擎仍是旧代码。
# 因此这里先彻底清掉旧引擎进程，再启动新版，保证引擎一定是新版本。
Get-Process PinyinPlus.Engine -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
# 先启动引擎；引擎会看护服务器（服务器被关掉会自动拉起）
Start-Process "$root\bin\PinyinPlus.Engine.exe" -WindowStyle Hidden
Start-Sleep -Seconds 2
# 再启动服务器（保证一定在运行）
Start-Process $serverExe

Write-Host ''
Write-Host '部署完成！' -ForegroundColor Green
Write-Host '  · 新打开的程序会使用新版本的输入法。' -ForegroundColor Gray
Write-Host '  · 已经打开的程序（如浏览器/编辑器）下一次输入自动走新引擎，不用重启。' -ForegroundColor Gray
Write-Host '  · 如果输入法列表里没有 [Nova 输入法]，请到：设置 → 语言 → 中文 → 键盘 添加。' -ForegroundColor Gray
Write-Host ''
