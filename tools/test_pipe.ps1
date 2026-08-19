# Pinyin-Plus 服务进程管道测试
# 用法：先启动服务进程（或本脚本自动启动），再发送测试消息

$serverExe = "g:\pinyin-plus\src\server\bin\Release\net9.0-windows\PinyinPlus.Server.exe"
$pipeName = "PinyinPlus.Service"

# 1. 启动服务进程
if (-not (Get-Process PinyinPlus.Server -ErrorAction SilentlyContinue)) {
    Write-Host "启动服务进程..." -ForegroundColor Cyan
    Start-Process $serverExe
    Start-Sleep -Seconds 2
}

# 2. 发送 ShowCandidates 帧
$enc = [System.Text.Encoding]::UTF8

function WriteStr($stream, [string]$s) {
    $bytes = $enc.GetBytes($s)
    $stream.Write([BitConverter]::GetBytes([int]$bytes.Length), 0, 4)
    $stream.Write($bytes, 0, $bytes.Length)
}

$payload = [System.IO.MemoryStream]::new()
WriteStr $payload "nihao"                          # 拼音缓冲
$payload.Write([BitConverter]::GetBytes([int]0), 0, 4)   # 选中索引
$payload.Write([BitConverter]::GetBytes([int]0), 0, 4)   # 页起始
$payload.Write([BitConverter]::GetBytes([int]6), 0, 4)   # 候选数
foreach ($c in @("你好", "尼", "妮", "拟", "泥", "呢")) {
    WriteStr $payload $c
}

$frame = [System.IO.MemoryStream]::new()
$frame.Write([BitConverter]::GetBytes([uint32]0x5050494D), 0, 4)  # magic "PPIM"
$frame.Write([BitConverter]::GetBytes([uint32]1), 0, 4)           # version
$frame.Write([BitConverter]::GetBytes([uint32]1), 0, 4)           # type = Show
$frame.Write([BitConverter]::GetBytes([uint32]$payload.Length), 0, 4)
$payload.Position = 0
$payload.CopyTo($frame)

$client = [System.IO.Pipes.NamedPipeClientStream]::new(".", "PinyinPlus.Service", [System.IO.Pipes.PipeDirection]::Out)
$client.Connect(3000)
$bytes = $frame.ToArray()
$client.Write($bytes, 0, $bytes.Length)
$client.Flush()
Start-Sleep -Milliseconds 300

# 3. 发送位置（屏幕中央附近）
$posPayload = [System.IO.MemoryStream]::new()
$posPayload.Write([BitConverter]::GetBytes([int]800), 0, 4)
$posPayload.Write([BitConverter]::GetBytes([int]400), 0, 4)
$posFrame = [System.IO.MemoryStream]::new()
$posFrame.Write([BitConverter]::GetBytes([uint32]0x5050494D), 0, 4)
$posFrame.Write([BitConverter]::GetBytes([uint32]1), 0, 4)
$posFrame.Write([BitConverter]::GetBytes([uint32]4), 0, 4)  # type = SetPosition
$posFrame.Write([BitConverter]::GetBytes([uint32]$posPayload.Length), 0, 4)
$posPayload.Position = 0
$posPayload.CopyTo($posFrame)
$pbytes = $posFrame.ToArray()
$client.Write($pbytes, 0, $pbytes.Length)
$client.Flush()
$client.Dispose()

Write-Host "已发送测试帧。请查看屏幕上是否出现深色候选窗。" -ForegroundColor Green
