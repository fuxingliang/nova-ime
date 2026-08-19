#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Nova 引擎大字库模式验证（ctypes 管道客户端）
流程：
  1. engine.conf bigdict=0 → 启动引擎 → 查 da/lei 候选不含龘/靁（默认词库过滤）
  2. engine.conf bigdict=1 → type 11 热重载（双缓冲，不重启引擎进程）
  3. 等 swap done → 查 da/lei 候选含龘/靁（大字库生僻字可达）
  4. 切回 bigdict=0 → type 11 → 查 da 候选不再含龘（双向切换）
用法：python tools/test_bigdict.py
"""
import ctypes
import os
import struct
import subprocess
import sys
import time

PIPE = r"\\.\pipe\PinyinPlus.Engine"
LOG = r"g:\pinyin-plus\tools\engine_debug.log"
ENGINE = r"g:\pinyin-plus\bin\PinyinPlus.Engine.exe"
CONF = r"g:\pinyin-plus\bin\engine.conf"

kernel32 = ctypes.windll.kernel32
GENERIC_READ_WRITE = 0xC0000000
OPEN_EXISTING = 3

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
        err = ctypes.windll.kernel32.GetLastError()
        raise RuntimeError(f"WriteFile 失败 err={err}")
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


def query_words(h, py):
    """type 1 查候选，返回 (ms, [词...])"""
    t0 = time.perf_counter()
    send(h, 1, py.encode("utf-8"))
    t, p = recv(h)
    dt = (time.perf_counter() - t0) * 1000
    if t != 2 or len(p) < 4:
        return dt, []
    count = struct.unpack("<I", p[:4])[0]
    words = []
    pos = 4
    for _ in range(count):
        ln = struct.unpack("<I", p[pos:pos + 4])[0]
        pos += 4
        words.append(p[pos:pos + ln].decode("utf-8"))
        pos += ln
    return dt, words


def write_conf(bigdict):
    with open(CONF, "w", encoding="ascii") as f:
        f.write(f"learn=1\nbigdict={1 if bigdict else 0}\n")
    print(f"    engine.conf → bigdict={1 if bigdict else 0}")


def wait_swap(timeout=60):
    """等待 engine_debug.log 出现新的 ReloadAll swap done（调用前先清空日志）"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(LOG, encoding="utf-8", errors="ignore") as f:
                tail = f.read()
            if "ReloadAll swap done" in tail:
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def clear_log():
    """清空引擎日志（引擎每次写都重新 fopen 追加，删除安全）"""
    try:
        os.remove(LOG)
    except OSError:
        pass


def start_engine():
    subprocess.Popen([ENGINE], creationflags=subprocess.CREATE_NO_WINDOW)
    time.sleep(2.5)   # 冷启动加载 29MB 词库


def stop_engine():
    subprocess.run(["taskkill", "/f", "/im", "PinyinPlus.Engine.exe"],
                   capture_output=True)


def check(label, words, want):
    hit = [w for w in words if w in want]
    status = "✓ 命中 " + ",".join(hit) if hit else "✗ 无"
    print(f"    {label}: {status} 候选总数={len(words)}")
    return len(hit) == len(want)


def main():
    stop_engine()
    time.sleep(0.5)

    # ---- 阶段 1：bigdict=0 默认词库 ----
    print("[1] bigdict=0 启动引擎（默认词库，无生僻字）")
    write_conf(False)
    clear_log()
    start_engine()
    h = connect()
    dt, words = query_words(h, "da")
    check("da 候选", words, ["龘"])
    dt, words = query_words(h, "lei")
    check("lei 候选", words, ["靁"])

    # ---- 阶段 2：bigdict=1 热重载换大字库 ----
    print("[2] bigdict=1 触发 type 11 热重载（不重启引擎进程）")
    write_conf(True)
    clear_log()
    t0 = time.perf_counter()
    send(h, 11)
    recv(h)
    dt11 = (time.perf_counter() - t0) * 1000
    print(f"    type 11 触发返回 {dt11:.2f} ms（应毫秒级）")
    if not wait_swap():
        print("    ✗ 等待 swap done 超时")
        return 1
    dt, words = query_words(h, "da")
    check("da 候选（大字库）", words, ["龘"])
    dt, words = query_words(h, "lei")
    check("lei 候选（大字库）", words, ["靁"])

    # ---- 阶段 3：切回 bigdict=0 ----
    print("[3] bigdict=0 再次热重载（双向切换）")
    write_conf(False)
    clear_log()
    send(h, 11)
    recv(h)
    if not wait_swap():
        print("    ✗ 等待 swap done 超时")
        return 1
    dt, words = query_words(h, "da")
    check("da 候选（切回默认）", words, ["龘"])

    kernel32.CloseHandle(h)
    print("\n验证结束")
    return 0


if __name__ == "__main__":
    sys.exit(main())
