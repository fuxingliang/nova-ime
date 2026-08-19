# Nova 输入法（Pinyin-Plus）开发记录

> 项目路径：`g:\pinyin-plus`　品牌：**Nova 系列**（与 G:\vllm_5090D、G:\Nova 统一）
> 本文档记录架构、已完成功能、待办与关键决策，防止开发上下文丢失。

---

## 一、总体架构（沙箱式，宿主零重启）

三进程协作，互相守护，任何一方崩溃/被杀都由另一方秒级拉起：

```
宿主应用（Trae/记事本等）
   │  TSF 进程内注入
   ▼
SampleIME.dll（纯 TSF 壳，无词库/无引擎逻辑，约几十 KB）
   │  命名管道 PinyinPlus.Engine（PPIM 帧协议，请求-响应）
   ▼
PinyinPlus.Engine.exe（独立引擎进程：词库/音节切分/候选/整句预测/自学习/造词）
   │  命名管道 PinyinPlus.Service（Server 提供，UI 数据）
   ▼
PinyinPlus.Server.exe（WPF 候选窗 + 托盘 + 设置面板 + 引擎看门狗）
```

- **PPIM 帧协议**：`[u32 magic=0x5050494D][u32 version=1][u32 type][u32 payloadLen][payload]`
- **互相守护闭环**：
  - Server 内部定时器看护引擎（消失 1 秒内拉起）
  - 引擎内 `ServerWatchdogProc` 看护 Server（每秒枚举进程，消失 2 秒内拉起）
  - DLL 内 `KeepAliveThreadProc`（300ms 轮询）兜底拉起引擎 + 重连管道
- **引擎单实例**：`CreateMutexW(TRUE)` 持所有权；`WAIT_TIMEOUT` 退出、`WAIT_ABANDONED` 接管（崩溃自愈）
- **引擎请求串行化**：每连接一线程，全部请求处理加 `CRITICAL_SECTION`（防 BoostWord/查询并发数据竞争崩溃）
- **DLL 管道句柄唯一关闭方**：ReadLoop 同步读 + 自身 CloseHandle；StopReadThread 只 CancelIoEx；WriteAll 失败只复位标记（消除句柄并发关闭竞态 → 宿主崩溃根因）
- **部署无需重启电脑/注销**：DLL 用带时间戳文件名（`PinyinPlusIme-<时间>.dll`），注册指向新文件；引擎/Server 热更由看门狗自动拉起

## 二、已完成功能

### 阶段 0：架构（沙箱 + 纯壳 + 守护）✅
- 瘦 DLL 纯壳：词库/引擎逻辑全部外置引擎进程
- 引擎互斥锁串行化（修复候选窗不显示/卡顿——引擎崩溃根因）
- 单实例互斥体修复（`CreateMutexW` 必须持所有权）
- 后台保活线程（修复热更新导致 Trae 失去响应——TSF 线程零阻塞）
- 句柄唯一关闭方模式（修复热更新 Trae 崩溃——句柄并发关闭竞态）
- 引擎兼任 Server 看门狗（修复 Server 被杀后无人守护）
- 互相守护闭环验证：杀 Server → 引擎 2 秒拉起 → 宿主存活

### 阶段 1：候选窗视觉与交互 ✅
- 滚轮翻页（上滚=上一页、下滚=下一页，翻页高亮第一项）
- 多屏感知定位（目标屏幕 WorkArea 约束）
- **候选窗定位模块** `src/server/CandidatePlacement.cs`：
  - Win32 `GetDpiForMonitor` 取光标所在屏 DPI 做 物理→DIP 换算（不依赖窗口渲染）
  - 多屏/混合 DPI 独立约束
  - 抖动抑制：位置更新距离 < 5 DIP 不移动（吸收 GetTextExt 1-3px 抖动，根治"跳舞"）
- Server Per-Monitor V2 DPI 感知（`app.manifest`）
- Server 单实例：命名 Mutex `Local\PinyinPlus.Server.SingleInstance`（替换有 TOCTOU 竞态的进程枚举；abandoned 接管）

### 阶段 2：输入功能增强 ✅
- **模糊音**（`CollectWordFuzzy`）：声母类任意位置替换（zh/z、ch/c、sh/s、l/n、f/h、r/l）、韵母类仅词尾替换（finalOnly，防破坏音节边界）
- **简拼增强**（`CollectWordByInitial` 排序优化）：
  - 修复截断 bug：收集上限放宽到 500 再排序，音节数完全匹配的词不被长词简拼前缀挤掉
  - 排序：音节数越接近输入串越靠前，组内按词频降序
- **整句预测污染防护**（`IsValidFullPinyin`）：用户词库的简拼/畸形词条（如 z→钟婷、zt→钟婷天气）不得参与整句预测，否则拼出怪异整句（zting → "钟婷听"）
- **搜狗式隐藏拼音**（拼音不写入编辑区，只在候选窗）：
  - `_SetInputString` 增加 `writeToComposition` 参数：拼音输入 FALSE（组合保持空）、选字/造词/标点 TRUE（正常上屏）
  - `_GetTextExt` 空组合回退：用插入符（selection）位置定位候选窗
  - **QQ 进程感知**（`HostNeedsCompositionText`）：QQ(NT/Chromium) 用"写拼音组合"兼容模式（与搜狗一致），其余应用保持隐藏拼音
  - **英文输入兜底（隐藏拼音的配套修复）**：隐藏拼音后组合为空，按回车/空格时"上屏拼音原文"失去载体，输入 hello 等非拼音串完全丢失。修复两处：
    - `GetCandidateList`：引擎无候选时（如 hello）把原文作为唯一候选显示在候选窗（对齐搜狗）
    - `_HandleCompositionFinalize`：组合文本为空且非造词模式时，把键盘缓冲原文写入组合再提交（回车上屏原文）
  - **引擎冷启动快速失败**：`EngineClient` 记录管道连接时刻，连接 <3 秒（引擎加载 50MB 词库窗口）内击键超时 300ms→30ms，避免部署/重启时每次击键阻塞宿主 TSF 线程（Chromium 叠加渲染卡顿会表现为应用无响应）
