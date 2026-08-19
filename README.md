# Nova 输入法（Pinyin-Plus）
[![GitHub Release](https://img.shields.io/github/v/release/fuxingliang/nova-ime?include_prereleases&label=release&color=4A6FA5)](https://github.com/fuxingliang/nova-ime/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](./LICENSE)

> 基于 Windows **TSF 框架**的自研拼音输入法，**Nova 系列**（与 Nova Studio / Nova Browser 统一）。
> 沙箱式三进程架构，宿主零重启，装完即用。

## 特性 / Features

- **沙箱式架构**：纯 TSF 壳 DLL（无词库/引擎逻辑）+ 独立引擎进程 + WPF 候选窗服务，三方互相守护，任一崩溃秒级拉起
- **宿主零重启**：部署 / 热更无需重启电脑、注销或重启宿主应用
- **模糊音**：声母任意位置替换（zh/z、ch/c、sh/s、l/n、f/h、r/l）+ 韵母词尾替换（防破坏音节边界）
- **简拼增强**：音节数完全匹配优先、组内按词频降序，长词简拼不再挤掉精确匹配
- **整句预测** + 用户词库污染防护（简拼/畸形词条不参与整句预测）
- **搜狗式隐藏拼音**：拼音不写入编辑区、仅候选窗显示；QQ（NT/Electron）自动切兼容模式
- **自学习**：词频实时热加载，无需重启引擎
- **词库导入/导出**：用户词库合并去重 + 管道消息热重载
- **多屏 / 混合 DPI 感知**：候选窗按光标所在屏 DPI 精确定位，抖动抑制根治"跳舞"
- **rime-ice 词库**：三源合并（常用/核心/扩展）+ 繁体过滤
- **Nova 品牌化**：候选窗 Nova 皮肤、托盘、设置面板

## 架构 / Architecture

```text
宿主应用（记事本 / QQ / Trae …）
   │  TSF 进程内注入
   ▼
SampleIME.dll          纯 TSF 壳（无词库/引擎，约几十 KB）
   │  命名管道 PinyinPlus.Engine（PPIM 帧协议）
   ▼
PinyinPlus.Engine.exe  独立引擎：词库 / 音节切分 / 候选 / 整句预测 / 自学习 / 造词
   │  命名管道 PinyinPlus.Service
   ▼
PinyinPlus.Server.exe  WPF 候选窗 + 托盘 + 设置面板 + 引擎看门狗
```

三进程互相守护：Server 看护引擎、引擎 `ServerWatchdogProc` 看护 Server、DLL 保活线程兜底。

## 目录结构 / Repository Layout

```text
pinyin-plus/
├── src/
│   ├── ime/        # C++ IME（TSF 壳 DLL，基于微软 SampleIME）+ 运行词库
│   ├── engine/     # C++ 引擎进程
│   ├── server/     # C# WPF 候选窗服务（.NET）
│   └── activate/   # 激活工具（ImeActivate）
├── installer/      # Inno Setup 6 安装脚本（NovaInput.iss）
├── tools/          # 造词脚本（Python）+ 部署/注册脚本（PowerShell）
├── docs/           # 开发记录（DEVELOPMENT.md）
└── config.json     # 运行配置（PageSize 等）
```

## 技术栈 / Tech Stack

- C++（Visual Studio / MSVC，x64）
- C# / .NET（WPF 候选窗服务）
- Inno Setup 6（安装包）
- Python（词库构建 `build_dict.py`）
- 词库：rime-ice（LGPL-3.0）+ OpenCC 繁体映射（Apache-2.0）

## 运行要求 / Requirements

- Windows 10 / 11（x64）
- 构建 C++：Visual Studio（MSVC）
- 构建 Server：.NET（C# / WPF）
- 构建安装包：Inno Setup 6
- 词库数据：`tools/download_rime_dicts.ps1` 下载（数据不入库）

## 构建 / Build

```powershell
# 1) 下载词库数据源（rime-ice + OpenCC）
powershell -ExecutionPolicy Bypass -File tools/download_rime_dicts.ps1
# 2) 造词
python tools/build_dict.py
# 3) 编译 IME / 引擎（Visual Studio 打开 src/ime/SampleIME.sln 等）+ 编译 Server（.NET）
# 4) 打安装包
powershell -ExecutionPolicy Bypass -File installer/build_installer.ps1
```

产物：`dist/NovaInput-Setup-<版本>.exe`（装完即用）。

## 安装 / Install

运行 `NovaInput-Setup-<版本>.exe`：

- 安装目录 `%LocalAppData%\NovaInput`
- 用户数据 `%AppData%\NovaInput`（`engine.conf` / `userdict.txt` / `config.json`，卸载时保留）
- 装完自动激活 + 设为默认 + 启动候选窗服务

## 开发记录 / Development

架构细节、已完成功能、踩坑与关键决策详见 [docs/DEVELOPMENT.md](./docs/DEVELOPMENT.md)。

## License

[MIT](./LICENSE)
