# ============================================================
#  Nova 输入法 - 安装包构建脚本
#
#  运行：右键此文件 → 使用 PowerShell 运行（无需管理员）
#
#  过程：
#    1. 停止旧的输入法进程（避免文件占用）
#    2. 编译 DLL + 引擎（MSBuild Release x64）
#    3. 发布候选窗服务器（dotnet publish self-contained win-x64）
#    4. 用 Inno Setup 编译安装包 → dist\NovaInput-Setup-<版本>.exe
#
#  前置：安装 Inno Setup 6（winget install JRSoftware.InnoSetup）
# ============================================================

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

Write-Host ''
Write-Host '========== Nova 输入法 安装包构建 ==========' -ForegroundColor Cyan
Write-Host ''

# ---------- 第 1 步：编译 DLL + 引擎 ----------
# 注：MSBuild 输出到 src\*\x64\Release\，不直接覆盖 bin 目录里被占用的 DLL，
# 所以构建阶段无需杀进程。Inno Setup 安装阶段会自行停 Server/Engine（仅自己的进程）。
Write-Host '[1/3] 编译 DLL + 引擎（MSBuild Release x64）...' -ForegroundColor Cyan
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -Last 1
if (-not $msbuild) { Write-Host '错误：未找到 MSBuild（需要 Visual Studio）。' -ForegroundColor Red; exit 1 }

# 编译输出落盘（避免终端被吞），失败时显示错误行
$dllLog = "$root\dist\build_dll.log"
& $msbuild "$root\src\ime\SampleIME.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo *> $dllLog
if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$root\src\ime\x64\Release\SampleIME.dll")) {
    Get-Content $dllLog | Select-String 'error' | Select-Object -First 10
    Write-Host '错误：DLL 编译失败。' -ForegroundColor Red; exit 1
}
$engLog = "$root\dist\build_engine.log"
& $msbuild "$root\src\engine\PinyinPlus.Engine.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo *> $engLog
if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$root\src\engine\x64\Release\PinyinPlus.Engine.exe")) {
    Get-Content $engLog | Select-String 'error' | Select-Object -First 10
    Write-Host '错误：引擎编译失败。' -ForegroundColor Red; exit 1
}
# 激活工具（EnableLanguageProfile + 设为默认输入法）
$actLog = "$root\dist\build_activate.log"
& $msbuild "$root\src\activate\ImeActivate.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo *> $actLog
if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$root\src\activate\x64\Release\ImeActivate.exe")) {
    Get-Content $actLog | Select-String 'error' | Select-Object -First 10
    Write-Host '错误：激活工具编译失败。' -ForegroundColor Red; exit 1
}
Write-Host '      编译完成。' -ForegroundColor DarkGray

# ---------- 第 2 步：发布候选窗服务器（self-contained） ----------
Write-Host '[2/3] 发布候选窗服务器（self-contained win-x64）...' -ForegroundColor Cyan
& dotnet publish "$root\src\server\PinyinPlus.Server.csproj" -c Release -o "$root\dist\server" --nologo
if ($LASTEXITCODE -ne 0) { Write-Host '错误：服务器发布失败。' -ForegroundColor Red; exit 1 }
if (-not (Test-Path "$root\dist\server\PinyinPlus.Server.exe")) { Write-Host '错误：发布产物缺失。' -ForegroundColor Red; exit 1 }
Write-Host '      发布完成。' -ForegroundColor DarkGray

# ---------- 第 2.5 步：预生成词库二进制缓存 ----------
# 消除安装后引擎首次启动的解析重活（29MB 文本解析 + 聚合字频 + 排序 + 写缓存）。
# 缓存随安装包分发，安装目录 txt 的 mtime 由 Inno 原样保留 → 缓存命中。
# 缓存有效性 = 缓存头里的 txtSize/txtMtime 与当前词库一致（不能拿 bin 文件自身
# 的 mtime 比——bin 的 mtime 与命中无关）。这里解析缓存头做真实校验，
# 词库文本更新后能立刻发现"缓存陈旧"。
Write-Host '[3.5/4] 预生成词库二进制缓存...' -ForegroundColor Cyan
& "$root\src\activate\x64\Release\ImeDictTest.exe" *> $null
$cacheBin = "$root\bin\pinyin-plus-big.txt.bin"
if (-not (Test-Path $cacheBin)) {
    Write-Host '      警告：词库缓存未生成（安装后首次启动由引擎后台生成，会稍慢）。' -ForegroundColor Yellow
} else {
    $txt = Get-Item "$root\bin\pinyin-plus-big.txt"
    $hdr = [System.IO.File]::ReadAllBytes($cacheBin)[0..27]
    $hdrSize  = [BitConverter]::ToUInt64($hdr, 8)   # u64 txtSize
    $hdrMtime = [BitConverter]::ToUInt64($hdr, 16)  # u64 txtMtime (FILETIME)
    $curSize  = [uint64]$txt.Length
    $curMtime = [uint64]$txt.LastWriteTime.ToFileTimeUtc()
    if ($hdrSize -eq $curSize -and $hdrMtime -eq $curMtime) {
        $sz = [math]::Round((Get-Item $cacheBin).Length / 1MB, 1)
        Write-Host "      缓存有效（${sz} MB，mtime 与词库一致，安装后直接命中）。" -ForegroundColor DarkGray
    } else {
        Write-Host "      警告：缓存与词库不匹配（hdr=$hdrMtime cur=$curMtime），安装后首启会重新解析，请确认词库未被改动。" -ForegroundColor Yellow
    }
}

# ---------- 第 3 步：Inno Setup 打包 ----------
Write-Host '[3/3] 用 Inno Setup 编译安装包...' -ForegroundColor Cyan
$iscc = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
) | ForEach-Object { Get-Item $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
if (-not $iscc) {
    Write-Host '错误：未安装 Inno Setup 6。请先执行：' -ForegroundColor Red
    Write-Host '      winget install JRSoftware.InnoSetup' -ForegroundColor Yellow
    exit 1
}
& $iscc.FullName "$root\installer\NovaInput.iss"
if ($LASTEXITCODE -ne 0) { Write-Host '错误：安装包编译失败。' -ForegroundColor Red; exit 1 }

$setup = Get-ChildItem "$root\dist\NovaInput-Setup-*.exe" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Host ''
Write-Host '构建完成！' -ForegroundColor Green
Write-Host "  安装包：$($setup.FullName)  ($([math]::Round($setup.Length / 1MB, 1)) MB)" -ForegroundColor Gray
Write-Host '  安装后到：设置 → 时间和语言 → 语言和区域 → 中文 → 键盘 → 添加键盘 → [Nova 输入法]' -ForegroundColor Gray
Write-Host '  用户数据（词库/配置）存放在 %AppData%\NovaInput，卸载不丢失。' -ForegroundColor Gray
Write-Host ''