- **QQ 兼容崩溃修复（重要踩坑）**：
  - 现象：QQ(NT/Electron) 中文输入崩溃，微信/记事本正常
  - dump 分析：异常 0xC0000005 空指针，崩溃指令在 QQ 自带 MSVCP140.dll（14.29）的 iostream/locale vtable 解引用（`mov rax,[rax]`）
  - 根因：DLL 的 `DebugLog`/`ClientLog` 用 `fwprintf` 写日志——依赖 CRT locale/facet，在 QQ 的 TSF 线程上被调用时触发宿主 MSVCP140 的 locale 代码空指针崩溃
  - 修复：日志全部改为纯 Win32 `CreateFileW`+`WriteFile`（UTF-8 追加），去掉 CRT locale/fwprintf/static mutex 依赖
  - 教训：**进程内 DLL 的日志/格式化不得用 CRT iostream/fwprintf**（跨模块 locale 竞态），统一用 Win32 API
- **崩溃 dump 分析工具**：`tools/parse-dump.ps1`（解析 minidump：异常码/寄存器/模块/崩溃线程栈扫描）+ dbh 符号化 + dumpbin 反汇编，定位宿主崩溃与 DLL 的调用关系
- **数字混输（声调）**：调研后暂缓——与 1-9 选字键冲突、实用性存疑，留待阶段 3 做成可选开关

### 阶段 3：设置面板（进行中）✅ 大部分完成
- **Nova 品牌化**：Server exe 图标/托盘 = Nova.ico（复制自 G:\Nova）；候选窗 Nova 皮肤（深夜蓝灰渐变 #0C1117→#172330、青 #55D8FF 高亮、拼音行带 N 标识）；设置窗口 Nova 品牌
- **设置面板** `SettingsWindow.xaml`（托盘双击/右键打开）：
  - 每页候选数 5-12（config.json `PageSize`，热生效）
  - 候选字大小 12-22（config.json `CandidateFontSize`，热生效）
  - **自学习开关**：写入 `bin\engine.conf` `learn=0/1`，引擎每次 BoostWord/AddUserWord 实时读文件（无需重启引擎）
  - **词库导入/导出**：导出 = 复制 `bin\userdict.txt`；导入 = 合并去重（同 pinyin+word 累加词频）+ 引擎管道消息 type 11 → `ReloadAll()` 热重载（主词库+用户词库重建，锁内执行）
- 引擎管道客户端 `EnginePipeClient.cs`（PPIM 帧，Server 侧发送 type 11）
- **Server 全局异常兜底**（`App.xaml.cs`）：`DispatcherUnhandledException` 标记已处理防进程退出、`AppDomain`/`TaskScheduler` 异常记录到 `tools/app_trace.log`——此前 Server 出现两次未处理异常静默崩溃（dump 0xE0434352），候选窗消失

### 阶段 4：词库替换 rime-ice + 造词/重载优化 ✅（2026-08-13）
- **词库替换为 rime-ice**（调研结论落地）：`tools/build_dict.py` 改为解析 rime 格式（`词\t拼音\t权重`），三源合并（8105 常用层 + base 核心 + ext 扩展），过滤层：仅 CJK 基本区、词长 1-10、繁体兜底（OpenCC TSCharacters key 侧）
  - 结果：15.4 万 → **88.1 万条 / 29.0 MB**（+12 倍），扩展区生僻字 11.4%→0、繁体残留 0、词频真实（人名地名如"朱镕基"可达）
  - **繁体过滤关键修复（用户报 bug"qian 找不到乾"）**：OpenCC TSCharacters key 侧含**繁简共用字**（么/像/坏/乾/覆/沈 等，value 含自己，如 `乾\t干 乾`）——只有 value 不含 key 的才是纯繁体。修正后恢复 1.2 万词条，乾/乾坤/乾隆 ✓ 可达
- **打字卡顿根治（148 倍提速）**：性能测试定位热点 → 简拼索引 `_initialEntries` 改存下标（防 realloc 悬垂）+ 二分替代遍历区间；CollectWordByMixed 先贪心快速校验、失败且首音节匹配才全切分重试；SearchPrefix 分组截断（GROUP_TOP=40/COLLECT_CAP=4000）；热路径 DebugLog 全删。fuxl 132ms→1.7ms、全拼 <0.1ms、单音节 0.4ms
- **任务 1：造词局部重排**：AddUserWord 简拼索引局部插入 + FixInitialIndexFreqOrder 局部重排（造词 1.5-3s → 50ms，BoostWord 10.7ms）；AddUserWord 内纠正旧畸形简拼（gaap→gggp）
- **任务 2：造词全切分+打分**：`SegmentToSyllablesBest` 枚举全部完整音节切分 + 打分（每多 1 音节扣 10 + 零声母扣 12 + 单字词频 log 加分）。ganganganpin→[gan,gan,gan,pin] ✓、zhongguo→[zhong,guo] ✓（原贪心切 [zhong,gu,o] 造不出词）
- **任务 3：双缓冲热重载**：`g_engine`（当前）+ `g_reloadEngine`（后台重建）双实例，type 11 触发后台线程 `Initialize` 后锁内原子切换——导入词库/切换模式时打字**零阻塞**（原锁内同步 ReloadAll 需 7.5s 全卡）
- **引擎连接风暴崩溃防御**（三层）：
  - 引擎锁初始化竞态 → `std::call_once` 预初始化（连接风暴下多线程首次并发 `InitializeCriticalSection` 损坏锁 → 引擎假死 err=109）
  - 连接线程 SEH 兜底 + 并发限流 32（线程爆炸防护）+ 存活 tick 日志
  - **EngineLog 日志锁同款竞态**（`s_cs` 非原子初始化）→ 同步修复为 `std::call_once`
