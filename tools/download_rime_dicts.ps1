# Nova 词库数据源下载脚本 —— 拉取 rime-ice 最新词库到 tools/data/rime-ice/
#
# 数据源：https://github.com/iDvel/rime-ice （LGPL-3.0 开源词库）
# 用途：build_dict.py 的输入。rime-ice 词库持续更新，定期重跑本脚本即可同步。
#
# 用法：powershell -ExecutionPolicy Bypass -File tools/download_rime_dicts.ps1

$dest = Join-Path $PSScriptRoot "data\rime-ice"
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$files = @("8105.dict.yaml", "41448.dict.yaml", "base.dict.yaml", "ext.dict.yaml", "tencent.dict.yaml", "others.dict.yaml")
foreach ($f in $files) {
    $url = "https://raw.githubusercontent.com/iDvel/rime-ice/main/cn_dicts/$f"
    $out = Join-Path $dest $f
    try {
        Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing -TimeoutSec 120
        $size = (Get-Item $out).Length
        Write-Host "OK  $f  $size bytes"
    } catch {
        Write-Host "FAIL $f  $($_.Exception.Message)"
    }
}

# OpenCC 繁→简单字映射（build_dict.py 繁体过滤数据源，Apache-2.0）
$openccDest = Join-Path $PSScriptRoot "data\opencc"
New-Item -ItemType Directory -Force -Path $openccDest | Out-Null
try {
    $url = "https://raw.githubusercontent.com/BYVoid/OpenCC/master/data/dictionary/TSCharacters.txt"
    $out = Join-Path $openccDest "TSCharacters.txt"
    Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing -TimeoutSec 120
    Write-Host "OK  TSCharacters.txt  $((Get-Item $out).Length) bytes"
} catch {
    Write-Host "FAIL TSCharacters.txt  $($_.Exception.Message)"
}
Write-Host "done -> $dest"
