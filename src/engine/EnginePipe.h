//+---------------------------------------------------------------------------
//
//  EnginePipe.h
//
//  Pinyin-Plus 独立引擎进程的命名管道服务。
//  帧格式与 DLL/Server 侧 PPIM 一致：
//    [uint32 magic 0x5050494D][uint32 version][uint32 type][uint32 payloadLen][payload]
//  管道名：\\.\pipe\PinyinPlus.Engine
//
//  消息类型：
//    DLL→Engine   1 RequestCandidates   payload: [u32 pinyinLen(utf8)][utf8 pinyin]
//    Engine→DLL   2 ResponseCandidates  payload: [u32 count][(u32 len)(utf8 word)]*
//    DLL→Engine   3 RequestSyllableChars payload: [u32 sylLen(utf8)][utf8 syl]
//    Engine→DLL   4 ResponseSyllChars   payload: [u32 count][(u32 len)(utf8 word)]*
//    DLL→Engine   5 BoostWord           payload: [u32 plen][utf8 pinyin][u32 wlen][utf8 word]
//    DLL→Engine   6 AddUserWord         payload: [u32 plen][utf8 pinyin][u32 wlen][utf8 word]
//
//----------------------------------------------------------------------------

#pragma once

#include <windows.h>
#include "PinyinEngine.h"

// 引擎服务主循环：接受连接，逐请求-响应处理（每连接独立线程）。
// 阻塞直到进程退出。引擎实例由堆分配并持有（双缓冲热重载在此指针上切换）。
void RunEngineServer(CPinyinEngine* engine);