- **大字库模式开关（设置面板）**：
  - `build_dict.py --big`：+41448 大字表（4 万+ 生僻字），放宽到 CJK 扩展区，生僻字低词频（0.5）自动殿后；输出 `pinyin-plus-big.txt`（916,511 条）
  - 引擎 `ResolveDictPath()` 读 `bin\engine.conf` 的 `bigdict=0/1` 选词库（启动 + 热重载都经它）
  - 设置面板「数据与学习」新增「大字库模式」勾选框（engine.conf 统一读写 learn/bigdict），切换时 type 11 热重载换词库（双缓冲零阻塞）
  - 龘→da、靁→lei、𠀀→he、㵘→man 大字库全可达；默认词库不含（防刷屏）
  - ⚠️ **验证受挫记录**：trae-sandbox 命令环境对命名管道**单向转发**（客户端→引擎通、引擎→客户端响应读不到），沙箱内 ctypes/.NET/C++ 测试均无法读到引擎响应；引擎日志证实 `got request → done in 0ms`（引擎本身正常）。**功能验证需在真实打字环境进行**
- **删除用户词（候选模式 Ctrl+Delete）✅（2026-08-13，用户造词可能造错的需求）**：
  - **按键链路**：`IsVirtualKeyNeed` 开头拦截 `Ctrl+Delete`（`GetKeyState(VK_CONTROL)&0x8000` + 候选模式 ∈{ORIGINAL,INCREMENTAL,PHRASE,WITH_NEXT_COMPOSITION}）→ `FUNCTION_DELETE_USER_WORD` → `_HandleCandidateDeleteUserWord` 取高亮候选 → `CEngineClient::DeleteUserWord` → 管道 type 13 → 引擎 `DeleteUserWordByWord` → 重写 userdict.txt → `_HandleCompositionConvert` 刷新候选；普通 Delete 不吞
  - **Entry.isUser 标记**（核心）：「删哪个」判定标准：主词库条目 false、用户词库独有条目 true、AddUserWord 新词 true；删除只删 `isUser && word==wd`，清 `_userFreq`/`_userInitial` + `RebuildInitialIndex()` + 落盘
  - **误删词库符号 △ 修复（用户报"删了傅兴亮 fxl 没了但 fuxingliang 还在"排查发现）**：原删除逻辑按 `_userFreq` 有记录判定"用户词"，但 **BoostWord 会把词库词/符号（如 △）提升进 `_userFreq`**——导致删的是 △/发现了/泥十个忍 等词库词而非傅兴亮；`isUser` 位从根上区分"用户造词"与"被 boost 的词库词"后：△ 等词库词/符号不可删（保护），傅兴亮（isUser=true）一次性清掉全部 5 个拼音入口（f/fux/fuxingliang/fxl/fxliang）
  - 验证：删傅兴亮 → fuxingliang/fxl 全消失；sanjiao → △ 仍在（删不掉，正确）
  - 存储评估结论：userdict 全量重写毫秒级，txt 增删无痛点；真痛点是主词库 88 万条冷启动（**已解决**：见下方"稳定加固"）
- **新词三输入支持（全拼/简拼/混合）✅（2026-08-13，用户反馈"造词后不全支持各种拼音输入"）**：
  - **根因 1（自学习新词不入库）**：`BoostWord` 对词库不存在的词（组词/人名兜底产物，如"傅兴亮"）原实现只加 `_userFreq` 落盘 **3 列（无简拼）**，不真正插入 `_entries` → 本次会话全拼仍靠组词兜底、重启后简拼/混合查不到。修复：与 `AddUserWord` 一致立即入库（算简拼 → 插 `_entries`(isUser=true) → `InsertIntoInitialIndex` → 落盘带第 4 列简拼），三种输入**当场可用**
  - **根因 2（存量 3 列词条无简拼）**：新增 `FillMissingInitials()`——ReloadAll 在音节表构建后、简拼索引前，对 initial 为空且 `IsValidFullPinyin` 的词条统一补算简拼（旧 3 列 userdict 遗留 152 条自动补齐，重启即生效）
  - **根因 3（用户词被词库高频词压出候选）**：简拼/混合匹配组内按词频降序，用户词"傅兴亮"(freq=2) 被主词库高频词（放下了/放心了/复兴路，freq 数十上百）压到 50 名之外 → fxl/fuxl 打不出来。修复：`CollectWordByInitial`/`CollectWordByMixed` 排序加 **isUser 用户词优先**（用户词 > 符号 > 词频）
  - **根因 4（★最隐蔽：造词后简拼索引整体错位）**：`_initialEntries` 存下标，但 `AddUserWord`/`BoostWord` 用 `_entries.insert()` 在**中间**插入新词后，插入点之后所有元素下标 +1，索引里的旧下标**未同步平移** → f 区插入破坏 f 区之后全部简拼索引。用户"造完词立即输 fxl"查不到傅兴亮（引擎日志证实 AddUserWord py=fuxingliang initial=fxl 入库正确，纯粹是索引错位）。修复：insert 后遍历 `_initialEntries`，所有 `>= entryIdx` 的下标 +1 平移（排序不变，~1ms）。**教训**：下标索引的"局部维护"必须处理 insert 平移与 sort 交换，缺一即整体错位；此前注释"存下标不受 realloc 影响"误以为 insert 安全
  - 验证（engine_test 进程内，加载 bin 词库+userdict）：加载后 fxl→傅兴亮第 1 位；再造词（found 分支）+ 新词插入（fuxiyang 触发 insert 平移）后，fxl/fuxl 仍首位可达傅兴亮、新词 fuxy 简拼直达；sjx→△ 符号优先不受影响
  - **调试教训**：engine_test 复刻 QueryCandidates 时 `CStringRange` 必须 `Set` 初始化（未初始化 → key 空 → 简拼/混合静默返回 0，一度误判为引擎 bug；真实打字走引擎 `QueryCandidates` 的 `range.Set` 正常，问题其实在排序）
