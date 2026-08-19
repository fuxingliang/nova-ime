# ============================================================
#  parse-dump.ps1 — 轻量 minidump 解析
#  读取崩溃 dump 的异常信息与模块列表，定位崩溃指令所在模块。
#  用法: .\parse-dump.ps1 <dump文件>
# ============================================================

param([Parameter(Mandatory = $true)][string]$DumpPath)

$bytes = [System.IO.File]::ReadAllBytes($DumpPath)

function U32($off) { return [BitConverter]::ToUInt32($bytes, $off) }
function U64($off) { return [BitConverter]::ToUInt64($bytes, $off) }

if ((U32 0) -ne 0x504D444D) { Write-Host '不是有效的 minidump'; exit 1 }

$numStreams = U32 8
$dirRva     = U32 12

$exStream = $null
$modStream = $null
$threadStream = $null

for ($i = 0; $i -lt $numStreams; $i++) {
    $e = $dirRva + $i * 12
    $type = U32 $e
    $size = U32 ($e + 4)
    $rva  = U32 ($e + 8)
    if ($type -eq 6) { $exStream = @{ Rva = $rva; Size = $size } }
    if ($type -eq 4) { $modStream = @{ Rva = $rva; Size = $size } }
    if ($type -eq 3) { $threadStream = @{ Rva = $rva; Size = $size } }
}

# 模块基址表（用于栈扫描匹配）
$modules = @()
if ($modStream) {
    $m = $modStream.Rva
    $count = U32 $m
    for ($i = 0; $i -lt $count; $i++) {
        $mod = $m + 4 + $i * 108
        $base = U64 $mod
        $size = U32 ($mod + 8)
        $nameRva = U32 ($mod + 20)
        $nlen = U32 $nameRva
        $name = [System.Text.Encoding]::Unicode.GetString($bytes, $nameRva + 4, $nlen)
        $modules += @{ Base = $base; Size = $size; Name = $name }
    }
}

# 崩溃时寄存器（MINIDUMP_EXCEPTION_STREAM.ThreadContext → CONTEXT x64）
if ($exStream) {
    $ctxRva = U32 ($exStream.Rva + 164)
    if ($ctxRva -gt 0) {
        # CONTEXT x64: Rax@120, Rbx@144, Rsp@152, Rbp@160, Rdi@176, Rip@248
        $regs = [ordered]@{
            'RAX' = U64 ($ctxRva + 120)
            'RBX' = U64 ($ctxRva + 144)
            'RSP' = U64 ($ctxRva + 152)
            'RBP' = U64 ($ctxRva + 160)
            'RDI' = U64 ($ctxRva + 176)
            'RIP' = U64 ($ctxRva + 248)
        }
        Write-Host "崩溃线程寄存器:"
        foreach ($k in $regs.Keys) {
            $v = $regs[$k]
            $hit = $modules | Where-Object { $v -ge $_.Base -and $v -lt ($_.Base + $_.Size) } | Select-Object -First 1
            if ($hit) {
                Write-Host ("  {0} = 0x{1:X}   → {2}  (+0x{3:X})" -f $k, $v, [System.IO.Path]::GetFileName($hit.Name), ($v - $hit.Base))
            } else {
                Write-Host ("  {0} = 0x{1:X}" -f $k, $v)
            }
        }
    }
}

Write-Host "========== 崩溃 dump 分析 =========="
if ($exStream) {
    $e = $exStream.Rva
    $code = U32 ($e + 8)          # MINIDUMP_EXCEPTION.ExceptionCode
    $addr = U64 ($e + 16)         # ExceptionAddress
    $tid  = U32 $e                 # ThreadId
    Write-Host ("异常代码 : 0x{0:X8}  (c0000005=访问违例)" -f $code)
    Write-Host ("异常线程 : {0}" -f $tid)
    Write-Host ("异常地址 : 0x{0:X}" -f $addr)
} else {
    Write-Host '无异常流'
}

