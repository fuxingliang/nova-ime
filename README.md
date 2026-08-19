# Nova 输入法（Pinyin-Plus）
[![GitHub Release](https://img.shields.io/github/v/release/fuxingliang/nova-ime?include_prereleases&label=release&color=4A6FA5)](https://github.com/fuxingliang/nova-ime/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](./LICENSE)

> 基于 Windows **TSF 框架**的自研拼音输入法，**Nova 系列**（与 Nova Studio / Nova Browser 统一）。
> 沙箱式三进程架构，宿主零重启，装完即用。

## Download

> **Windows 安装包（推荐）：**
> [NovaInput-Setup-1.0.2.exe](https://github.com/fuxingliang/nova-ime/releases/download/v1.0.2/NovaInput-Setup-1.0.2.exe)
> （一键安装：TSF 注册 + 激活默认 + 启动候选窗服务）
>
> **Windows 便携版（无需安装）：**
> [NovaInput-v1.0.2-windows-portable.zip](https://github.com/fuxingliang/nova-ime/releases/download/v1.0.2/NovaInput-v1.0.2-windows-portable.zip)
> （~110 MB — 解压后右键 `install-portable.ps1` 用 PowerShell 运行）
>
> [便携版包内说明](./docs/portable-release-quickstart.md) · [全部 Release](https://github.com/fuxingliang/nova-ime/releases)

## Status

[`v1.0.2`](https://github.com/fuxingliang/nova-ime/releases/tag/v1.0.2) — rime-ice 大词库（88 万条）+ 设置面板 + 便携版

What is already working well:

- 完整输入体验：全拼 / 简拼 / 混合输入 / 整句预测 / 模糊音 / 自学习
- 候选窗多屏 / 混合 DPI 精确定位（抖动抑制，根治"跳舞"）
- QQ（NT/Electron）兼容（自动切"写拼音组合"兼容模式）
- 沙箱三进程互相守护，升级/热更宿主零重启
- 设置面板：候选数 / 字号 / 自学习开关 / 大字库模式 / 词库导入导出

What is still evolving:

- 数字混输（声调）：调研后暂缓（与 1-9 选字键冲突），留作可选开关
- 发布体验：Release 产物上传与自动打包
- 高级功能：用户词删除入口扩展、造词流程打磨

## 特性 / Features

- **沙箱式架构**：纯 TSF 壳 DLL（无词库/引擎逻辑）+ 独立引擎进程 + WPF 候选窗服务，三方互相守护，任一崩溃秒级拉起
- **宿主零重启**：部署 / 热更无需重启电脑、注销或重启宿主应用
- **模糊音**：声母任意位置替换（zh/z、ch/c、sh/s、l/n、f/h、r/l）+ 韵母词尾替换（防破坏音节边界）
- **简拼增强**：音节数完全匹配优先、组内按词频降序，长词简拼不再挤掉精确匹配
- **整句预测** + 用户词库污染防护（简拼/畸形词条不参与整句预测）
- **搜狗式隐藏拼音**：拼音不写入编辑区、仅候选窗显示；QQ（NT/Electron）自动切兼容模式
- **自学习**：词频实时热加载，无需重启引擎
- **词库导入/导出**：用户词库合并去重 + 管道消息热重载
- **删除用户词**：候选高亮时 `Ctrl+Delete`（只删用户造词，词库词/符号受保护）
- **多屏 / 混合 DPI 感知**：候选窗按光标所在屏 DPI 精确定位，抖动抑制根治"跳舞"
- **rime-ice 词库**：三源合并（常用/核心/扩展，88 万条）+ 繁体过滤 + 可选大字库模式（生僻字：龘→da、靁→lei、𠀀→he）
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

## 便携版 / Portable（无需安装）

> 解压 → 右键 `install-portable.ps1` 用 PowerShell 运行（UAC 一次）→ 即用。
> 文件放在你解压的目录（可移动/删除）；用户数据在 `%AppData%\NovaInput`（卸载保留）。

- 包内自带 `install-portable.ps1` / `uninstall-portable.ps1`（一键注册/卸载，等价安装包的注册 + 激活 + 自启动 + 启动服务）
- 与安装包布局一致（Server 在 `<根>\server\`），两版可互换、数据通用
- 详见 [便携版快速上手](./docs/portable-release-quickstart.md)

## 使用 / Usage

- **切换**：`Win + Space` 或 `Alt + Shift`；装完自动激活进列表
- **添加到语言列表**（若未自动出现）：设置 → 时间和语言 → 语言和区域 → 中文 → 键盘 → 添加键盘 → **[Nova 输入法]**
- **设置面板**：托盘 Nova 图标双击打开（每页候选数 5-12 / 字号 12-22 / 自学习开关 / 大字库模式 / 词库导入导出，均热生效）
- **删除用户词**：候选高亮时 `Ctrl + Delete`
- **开机自启动**：登录自动拉起候选窗服务（`HKCU\...\Run`）

## 目录结构 / Repository Layout

```text
pinyin-plus/
├── src/
│   ├── ime/        # C++ IME（TSF 壳 DLL，基于微软 SampleIME）+ 运行词库
│   ├── engine/     # C++ 引擎进程
│   ├── server/     # C# WPF 候选窗服务（.NET）
│   └── activate/   # 激活工具（ImeActivate）
├── installer/      # Inno Setup 6 安装脚本（NovaInput.iss）+ 便携版（package_portable.ps1 / portable/）
├── tools/          # 造词脚本（Python）+ 部署/注册脚本（PowerShell）
├── docs/           # 开发记录（DEVELOPMENT.md）+ 便携版快速上手
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
# 5) 便携版打包（可选）
powershell -ExecutionPolicy Bypass -File installer/package_portable.ps1
```

产物：

- 安装包：`dist/NovaInput-Setup-<版本>.exe`（装完即用）
- 便携版：`dist/NovaInput-v<版本>-windows-portable.zip`（解压即用，见 [便携版快速上手](./docs/portable-release-quickstart.md)）

## 安装 / Install

**安装包**：运行 `NovaInput-Setup-<版本>.exe`：

- 安装目录 `%LocalAppData%\NovaInput`
- 用户数据 `%AppData%\NovaInput`（`engine.conf` / `userdict.txt` / `config.json`，卸载时保留）
- 装完自动激活 + 设为默认 + 启动候选窗服务

**便携版**：解压 → 右键 `install-portable.ps1` 用 PowerShell 运行 → `Win+Space` 切换即用（详见 [便携版快速上手](./docs/portable-release-quickstart.md)）。

## FAQ

**升级后为什么不用重启？**
升级用版本化 DLL 文件名（`SampleIME_v<版本>.dll`）+ 注册表指向新版：新进程装完即用新版，
旧进程继续用已加载的旧 DLL；引擎/Server 由看门狗自动拉起新版。

**候选窗没出现 / 只能打英文？**
候选窗服务未以普通用户完整性运行（提权启动会使引擎管道 High 完整性，普通应用连不上）。
托盘无 Nova 图标时，以普通用户双击 `server\PinyinPlus.Server.exe`。

**QQ 里为什么行为不同？**
QQ（NT/Chromium）对"隐藏拼音"不兼容，自动切"写拼音组合"兼容模式（与搜狗一致），
其余应用保持隐藏拼音。

**杀毒软件提示 SampleIME.dll？**
TSF 输入法需注入宿主进程（所有输入法相同），个别杀毒可能标记，确认来源后可添加信任。

**怎么卸载？**
- 安装包：设置 → 应用 → Nova 输入法 → 卸载（用户数据保留）
- 便携版：右键 `uninstall-portable.ps1` 用 PowerShell 运行 → 删除文件夹（用户数据保留）

**为什么安装需要管理员？**
TSF 输入法注册写 HKLM（机器级，TSF 枚举只读 HKLM，与搜狗/微软拼音一致），只需一次 UAC；
日常使用无需管理员。

## 开发记录 / Development

架构细节、已完成功能、踩坑与关键决策详见 [docs/DEVELOPMENT.md](./docs/DEVELOPMENT.md)。

## Feedback

问题与反馈请提 [GitHub Issue](https://github.com/fuxingliang/nova-ime/issues)；
真实输入场景的反馈（尤其是 QQ / 浏览器 / IDE 中的表现）最有价值。

## License

[MIT](./LICENSE)
