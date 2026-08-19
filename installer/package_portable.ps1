# ============================================================
#  Nova 输入法 - 便携版打包脚本（参照 Nova Studio 的 portable 发布流程）
#
#  运行：右键此文件 → 使用 PowerShell 运行（无需管理员）
#
#  前置：已完成构建（同 build_installer.ps1 的 1-3 步）：
#    · src\ime\x64\Release\SampleIME.dll          （MSBuild DLL）
#    · src\engine\x64\Release\PinyinPlus.Engine.exe（MSBuild 引擎）
#    · src\activate\x64\Release\ImeActivate.exe   （MSBuild 激活工具）
#    · dist\server\                                （dotnet publish self-contained）
#    · bin\pinyin-plus.txt / pinyin-plus-big.txt   （python tools\build_dict.py [--big]）
#    · tools\data\*.txt                             （powershell -File tools\download_rime_dicts.ps1）
#
#  产物：
#    dist\NovaInput-v<版本>-windows-portable\    （解压即用的文件夹）
#    dist\NovaInput-v<版本>-windows-portable.zip （上传 GitHub Release 用）
#
#  版本号从 installer\NovaInput.iss 的 MyAppVersion 读取（与安装包一致）。
# ============================================================

$ErrorActionPreference = 'Stop'
$instDir = $PSScriptRoot
$root    = Split-Path -Parent $instDir

Write-Host ''
Write-Host '========== Nova 输入法 便携版打包 ==========' -ForegroundColor Cyan

# ---------- 0. 读取版本号 ---------------------------------------------------
$iss = Get-Content (Join-Path $instDir 'NovaInput.iss') -Encoding UTF8
$verMatch = ($iss | Select-String -Pattern '#define\s+MyAppVersion\s+"([^"]+)"' | Select-Object -First 1)
if (-not $verMatch) { Write-Host '错误：NovaInput.iss 中未找到 MyAppVersion。' -ForegroundColor Red; exit 1 }
$version = $verMatch.Matches[0].Groups[1].Value
Write-Host ("版本: v" + $version) -ForegroundColor DarkGray

# ---------- 1. 输入检查 ------------------------------------------------------
# 必需文件：路径 → 缺失时的构建提示
$required = @(
    @{ P = 'src\ime\x64\Release\SampleIME.dll';            Hint = 'MSBuild src\ime\SampleIME.vcxproj /p:Configuration=Release /p:Platform=x64' },
    @{ P = 'src\engine\x64\Release\PinyinPlus.Engine.exe'; Hint = 'MSBuild src\engine\PinyinPlus.Engine.vcxproj /p:Configuration=Release /p:Platform=x64' },
    @{ P = 'src\activate\x64\Release\ImeActivate.exe';     Hint = 'MSBuild src\activate\ImeActivate.vcxproj /p:Configuration=Release /p:Platform=x64' },
    @{ P = 'dist\server\PinyinPlus.Server.exe';            Hint = 'dotnet publish src\server\PinyinPlus.Server.csproj -c Release -o dist\server' },
    @{ P = 'bin\pinyin-plus.txt';                          Hint = 'python tools\build_dict.py' },
    @{ P = 'bin\pinyin-plus-big.txt';                      Hint = 'python tools\build_dict.py --big' },
    @{ P = 'tools\data\STCharacters.txt';                  Hint = 'powershell -ExecutionPolicy Bypass -File tools\download_rime_dicts.ps1' },
    @{ P = 'tools\data\STPhrases.txt';                     Hint = '同上（download_rime_dicts.ps1）' },
    @{ P = 'tools\data\TSCharacters.txt';                  Hint = '同上（download_rime_dicts.ps1）' },
    @{ P = 'tools\data\TSPhrases.txt';                     Hint = '同上（download_rime_dicts.ps1）' },
    @{ P = 'tools\data\symbols.txt';                       Hint = '同上（download_rime_dicts.ps1）' }
)
$missing = @()
foreach ($r in $required) {
    if (-not (Test-Path (Join-Path $root $r.P))) {
        $missing += @{ P = $r.P; Hint = $r.Hint }
    }
}
if ($missing.Count -gt 0) {
    Write-Host ''
    Write-Host '错误：以下构建产物/数据缺失，请先构建再打包：' -ForegroundColor Red
    foreach ($m in $missing) {
        Write-Host ("  - " + $m.P) -ForegroundColor Red
        Write-Host ("      构建: " + $m.Hint) -ForegroundColor Yellow
    }
    exit 1
}
# 可选：预生成词库缓存（缺失只警告——引擎首启会自行生成，仅首次稍慢）
foreach ($cache in @('pinyin-plus-big.txt.bin', 'pinyin-plus.txt.bin')) {
    if (-not (Test-Path (Join-Path $root "bin\$cache"))) {
        Write-Host "提示：未找到词库缓存 bin\$cache（引擎首次加载对应词库时自行生成，稍慢，不影响功能）。" -ForegroundColor Yellow
    }
}