- **稳定加固三项 ✅（2026-08-13，用户诉求"稳定可靠，兼顾速度"）**：
  - **① 主词库二进制预索引（冷启动 5-10s → <1s）**：
    - 引擎首次解析 txt 后写 `<dict>.bin` 缓存（header 含 txt size+mtime 校验 + 排序后的全部 Entry）；后续启动直接读入，跳过 29MB 文本解析与 88 万条排序
    - 缓存格式：`u32 magic | u32 ver | u64 txtSize | u64 txtMtime | u32 count` + 每条 `len+pinyin(UTF-16) | len+word | len+initial | f32 freq | u8 isUser`
    - **★踩坑**：header 用 struct 整块写会因 u64 对齐引入 4 字节填充（sizeof=32），读端按 28 字节解析导致指针错位 → 缓存永远校验失败静默回退 txt 解析。修复：header 逐字段写（28 字节）读写严格一致
    - **性能关键**：逐条 ReadFile/WriteFile 是灾难（88 万 × 7 次 = 616 万次系统调用，读缓存 21.9s）；改为**单次大块读/写 + 内存解析**（读 1.5s → 218ms）
    - 实测（vLLM 满载环境）：缓存 HIT 总加载 **0.9-1.2s**；txt 解析路径（首次/缓存失效）也因写缓存单次大写降到 **2.3s**（原 5-10s 空闲 / 18.8s 满载）
  - **② 简拼索引自检兜底（ValidateInitialIndex）**：ReloadAll 及 AddUserWord/BoostWord/DeleteUserWordByWord 后校验 `_initialEntries`（下标越界/initial 空/排序错乱 → 自动 RebuildInitialIndex 重建）——根因 4 类"局部维护漏步"从此有兜底，索引永不错位
  - **③ userdict 加载校验（IsValidUserRow）**：pinyin 仅小写字母 1-30、word 1-20 且含非 ASCII（汉字/符号/emoji）、freq>0；畸形行跳过 + 日志（当前 userdict 过滤 20 条历史垃圾词条，SaveUserDict 写盘时自动清除）
  - **附带优化（LoadUserDict 合并 6.4s → 0.15s）**：合并查找从**线性扫描 88 万 × 用户词数**（7.9 亿次比较）改**二分定位拼音区间**；段排序改 `inplace_merge` 归并（主词库段已有序，仅用户词段排序后线性归并，替代 88 万全量 sort）
  - 验证：engine_test 回归全部通过（fxl/fuxl/sjx 可达、造词后索引正确）；真实引擎部署后缓存 HIT 加载 1.2s、entries=881402