Write-Host ""
Write-Host "========== 已加载模块（按基址） =========="
if ($modStream) {
    $m = $modStream.Rva
    $count = U32 $m
    Write-Host ("模块数: {0}" -f $count)
    $found = $false
    for ($i = 0; $i -lt $count; $i++) {
        $mod = $m + 4 + $i * 108
        $base = U64 $mod
        $size = U32 ($mod + 8)
        $nameRva = U32 ($mod + 20)
        $nlen = U32 $nameRva
        $name = [System.Text.Encoding]::Unicode.GetString($bytes, $nameRva + 4, $nlen)
        # 只列出可疑/相关模块
        if ($name -match 'SampleIME|PinyinPlus|QQ|Chrome|electron|msctf|TSF|Input|ctf|ime') {
            Write-Host ("  0x{0:X16}  {1,-45} {2}" -f $base, $name, $size)
        }
        if ($exStream) {
            $addr = U64 ($exStream.Rva + 16)
            if ($addr -ge $base -and $addr -lt ($base + $size)) {
                Write-Host ""
                Write-Host ">>> 崩溃指令位于模块: $name" -ForegroundColor Yellow
                Write-Host ("    模块基址 0x{0:X}  偏移 0x{1:X}" -f $base, ($addr - $base))
                $found = $true
            }
        }
    }
    if (-not $found -and $exStream) { Write-Host ">>> 异常地址未落在已加载模块内（可能位于已卸载模块）" -ForegroundColor Yellow }
}

Write-Host ""
Write-Host "========== 崩溃线程调用栈（扫描栈内存中的模块返回地址） =========="
if ($exStream -and $threadStream) {
    $exTid = U32 $exStream.Rva
    $t = $threadStream.Rva
    $tCount = U32 $t
    $crashThread = $null
    for ($i = 0; $i -lt $tCount; $i++) {
        $th = $t + 4 + $i * 48
        if ((U32 $th) -eq $exTid) {
            # MINIDUMP_THREAD.Stack: StartOfMemoryRange@24, DataSize@32, Rva@36
            $stackStart = U64 ($th + 24)
            $stackSize  = U32 ($th + 32)
            $stackRva   = U32 ($th + 36)
            $crashThread = @{ Start = $stackStart; Size = $stackSize; Rva = $stackRva; Id = $exTid }
            break
        }
    }
    if ($crashThread) {
        Write-Host ("线程 {0} 栈范围: 0x{1:X} 长度 {2} 字节" -f $crashThread.Id, $crashThread.Start, $crashThread.Size)
        $counts = @{}
        $ourAddrs = @()   # 栈上落在 Nova DLL 内的地址（原始地址）
        $stackBytes = $bytes[$crashThread.Rva .. ($crashThread.Rva + $crashThread.Size - 1)]
        for ($off = 0; $off + 8 -le $stackBytes.Length; $off += 8) {
            $v = [BitConverter]::ToUInt64($stackBytes, $off)
            if ($v -eq 0) { continue }
            foreach ($md in $modules) {
                if ($v -ge $md.Base -and $v -lt ($md.Base + $md.Size)) {
                    $key = $md.Name
                    if (-not $counts.ContainsKey($key)) { $counts[$key] = 0 }
                    $counts[$key]++
                    if ($key -match 'SampleIME|PinyinPlusIme') { $ourAddrs += $v }
                    break
                }
            }
        }
        $counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 20 | ForEach-Object {
            Write-Host ("  {0,-5} {1}" -f $_.Value, $_.Key)
        }
        # 栈上 Nova DLL 的具体地址（去重，供 dbh 符号化）
        $ourDllBase = ($modules | Where-Object { $_.Name -match 'SampleIME|PinyinPlusIme' } | Select-Object -First 1).Base
        if ($ourDllBase) {
            Write-Host ""
            Write-Host "Nova DLL 在崩溃线程栈上的地址（RVA）:"
            $ourAddrs | Sort-Object -Unique | ForEach-Object {
                Write-Host ("  0x{0:X}" -f ($_ - $ourDllBase))
            }
        }
        # 特别标记我们的 DLL
        if ($counts.Keys | Where-Object { $_ -match 'SampleIME|PinyinPlusIme' }) {
            Write-Host "" -ForegroundColor DarkYellow
            Write-Host "!!! 调用栈中包含 Nova 输入法 DLL —— 崩溃与输入法相关" -ForegroundColor Yellow
        } else {
            Write-Host "" -ForegroundColor DarkYellow
            Write-Host "调用栈中未发现 Nova 输入法 DLL —— 崩溃更可能是 QQ 自身" -ForegroundColor Green
        }
    } else {
        Write-Host "未找到崩溃线程的栈信息"
    }
}
