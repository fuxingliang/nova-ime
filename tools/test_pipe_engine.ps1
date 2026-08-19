# 引擎管道最小客户端测试（.NET NamedPipeClientStream，无 ctypes 坑）
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "PinyinPlus.Engine", [System.IO.Pipes.PipeDirection]::InOut)
try {
    $pipe.Connect(3000)
} catch {
    Write-Host "连接失败: $_"
    exit 1
}
Write-Host "已连接"

# 帧: magic=0x5050494D, ver=1, type=1, len=5, payload=nihao
$payload = [System.Text.Encoding]::UTF8.GetBytes("nihao")
$frame = New-Object byte[] (16 + $payload.Length)
[BitConverter]::GetBytes([uint32]0x5050494D).CopyTo($frame, 0)
[BitConverter]::GetBytes([uint32]1).CopyTo($frame, 4)
[BitConverter]::GetBytes([uint32]1).CopyTo($frame, 8)
[BitConverter]::GetBytes([uint32]$payload.Length).CopyTo($frame, 12)
$payload.CopyTo($frame, 16)

$pipe.Write($frame, 0, $frame.Length)
$pipe.Flush()
Write-Host "已发送 $($frame.Length) 字节"

# 读响应头 16 字节（异步 + 5s 超时，避免永久阻塞）
$hdr = New-Object byte[] 16
$total = 0
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$timeout = $false
while ($total -lt 16 -and $sw.ElapsedMilliseconds -lt 5000) {
    $readTask = $pipe.ReadAsync($hdr, $total, 16 - $total)
    if (-not $readTask.Wait(1000)) {
        $timeout = $true
        break
    }
    $n = $readTask.Result
    if ($n -le 0) { break }
    $total += $n
}
$sw.Stop()
if ($total -lt 16) {
    Write-Host "读响应头失败（$total/16 字节, $($sw.ElapsedMilliseconds)ms, 超时=$timeout）"
} else {
    $magic = [BitConverter]::ToUInt32($hdr, 0)
    $type = [BitConverter]::ToUInt32($hdr, 8)
    $plen = [BitConverter]::ToUInt32($hdr, 12)
    Write-Host ("收到响应 type={0} plen={1} 耗时 {2}ms" -f $type, $plen, $sw.ElapsedMilliseconds)
    if ($plen -gt 0) {
        $body = New-Object byte[] $plen
        $t2 = 0
        while ($t2 -lt $plen) { $n = $pipe.Read($body, $t2, $plen - $t2); if ($n -le 0) { break }; $t2 += $n }
        Write-Host ("候选数: {0}" -f [BitConverter]::ToUInt32($body, 0))
    }
}
$pipe.Dispose()