- **候选窗标记用户自定义词 ★ 角标 ✅（2026-08-13，用户诉求"让用户知道候选里哪些是系统的、哪些是自己定义的"）**：
  - **方案：Server 本地比对 userdict.txt（不动协议）**。`Entry.isUser` 标志在候选链路（DLL CCandidateListItem → Show 帧 → Server）中丢失，但引擎每次用户词变更（BoostWord/AddUserWord/删词）都**立即落盘 userdict.txt**，且该文件恰为全部 isUser=true 词条 → Server 端 `UserDictIndex.cs` 按 word 比对即可等效判定，零协议改动、无兼容风险
  - 实现：`UserDictIndex` 读 userdict.txt 建 HashSet（**mtime+length 变化才重读**，命中微秒级）；候选词命中 → ViewModel `IsUser` → 词后金色半透明 **★ 星标**（国际通用"自定义/收藏"符号，U+2605 普通字体渲染稳定，不依赖 emoji 彩色）；**图例说明已删**（用户反馈省地方），悬停 ★ 有 ToolTip"自定义词"
  - **★踩坑：路径层级**。初版用"BaseDirectory 上 3 级 = 项目根"（以为与 AppConfig 同基准）→ 实际 `src\server\bin\Release\net9.0-windows` 上 3 级是 `src\server`（项目根需上 5 级）→ userdict 找不到、集合空、角标全不显示（用户 OCR 截图实测无标记才暴露）。修复：`LocateUserDict()` **从 Server 目录向上逐级搜索** userdict.txt（本目录或 `bin\` 下，最多 8 级），开发/便携形态都覆盖，不依赖层级
  - 语义边界：符号词条（△、😄）不在 userdict 不误标；词库已有词被 Boost 提升不算"自定义"（不落盘 userdict）
  - 附带结论：WPF 文本渲染不支持彩色 emoji（COLR），候选窗 emoji 保持黑白（用户选定，零改动）
- **鼠标 × 候选窗交互 ✅（2026-08-13）**：
  - 现状盘点：左键单击上屏（Server type 5 → `PinyinIpc.cpp` ReadLoop → `CCandidateSelectEditSession` → `_HandleCandidateSelectByGlobalIndex`，链路完整）✅、滚轮翻页 ✅、跟随光标定位 ✅
  - **★踩坑：点击候选窗导致候选窗消失（点击不上屏的根因）**。WPF 窗口即使 `ShowActivated=False`，鼠标点击仍会让 Windows 把前台切到候选窗 → 编辑器失焦 → TSF 组合被取消 → DLL 发 Hide → 候选窗消失（MouseUp 都没触发，日志无记录）。修复：候选窗设 **WS_EX_NOACTIVATE** + 拦截 `WM_MOUSEACTIVATE` 返回 `MA_NOACTIVATE` 双保险（SourceInitialized 时 SetWindowLongPtrW + AddHook）——候选窗/符号面板永不被激活，编辑器始终保焦
  - **悬停高亮**：候选项 Border `MouseEnter/MouseLeave` → ViewModel `IsHovered` → MultiDataTrigger（IsHovered && !IsSelected）淡蓝底 #26384C + 柔青字 #9BE0FF；与键盘选中态（深蓝底 #1E2A38 青字加粗）互不干扰——**选中态优先**
  - **翻页导航箭头**：候选窗底部 `◀ 1/6 ▶`（多页时显示），鼠标点击翻页（复用滚轮翻页逻辑）
  - **右键菜单（候选窗内嵌浮动 Border，不新建窗口 → 不抢焦点）**：复制该词（Clipboard）/ 必应搜索该词（Process.Start）/ 删除该词（仅 ★ 用户词显示）
  - **符号面板 ❖ 入口（候选窗底部常驻按钮）**：`SymbolWindow` 分类浏览（标点/数学/货币/箭头/图形/特殊/emoji，解析 `tools/data/symbols.txt`），点击符号插入编辑器光标处
- **Server→DLL 协议扩展 type 6/7 ✅（2026-08-13）**：
  - type 6 `DeleteUserWord`（右键删词）：Server `SendDeleteUserWord` → DLL ReadLoop 解析 UTF-8 字符串 → message-only 窗口 PostMessage（字符串 heap 分配，WndProc 接收后 delete）→ `_OnDeleteUserWordMessage` → `CCandidateTextCommandEditSession(DeleteUserWord)` → `CEngineClient::DeleteUserWord`（引擎 type 13）+ `_HandleCompositionConvert` 刷新候选
  - type 7 `InsertText`（符号上屏）：同上链路 → `CCandidateTextCommandEditSession(InsertText)` → **`ITfInsertAtSelection::InsertTextAtSelection`**（TSF 标准接口，不依赖组合状态；光标折叠到插入文本末尾）。符号面板为 WS_EX_NOACTIVATE 窗口，点符号时编辑器不失焦 → 插入可靠
  - 协议帧：payload = `[int32 len][UTF-8 bytes]`（`EncodeString` 统一编码，DLL 侧 `MultiByteToWideChar` 解码）
  - 未做（评估结论）：拖拽候选排序/置顶（引擎需新排序机制，高难度）
- **候选窗拖动定位（记忆偏移）✅（2026-08-13，用户诉求"游戏里候选窗位置不准，拖到自己喜欢的位置固定下来"）**：
  - **最终方案：纯光标拖动**。按住候选窗空白处（排除候选词/翻页/❖/右键菜单）→ 光标变十字（SizeAll）→ 原窗口**保持不动** → 松手后候选窗以鼠标为中心**一次性就位**，偏移持久化到 `tools/candidate_offset.json`（重启保留）；右键候选词 → "重置位置"清零回贴光标。位移 >6px 才算真拖动（点击空白只关菜单，不误移窗口）
  - **静止发抖根因链（多轮排查，教训完整）**：
    1. Server 每次 SetPosition 都 `File.AppendAllText` 同步写磁盘日志 → 高频消息阻塞 UI 线程（修：200ms 限流打日志）
    2. 候选内容变化 → `SizeToContent` 窗口宽度变化 → 屏幕边缘约束重算位置（贴右时 `left=屏宽-窗宽`）→ 位置跳变（修：抖动抑制**以 DLL 原始光标坐标**为基准，光标不动绝不重定位）
    3. 游戏 GetTextExt 坐标高频微跳 >5px（修：阈值提至 8px + **40ms 限频**，位置更新频率上限 ~25Hz）
    4. 直接拖动时 `AllowsTransparency` 分层窗口移动整窗重绘卡顿（修：**拖动不移动原窗口**，松手一次性就位——彻底绕开）
  - **幽灵卡方案（尝试后放弃）**：拖动时抓快照生成半透明幽灵窗口跟随鼠标（参考 Nova TabDragGhostWindow），原窗口不动、松手跳转——可彻底避免拖动抖动，但用户认为幽灵多余（"鼠标有十字架就够了"），且快照需处理尺寸/锚点细节。**最终采纳纯光标方案，删除 DragGhostWindow.cs**
  - 相关：DPI 坐标归一化（`_NormalizeToPhysical`，按窗口 DPI 感知度换算物理像素）、双链路坐标统一（GetTextExtentEditSession 改走 `_GetTextExt` 的 selection 回退）、GUI caret 兜底（GetGUIThreadInfo）、DLL 日志毫秒时间戳（与 Server 日志对齐）
- **繁体输出（OpenCC S2T）✅（2026-08-13，用户诉求"繁体输出"）**：
  - **方案：输出层转换 + 词库恒为简体**。词库（rime-ice）与引擎核心（PinyinEngine.cpp）零改动；转换在**引擎管道输出层**做——DLL 把引擎返回的 word 直接发 Server 显示 + 选字上屏，所以引擎侧转繁体 = 候选窗显示与上屏文本统一为繁体，DLL/Server 显示层零改动
  - **转换模块** `src/engine/TraditionalConvert.cpp/h`（全局，进程内加载一次）：OpenCC（BYVoid，Apache-2.0）官方字典 4 张（`bin\STPhrases/STCharacters/TSPhrases/TSCharacters.txt`，来自 GitHub raw，tools/data/ 存档、deploy.ps1 复制到 bin）
    - **S2T**（查询输出）：整词查 STPhrases（歧义来源："后面"→"後面"、"干活"→"幹活"、"干部"→"幹部"），未命中逐字查 STCharacters 首选值（"面"→"面"、"后"→"後"）
    - **T2S**（用户词回存）：繁体模式下选词/造词的候选是繁体，BoostWord/AddUserWord 入库前转回简体（"傅興亮"→"傅兴亮"）；DeleteUserWord 删词前同样 T2S 匹配——**词库恒为简体，切换简繁模式无繁体残留**
    - ★**踩坑（恒等条目）**：TSPhrases 用恒等条目做"整词不转换"防护（`乾隆\t乾隆`、`乾坤\t乾坤`），若解析时跳过恒等条目，T2S 逐字会把"乾隆"误转成"干隆"。**恒等条目必须保留**（命中整词返回原样）
  - **开关链路**：设置面板"数据与学习"→"繁体输出"勾选 → 写 `bin\engine.conf` `tradition=0/1` → 管道 **type 15 SetTradition**（新增，Server→引擎，payload 1 字节）即时生效（引擎无需重启）；引擎启动时读 engine.conf 做初始值。tradition=0 时转换零开销（直接返回原串）
  - 转换点覆盖：QueryCandidates（整句/前缀/简拼/混合/模糊/组词全路径）、QuerySyllableChars（造词单字）、type 9 ConvertedWildcard（文本造词反查）；英文/数字/符号（△、😄）无映射原样保留
  - 验证：PS 复刻转换算法 26 例全过（含歧义词/乾隆/乾坤/hello/△/傅興亮回存）；引擎日志确认表加载成功；管道直测在 sandbox 环境不可用（响应读不到，与既有记录一致）→ 真实打字待用户验证
  - 已知局限（OpenCC TSPhrases 表）：台湾词汇 T2S 仅字面转简（"軟體"→"软体"而非"软件"），只影响繁体模式造词回存场景，影响极小
  - **候选窗交互切换（2026-08-13 增强，用户诉求"右键切换 + 状态一目了然"）**：
    - 右键候选词菜单加"简体输出/繁体输出"两项（当前模式青色高亮）；候选窗底部状态条加「简/繁」徽标（点击同样切换）——切繁体后候选窗字即变繁体（引擎输出层保证），状态条让用户一目了然
    - Server 侧 `EngineConf.cs`（新增共享读写工具：learn/bigdict/tradition/punct，SettingsWindow 与候选窗共用，原子写防半截）；切换写 engine.conf + 引擎 type 15 即时生效
- **中英标点 + 全角/半角 ✅（2026-08-13，用户诉求"中文和英文标点符号的切换"）**：
  - **现状盘点**：DLL 本有 TSF 语言栏标点/全角按钮 + 保留键（compartment），但默认无候选窗交互入口；`Global::PunctuationTable`（14 个标点）+ `_PunctuationPair`（引号对）+ `_PunctuationNestPair`（书名号嵌套）
  - **两个独立功能（用户确认"两个都要"，候选窗状态条按钮）**：
    - **中英标点（punct）**：候选窗底部「，」/「,」按钮切换。重写完整中文标点表 `s_CnPunctTable`（28 项覆盖全部可打印 ASCII 标点：`-`→`－`、`/`→`／`、`^`→`……`、`_`→`——`、`&`→`＆`、`\`→`、`、`@`→`·`、`[`→`【`、`]`→`】` 等；修正原表 `&`→破折号、`^`→单个省略号的错误）；`"`/`'` 配对、`<`/`>` 书名号嵌套保留原对子逻辑。**GetPunctuation 改返回 std::wstring（支持多字符 ……/——）**；英文标点模式直出半角原键
     - **★踩坑（句号变"点"根因，2026-08-13 用户报"句号是个点不是圈"）**：KeyEventSink 标点分支原要求 `_candidateMode == CANDIDATE_NONE`——候选窗显示时（打字过程中）按 `.` 不进入标点处理，`.`/`,` 又不在翻页键（`-`/`=`）里 → 半角 `.` 直接透传给应用输出"点"。**修复：去掉 candidateMode 条件**，候选模式也拦截标点（`_HandleCompositionPunctuation` 本就实现了"先提交选中候选再上屏中文标点"的搜狗行为）。同时按用户习惯把 `[`/`]` 映射从全角方括号 ［］ 改为中文方括号 【】
     - **★踩坑2（全角模式短路中文标点，同日用户报"打不出中文标点"）**：engine.conf 被测试遗留成 `width=1`，而 `GetPunctuation` 把**全角化分支放最前**——全角模式下 `.` 返回全角句点 `．`（U+FF0E，像点）而非中文圈句号 `。`（U+3002），用户误以为中文标点失效。**修复：中文标点（punct）优先于全角化**（中文标点本身是全角，不受 width 影响）；同时 KeyEventSink 标点拦截改为由 engine.conf punct 决定而非 TSF 语言栏标点 compartment（用户可能无意按 Ctrl+. 关闭它导致全应用打不出中文标点）
      - **★踩坑3（`>` 不转 `》`，同日用户报" > 没有转换为 》"）**：书名号嵌套计数器 `_nestCount`（int）——单独按 `>`（未先按 `<`）时 `--0` 变 -1，永远归不了零 → 永远输出 `〉`。**修复：`>` 时若 `_nestCount <= 0` 直接输出 `》`**（不递减）；嵌套语义保留（`<`→《，`<<`→〈，`>>`→〉）
     - **全角/半角（width）**：候选窗底部「半」/「全」按钮切换。全角模式所有 ASCII 可打印字符（字母/数字/标点）全角化（0xFF01-0xFF5E，复用 `Global::FullWidthCharTable`）；实现位置：`GetPunctuation`（英文标点态标点）+ `_AddComposingAndChar`/`_AddCharAndFinalize` 入口（字母/数字/符号上屏兜底，`ToFullWidthIfNeeded`）——汉字/emoji（非 ASCII）不受影响，半角模式零开销；中文标点态不受全角模式影响
  - **配置**：engine.conf `punct=0/1`（默认 1=中文标点）、`width=0/1`（默认 0=半角）；DLL 读取用**纯 Win32**（CreateFileW+ReadFile，`Global::ReadConfFlag`，进程内 DLL 禁止 CRT 文件 IO/locale——跨模块 locale 竞态崩溃教训），DLL 多宿主进程实例无需消息同步，下次按键立即生效
  - 与既有 TSF 语言栏按钮并存（per-实例 compartment vs 全局配置，互不干扰）
  - 验证：编译/部署通过；真实打字待用户验证