# ---------- 2. 组装便携版文件夹 ---------------------------------------------
$tag = "NovaInput-v$version-windows-portable"
$out = Join-Path $root "dist\$tag"
Write-Host ''
Write-Host "[1/3] 组装 $tag ..." -ForegroundColor Cyan
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Path $out | Out-Null

# 运行文件（与安装包 {app} 布局一致：Server 在 <根>\server\，数据在 %AppData%）
Copy-Item (Join-Path $root 'src\ime\x64\Release\SampleIME.dll')          (Join-Path $out 'SampleIME.dll')
Copy-Item (Join-Path $root 'src\engine\x64\Release\PinyinPlus.Engine.exe') (Join-Path $out 'PinyinPlus.Engine.exe')
Copy-Item (Join-Path $root 'src\activate\x64\Release\ImeActivate.exe')    (Join-Path $out 'ImeActivate.exe')
Copy-Item (Join-Path $root 'bin\pinyin-plus.txt')                        (Join-Path $out 'pinyin-plus.txt')
Copy-Item (Join-Path $root 'bin\pinyin-plus-big.txt')                    (Join-Path $out 'pinyin-plus-big.txt')
$withCache = $false
foreach ($c in @('pinyin-plus.txt.bin', 'pinyin-plus-big.txt.bin')) {
    if (Test-Path (Join-Path $root "bin\$c")) {
        Copy-Item (Join-Path $root "bin\$c") (Join-Path $out $c)
        $withCache = $true
    }
}
if ($withCache) { Write-Host '      含预生成词库缓存（首启直接命中）。' -ForegroundColor DarkGray }
foreach ($d in @('STCharacters.txt', 'STPhrases.txt', 'TSCharacters.txt', 'TSPhrases.txt', 'symbols.txt')) {
    Copy-Item (Join-Path $root "tools\data\$d")                          (Join-Path $out $d)
}
# 候选窗服务（self-contained，免装 .NET 运行时）
New-Item -ItemType Directory -Path (Join-Path $out 'server') | Out-Null
Copy-Item (Join-Path $root 'dist\server\*') (Join-Path $out 'server') -Recurse -Force
# 一键安装/卸载 + 快速上手
foreach ($s in @('install-portable.ps1', 'uninstall-portable.ps1', 'README-quickstart.txt')) {
    Copy-Item (Join-Path $instDir "portable\$s") (Join-Path $out $s)
}

# ---------- 3. 压缩 ----------------------------------------------------------
Write-Host ''
Write-Host '[2/3] 压缩 zip ...' -ForegroundColor Cyan
$zip = Join-Path $root "dist\$tag.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $out -DestinationPath $zip -Force

# ---------- 汇总 -------------------------------------------------------------
Write-Host ''
Write-Host '[3/3] 完成！' -ForegroundColor Green
$dirSize  = ((Get-ChildItem $out -Recurse | Measure-Object -Property Length -Sum).Sum / 1MB)
$zipSize  = (Get-Item $zip).Length / 1MB
Write-Host ("  文件夹: " + $out + "  ($([math]::Round($dirSize, 1)) MB)") -ForegroundColor Gray
Write-Host ("  压缩包: " + $zip + "  ($([math]::Round($zipSize, 1)) MB)") -ForegroundColor Gray
Write-Host ''
Write-Host '发布：把 zip 上传到 GitHub Release（与安装包同版本），README 的下载链接即可用。' -ForegroundColor Gray
Write-Host '用户侧：解压 → 右键 install-portable.ps1 用 PowerShell 运行（UAC 一次）→ 即用。' -ForegroundColor Gray
Write-Host ''
