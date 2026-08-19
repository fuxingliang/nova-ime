#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Nova 引擎双缓冲热重载验证（ctypes 管道客户端，无第三方依赖）
流程：
  1. type 1 查询 nihao（基线耗时）
  2. type 11 触发后台热重载（应毫秒级返回，不阻塞）
  3. 立即连续查询，验证重建期间打字零阻塞
  4. 等待重建完成（engine_debug.log 出现 ReloadAll swap done）
用法：python tools/test_reload.py
"""
import ctypes
import struct
import sys
import time

PIPE = r"\\.\pipe\PinyinPlus.Engine"
LOG = r"g:\pinyin-plus\tools\engine_debug.log"

kernel32 = ctypes.windll.kernel32
GENERIC_READ_WRITE = 0xC0000000
OPEN_EXISTING = 3

# 64 位句柄必须显式声明 restype/argtypes，否则默认 c_int 截断
HANDLE = ctypes.c_void_p
kernel32.CreateFileW.restype = HANDLE
kernel32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                 ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
kernel32.WriteFile.restype = ctypes.c_int
kernel32.WriteFile.argtypes = [HANDLE, ctypes.c_void_p, ctypes.c_uint32,
                               ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
kernel32.ReadFile.restype = ctypes.c_int
kernel32.ReadFile.argtypes = [HANDLE, ctypes.c_void_p, ctypes.c_uint32,
                              ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
kernel32.CloseHandle.restype = ctypes.c_int
kernel32.CloseHandle.argtypes = [HANDLE]


def connect():
    h = kernel32.CreateFileW(PIPE, GENERIC_READ_WRITE, 0, None, OPEN_EXISTING, 0, None)
    if not h:
        raise RuntimeError("连接引擎管道失败（引擎未运行？）")
    return h


def send(h, type_, payload=b""):
    frame = struct.pack("<IIII", 0x5050494D, 1, type_, len(payload)) + payload
    written = ctypes.c_uint32()
    buf = ctypes.c_char_p(frame)
    ok = kernel32.WriteFile(h, buf, len(frame), ctypes.byref(written), None)
    if not ok:
        err = ctypes.get_last_error() or ctypes.windll.kernel32.GetLastError()
        raise RuntimeError(f"WriteFile 失败 err={err} written={written.value}")
    if written.value != len(frame):
        raise RuntimeError(f"WriteFile 半写 {written.value}/{len(frame)}")


def recv(h):
    hdr = ctypes.create_string_buffer(16)
    read = ctypes.c_ulong()
    if not kernel32.ReadFile(h, hdr, 16, ctypes.byref(read), None):
        raise RuntimeError("ReadFile 头失败")
    magic, ver, type_, plen = struct.unpack("<IIII", hdr.raw)
    payload = b""
    if plen:
        buf = ctypes.create_string_buffer(plen)
        if not kernel32.ReadFile(h, buf, plen, ctypes.byref(read), None):
            raise RuntimeError("ReadFile payload 失败")
        payload = buf.raw[:plen]
    return type_, payload


def query(h, py):
    t0 = time.perf_counter()
    send(h, 1, py.encode("utf-8"))
    t, p = recv(h)
    dt = (time.perf_counter() - t0) * 1000
    if t != 2 or len(p) < 4:
        return dt, -1
    count = struct.unpack("<I", p[:4])[0]
    return dt, count


def wait_swap(timeout=30):
    """等待 engine_debug.log 出现 ReloadAll swap done"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(LOG, encoding="utf-8", errors="ignore") as f:
                for line in f:
                    if "ReloadAll swap done" in line:
                        return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def main():
    h = connect()
    print("已连接引擎管道")

    # 1. 基线查询
    dt, n = query(h, "nihao")
    print(f"[1] 基线查询 nihao: {dt:.2f} ms, 候选 {n} 个")

    # 2. 触发热重载（type 11）
    t0 = time.perf_counter()
    send(h, 11)
    t, p = recv(h)
    dt11 = (time.perf_counter() - t0) * 1000
    print(f"[2] type 11 热重载触发: {dt11:.2f} ms（应毫秒级，不等待重建完成）")

    # 3. 重建期间连续查询（模拟打字）
    print("[3] 重建期间连续查询（打字应零阻塞）...")
    max_ms = 0.0
    for i in range(20):
        dt, n = query(h, "nihao")
        if dt > max_ms:
            max_ms = dt
        time.sleep(0.05)
    print(f"    重建期间 20 次查询，最大单次 {max_ms:.2f} ms（应 <10ms，无 7.5s 卡顿）")

    # 4. 等待重建完成
    ok = wait_swap()
    print(f"[4] 等待后台重建完成: {'是（engine_debug.log 确认 swap done）' if ok else '否（超时）'}")

    # 5. 重建后查询仍正常
    dt, n = query(h, "zhurongji")
    print(f"[5] 重建后查询 zhurongji: {dt:.2f} ms, 候选 {n} 个")

    kernel32.CloseHandle(h)
    print("\n验证结束")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("错误:", e)
        sys.exit(1)