### 宿主崩溃排查经验（Trae 案例，教训完整记录）
- **现象**：部署后 Trae 打字"崩溃"，用户一度怀疑输入法；搜狗输入法同时注入 Trae 进程（全局钩子组件），曾怀疑冲突
- **排查结论**：
  - WER 无 Trae 崩溃记录、crashpad 无新 dump——真崩溃必有痕迹，实际是 `AppHangTransient`（无响应）+ 同刻系统 `LiveKernelEvent`（WATCHDOG 4400/4401 = 显卡驱动 TDR）
  - 用户在跑 vLLM（GPU 满载）→ 显卡 TDR 冻结 GPU 渲染 → Electron 主进程退出；与输入法无关（搜狗+Nova 同进程共存打字测试通过）
- **取证配置**：`HKCU\...\Windows Error Reporting\LocalDumps\Trae.exe`（DumpType=2）——今后 Trae 崩溃必留 dump 到 `CrashDumps`
- **搜狗处置**：搜狗残留装机，禁自启（SogouSvc 服务 Disabled + 杀后台进程）；输入法本身保留（可手动切换使用）

### 部署脚本 ✅
- `tools/deploy.ps1` 一键部署（5 步：停进程 → 全量编译 DLL+引擎+Server → 版本化复制 → 注册 + 开机自启 → 启动）
- 注册：`tools/register.ps1 -Dll <路径>`（自动提权 UAC）
- 开机自启：HKCU Run `PinyinPlus.Server` → Server exe

## 三、未来待办

### 阶段 3 剩余
- [ ] **快捷键设置**：中英切换、翻页键等（涉及 TSF DLL 按键逻辑，改动面最大、风险最高，最后做）
- [ ] **皮肤切换**：候选窗多主题（当前仅 Nova 深蓝灰）
- [ ] **数字声调输入开关**：ni3hao4 → 你好（需词库补声调数据，做成可选开关默认关，不破坏选字键）

