# 便携版快速上手 / Portable Release Quick Start

> Nova 输入法便携版：**解压即用，无需安装程序**。
> 产物：`NovaInput-v<版本>-windows-portable.zip`（约 110 MB，含 88 万条词库 + self-contained .NET 运行时）。
> 构建方式：`powershell -ExecutionPolicy Bypass -File installer/package_portable.ps1`（见 [README · Build](../README.md#-build)）。

## What's included / 包内内容

```text
NovaInput-vX.Y.Z-windows-portable/
├── SampleIME.dll              # TSF 输入法壳（注入宿主进程）
├── PinyinPlus.Engine.exe      # 独立拼音引擎（词库/候选/整句预测/自学习）
├── PinyinPlus.Server.exe 目录 # server/：WPF 候选窗 + 托盘 + 设置面板（self-contained）
├── ImeActivate.exe            # 激活工具（进 Win+Space 列表 + 设为默认）
├── pinyin-plus*.txt / .bin    # rime-ice 词库（88 万条）+ 大字库 + 预生成缓存
├── ST*/TS*.txt / symbols.txt  # 简繁转换表（OpenCC）+ 符号面板数据
├── install-portable.ps1       # 一键安装（注册 + 激活 + 自启动 + 启动服务）
├── uninstall-portable.ps1     # 一键卸载（反注册 + 清自启动）
└── README-quickstart.txt      # 本说明的包内精简版
```

与安装包 [NovaInput.iss](../installer/NovaInput.iss) 的 `{app}` 布局完全一致：
Server 位于 `<根>\server\`，向上搜索词库定位安装目录；用户数据在 `%AppData%\NovaInput`。

## Install / 安装（约 1 分钟）

1. 把文件夹解压到任意位置（如 `D:\NovaInput`），**保持文件夹结构**。
2. 右键 `install-portable.ps1` → **使用 PowerShell 运行**（弹出 UAC 确认，点"是"）。
   脚本依次：停止旧进程 → `regsvr32` 注册 TSF（HKLM）→ 激活 + 设为默认（`ImeActivate`）
   → 写开机自启动 → 以普通用户完整性启动候选窗服务。
3. 在任意输入框按 `Win + Space` 切换到 **[Nova 输入法]** 即可打字。
4. 若输入法列表里没有它：**设置 → 时间和语言 → 语言和区域 → 中文 → 键盘 → 添加键盘 → [Nova 输入法]**。
5. 托盘出现 Nova 图标 = 候选窗服务运行中（双击打开设置面板）。

## Uninstall / 卸载

1. 右键 `uninstall-portable.ps1` → **使用 PowerShell 运行**（UAC 点"是"）。
2. 删除便携文件夹（若提示文件占用，注销/重启后再删）。
3. 用户词库/配置保留在 `%AppData%\NovaInput`（不需要可手动删除该目录）。

## 与安装包的差别 / Differences

| | 安装包 `NovaInput-Setup` | 便携版 zip |
|---|---|---|
| 文件位置 | `%LocalAppData%\NovaInput` | 你解压的任意目录（可移动/删除） |
| 安装方式 | 安装向导（UAC 一次） | `install-portable.ps1`（UAC 一次） |
| 卸载 | 设置 → 应用 → Nova 输入法 | `uninstall-portable.ps1` + 删文件夹 |
| 升级 | 重跑 Setup（自动升级路径） | 替换文件夹后重跑 `install-portable.ps1` |
| 用户数据 | `%AppData%\NovaInput`（卸载保留） | 相同（两版可互换，数据通用） |
| 适用场景 | 长期固定使用 | 移动盘/多机器/临时使用 |

## FAQ

**为什么需要管理员权限？**
TSF 输入法注册表只认 HKLM（机器级，与搜狗/微软拼音一致），注册与反注册需一次 UAC；
日常使用无需管理员。

**文件夹能移动吗？**
能。移动后重新运行一次 `install-portable.ps1`（注册表指向新路径）。

**用户数据在哪？**
`%AppData%\NovaInput`（`userdict.txt` 用户词库 / `engine.conf` 引擎开关 / `config.json` 候选窗配置）。
与安装包共用同一数据目录，便携版与安装包可无缝互换。

**杀毒软件提示 SampleIME.dll？**
TSF 输入法需注入宿主进程（所有输入法相同），个别杀毒可能标记。
确认来源后可对本文件夹添加信任。

**候选窗没出现 / 只能打英文？**
候选窗服务未运行：托盘无 Nova 图标时，双击 `server\PinyinPlus.Server.exe`
（必须以普通用户身份启动——提权启动会使引擎管道为 High 完整性，
普通应用里的 DLL 连不上，只能打英文）。

**和安装包的输入法冲突吗？**
同一 CLSID 只能指向一份 DLL：后注册者覆盖前者。
先卸载/反注册另一份（安装包的卸载或 `uninstall-portable.ps1`），再装目标版本。