### 词库调研结论（2026-08-13）：rime-ice 替换方案 ✅ 调研完成

**现状诊断**（`bin/pinyin-plus.txt`，15.4 万条，来源 CC-CEDICT + wordfreq + pinyin-data，见 `tools/build_dict.py`）：
- CJK 扩展区生僻字 17,652 条（11.4%），最大噪音源
- 繁体字形混入简体词库（愛/車/對/個 等，数量少但词频高，直接挤占候选前位）
- wordfreq 是英文主词频，中文排序不准；CC-CEDICT 偏词典体（文言/生僻），缺人名地名

**rime-ice 实测**（2026-08-13 下载 GitHub 最新版逐条统计）：
- 总规模：去重 190 万词语条；tencent 98.2 万（人名/地名/网络词）/ base 54.3 万（核心）/ ext 33.9 万 / 41448 大字表 4.6 万 / 8105 常用层 0.9 万
- 生僻字全覆盖：朱镕基 ✓（base，`zhu rong ji` 权重 137）、镕 ✓（8105）、龘爨齉靁嫑犇羴鱻 全部 ✓；41448 覆盖 4 万汉字（基本区 20870 + 扩展A 5243 + 扩展B+ 15465）
- 分层设计：8105（8182 字）默认启用防生僻字刷屏，41448 按需开——治"生僻字混在候选"的良方
- 纯简体：8105 抽样繁体混入仅 1 字（'車' 异体对照），对比 CC-CEDICT 干净一个量级
- 开源可随 GitHub 仓库发布（对比搜狗词库 EULA 禁再分发）
- 格式 `词\t拼音(空格分隔)\t权重`，与 Nova `拼音\t词\t词频\t简拼` 不同 → build_dict.py 加解析分支（简拼现算、权重归一化），引擎零改动（`Entry{pinyin,word,initial,freq}` 兼容）

**简繁切换方案（业界标准，rime 生态做法）✅ 已实施（2026-08-13，见"阶段 4"繁体输出条目）**：
- 不维护两套词库：一套简体词库 + OpenCC（BYVoid，Apache-2.0）S2T/T2S 转换表，输出层转换（简体模式直出 / 繁体模式候选过 S2T）
- 引擎核心零改动（转换在引擎管道输出层）；设置面板"数据与学习"→"繁体输出"开关（engine.conf tradition + 管道 type 15 热生效）
- 当前混排词库无法做切换（无法程序化区分简繁）——替换词库是前提（已完成：rime-ice）

**搜狗词库结论**：
- 个人词库：搜狗"设置→词库→导出词库"导出 txt（词+拼音+频率），并入 `bin/userdict.txt` 私用 OK；安装目录 .dat 加密读不了；细胞词库 .scel 下载需登录
- ⚠️ 搜狗词库 EULA 禁止再分发 → 绝不能打包进 GitHub 公开仓库
- 工具：imewlconverter（深蓝词库转换，scel→txt）、ciku（pypi，Python 解析 scel/bdict/qcel）

**落地路径（分步推进）**：
1. build_dict.py 引入 rime-ice：默认 base+ext（88 万 ≈ 28MB，引擎上限 256MB 富余），tencent 可选层（实测启动速度定）✅
2. 过滤层：CC-CEDICT 保留源过繁体/扩展区/非汉字过滤；生僻字改由 41448 按需加载 ✅
3. 搜狗个人词库 → userdict.txt（私用）
4. 简繁切换：OpenCC 输出层转换 + 设置开关 ✅（2026-08-13，见"阶段 4"繁体输出条目）

### 阶段 4：词库与算法
- [x] 词库替换为 rime-ice（已完成：88.1 万条，见上方"阶段 4"）
- [x] 造词切分优化（SegmentToSyllablesBest）、局部重排、双缓冲热重载（任务 1/2/3）
- [ ] 整句预测精度优化
- [ ] 用户词库去重与容量管理（畸形词条防护：加载时校验、上限控制）
- [ ] 造词模式存完整拼音（当前简拼造词会产生 zt→xxx 类畸形词条，仅靠 IsValidFullPinyin 防整句污染）
- [x] **符号/表情输入（方案 A：词库符号层）** ✅（2026-08-13，见下方落地记录）
- [ ] 符号面板（方案 B，可后续增强：Server 分类浏览，与词库层并存）

### 符号输入调研（2026-08-13）：现状 + 方案评估（未动手）

**现状**：不支持。词库 `pinyin-plus.txt` 只有纯汉字词（build_dict 的 `is_pure_chinese`/`is_hanzi` 过滤层砍掉了一切符号）；rime-ice 的 `others.dict.yaml` 是容错词（口语读音如"空落落 kong luo luo"），并非符号表。引擎查询/候选/上屏链路对 word 内容无汉字假设（`Entry.word` 是任意 wstring），**加符号词条技术上零阻塞**。

**三个方案的对比评估**：

| 方案 | 做法 | 快 | 好用 | 风险 |
|---|---|---|---|---|
| **A 词库符号词条**（推荐先做） | 自建符号表（拼音→符号，数百条）并入 build_dict（绕过汉字过滤，低词频殿后） | ✅ 改动最小：词库+构建脚本，引擎/Server 零改动 | 拼音直达，候选即时翻页（dui→✓、wenhao→？、sanjiao→△、xiaolian→😄） | 无符号分类浏览；emoji 渲染依赖系统字体（Segoe UI Emoji 自带 ✓） |
| B 符号面板 | Server 侧弹窗分类（标点/数学/货币/箭头/emoji），点击上屏 | ❌ 需新 UI + 插入上屏链路 | ✅ 全符号浏览 | 工作量大，暂缓 |
| C v 模式 | 输入 v 进入符号模式（微软拼音式） | ❌ 涉及 DLL 按键逻辑 | ✅ 顺手 | 与现有 v 开头的全拼/造词路径冲突，风险最高 |

**结论**：先做方案 A（词库符号层，一次构建，拼音直达最快）；方案 B（面板）作为后续增强，可与 A 并存（词库管拼音直达、面板管分类浏览）。符号词条数量控制在 200-500 条（不刷屏），词频给低值自动殿后，不影响常用字候选。

**落地记录（方案 A 已实施 ✅）**：
- `tools/data/symbols.txt`：符号表（`拼音\t符号[\t简拼]`，**208 条**），分类：中文标点（顿号/问号/省略号…）、数学（≥/π/√/‰…）、货币（¥/€/£）、箭头（↑→）、图形（△/★/✓/✗）、版权商标（©/™/®）、emoji 常用 100+（😄/😂/❤️/🌹/🐱…）
- **数学符号补全（2026-08-13，用户反馈缺失后补）**：相似∽、全等于≌（U+224C 三角形）、恒等于≡（独立 hengdengyu）、平行于⫽（U+2AFD 斜线两根，非垂直双线 ∥）、垂直于⊥，另有集合/逻辑（∈/⊆/∪/∩/∨/∧）、希腊字母（α/β/γ/Δ/π/Σ）等 46 条
- **简拼直达列（2026-08-13）**：120 条多字符号配第三列显式简拼（`sanjiao\tsjx`、`quandengyu\tqdy`、`renminbi\trmb`、`weiho\swh`、`xueli\txl`…），简拼输入直达符号
- `tools/build_dict.py`：`load_symbols()` 并入（**绕过汉字过滤**——符号非汉字，被 is_pure_chinese 砍掉；SYMBOL_FREQ=0.5 低词频殿后；initial 缺省取首字母、有第三列取显式简拼）
- 结果：默认词库 881,774 条（+208 符号），引擎零改动（Entry.word 是任意 wstring，查询/候选/上屏链路天然兼容）
- **引擎符号优先排序**（`CollectWordByInitial`）：音节数完全匹配组内 `HasNonHanzi`（非 CJK 基本区=符号）优先于汉字——sjx 首位 = △（否则被高频汉字"数据线/实践性"淹没）
- **畸形简拼过滤**：`CollectWordForWildcard` 加 `IsValidFullPinyin` 过滤——userdict 历史畸形词条（如旧版 `sjx→数据线`）命中会截断简拼回退路径，过滤后 sjx→△ 直达正常
- 验证：sanjiao→△（首位）、quandengyu→≌、pingxing→⫽、chuizhi→⊥、dui→✓（排在"对/队/堆/兑/怼"后第 6 位，不刷屏）
- ⚠️ 注意：wenhao 等低频拼音下符号可能排到第 5 位（同音汉字词频低），属正常；不刷屏

### 产品/发布
- [ ] 中英双语 README + Release 文案（GitHub 公开项目，参照 Nova 浏览器风格）
- [ ] 输入法显示名：注册/设置面板统一为"Nova 输入法"（当前注册名仍是 Sample IME / PinyinPlus）
- [ ] 托盘菜单"关于"、版本信息

## 四、关键文件索引

| 文件 | 职责 |
|---|---|
| `src/ime/SampleIME.vcxproj` | TSF DLL（纯壳） |
| `src/ime/Composition.cpp` | 组合文本管理（拼音隐藏/汉字上屏） |
| `src/ime/KeyHandler.cpp` | 按键处理入口 |
| `src/ime/PinyinIpc.cpp` | DLL↔引擎 PPIM 管道（句柄唯一关闭方） |
| `src/ime/EngineClient.cpp` | DLL 侧引擎保活线程 + 重连 |
| `src/ime/PinyinEngine.cpp/h` | 引擎词库/候选/模糊音/简拼/整句预测/自学习 |
| `src/engine/engine_main.cpp` | 引擎进程入口（单实例互斥 + ServerWatchdog） |
| `src/engine/EnginePipe.cpp` | 引擎管道路由（type 1/3/5/6/7/9/11/13-15，13=删用户词，15=繁体开关） |
| `src/engine/TraditionalConvert.cpp/h` | 简繁转换模块（OpenCC S2T/T2S，输出层转换 + 用户词回存） |
| `src/server/PinyinPlus.Server.csproj` | WPF 候选窗/托盘/设置面板 |
| `src/server/CandidateWindow.xaml*` | 候选窗 UI + 定位 |
| `src/server/CandidatePlacement.cs` | 候选窗定位模块（DPI/多屏/抖动抑制） |
| `src/server/AppConfig.cs` | config.json（PageSize/CandidateFontSize） |
| `src/server/EngineConf.cs` | engine.conf 共享读写（learn/bigdict/tradition/punct，设置面板与候选窗状态条共用） |
| `src/server/SettingsWindow.xaml*` | 设置面板（外观/自学习/词库导入导出） |
| `src/server/EnginePipeClient.cs` | Server→引擎 重载消息 |
| `bin/engine.conf` | 自学习 + 大字库 + 繁体 + 中文标点 + 全半角（learn/bigdict/tradition/punct/width=0/1） |
| `bin/STPhrases.txt` 等 4 张 | OpenCC 简繁转换表（Apache-2.0，引擎繁体输出运行期读取；源文件在 tools/data/） |
| `bin/userdict.txt` | 用户词库（pinyin\tword\tfreq[\tinitial]） |
| `bin/pinyin-plus.txt` | 主词库（默认，88.1 万条） |
| `bin/pinyin-plus-big.txt` | 大字库（--big，91.7 万条，含生僻字） |
| `tools/deploy.ps1` | 一键部署 |
| `tools/register.ps1` | DLL 注册（UAC） |
| `tools/data/symbols.txt` | 符号表（拼音→符号，208 条，可带显式简拼列） |

## 五、常用操作

- **全量部署**：右键运行 `tools/deploy.ps1`（改任何代码后跑它）
- **只重构建/部署 Server**：停 Server+引擎 → `dotnet build src/server/PinyinPlus.Server.csproj -c Release` → 启动引擎（看门狗拉 Server）
- **只更新 DLL**：构建 ime 项目 → 版本化复制到 bin → `register.ps1 -Dll 新DLL`（旧进程不受影响，新开应用生效）
- **日志**：`tools/engine_debug.log`（引擎）、`bin/ime_debug.log`+`bin/engine_client.log`（DLL）、`tools/app_trace.log`+`tools/server_debug.log`+`tools/pipe_debug.log`（Server）
- **候选窗定位调试**：拖窗口看是否贴光标；抖动异常查 CandidatePlacement 的 GetDpiForMonitor
