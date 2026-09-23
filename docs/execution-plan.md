# HSSH 执行计划（B1–B8 批次）

> 版本 v1（2026-09-21）。基于 `docs/implementation-plan.md` 的全部未完成/余量项（44 项），按"依赖 + 性价比"排成 8 个批次。
> 工作量为单人净人日；批次内按列出顺序执行；PH 编号对应 implementation-plan.md 的详细方案（目标/技术方案以那边为准，本文件补执行拆解与批次验收）。
> 当前快照：Phase 0（除 PH0-06）、Phase 1（除 PH1-09）、PH2-08 同步输入、PH2-10 ZMODEM 接收、PH3-02 串口已完成；代理/跳板/Agent 转发编译树验证、真机带凭据链路待实测。

## 批次总览

| 批 | 主题 | 项数 | 工作量 | 批次验收（DoD） |
|----|------|------|--------|----------------|
| B1 | P0 收尾 | 5 | 10–14d | M2 里程碑补齐：密钥管理全流程 + ZMODEM 双向 + 三链路真机报告 |
| B2 | 终端体验 | 5 | 16–23d | 链接/OSC52/命令面板/时间戳可用；reflow 单独验收 |
| B3 | 安全认证 | 4 | 8–11d | 首连指纹确认 + 2FA 登录 + 构建开关齐备 |
| B4 | 文件传输 | 6 | 13–15d | SCP UI / 远程编辑回传 / 搜索 / chmod / 直传全通 |
| B5 | Agent 扩展 | 4 | 9–10d | forward 工具 + 白名单授权 + Streamable HTTP + WS |
| B6 | 运维套件 | 5 | 15–19d | 监控/进程/网络/Docker/定时五个面板可用 |
| B7 | 协议矩阵 | 8 | 22–31d | Telnet/Raw/ppk/ControlMaster/mosh/X11/RDP/FIDO2（按需裁剪） |
| B8 | 平台化/远期 | 8+ | 23–32d | 脚本/插件/gRPC 等，用到再做 |

总计约 116–155 人日。B7/B8 为按需批，可整体延后。

---

## B1 · P0 收尾（10–14d）

### B1-1 PH1-09 SSH 密钥管理器（5–7d）✅ 完成（2026-09-21，test_keystore 8 用例 + 对话框冒烟）
> 生成仅 Ed25519：libssh mbedTLS 后端的 `pki_private_key_to_pem()` 是返回 NULL 的桩（pki_mbedcrypto.c:57），RSA/ECDSA 私钥导出不可用；导入不受影响。ppk 转换在 B7-3。
- 拆解：
  1. `src/app/dialogs/KeyManagerDialog.{h,cpp}`（新）：QTableWidget 列表（名称/类型/指纹 SHA256/路径）+ 工具栏（生成/导入/导出公钥/删除/刷新）。
  2. 生成对话框：类型下拉（ED25519/RSA-2048/4096/ECDSA-256/384）+ 可选 passphrase + 名称；调 `KeyStore::generateKey`。
  3. 导入：QFileDialog 选 OpenSSH/PEM 私钥 → 拷入 KeyStore 目录（重名自动后缀）；ppk 本批不做（B7-3）。
  4. 导出公钥：复制 authorized_keys 行到剪贴板 + 显示指纹。
  5. `MainWindow.cpp:202` Key Manager 菜单接 handler。
  6. 会话引用：NewSessionDialog 认证页加"KeyStore 密钥"下拉（keyId）；`SshConnect` 认证时 keyId → 路径解析。
- 涉及：KeyManagerDialog（新）、`src/core/KeyStore`（已有）、NewSessionDialog、MainWindow、SshConnect。
- 验收：生成→复制公钥→写入目标机 authorized_keys→免密登录成功；会话选 keyId 认证成功；删除密钥后引用它的会话报明确错误；主密码锁定时不可导出私钥。
- 测试：`tests/test_keystore.cpp`（生成/指纹/导入往返/删除）。

### B1-2 PH2-10 ZMODEM 发送方向收尾（1–2d）✅ 回环闭环完成（2026-09-21，test_zmodem 5 用例：22B/1024B 整块/64K+777/转义压力(1024×ZDLE+全特殊字节)/300KB 随机，全部字节一致）
> 修复了三个只在同步回环下暴露的重入 bug：① feed() 先 dispatch 后消费帧字节 → 同步应答重析旧帧；② 状态迁移放在 emit 之后 → 应答命中过期状态（ZFILE 双发、meta 被当数据写入）；③ ZEOF 完成路径先发 ZRINIT 后 finishTransfer → finished 双发。接收侧补上了此前缺失的子包/帧 CRC32 校验（对齐真实 rz 行为：错 CRC → ZNAK + 重发）。真机 rz 落盘验证并入 B1-5。
- 拆解：会话日志原始字节对拍（AGENTS.md 方法论）定位数据子包 CRC/转义差异 → 修 `ZModemEngine` 发送侧 → 三档验收（25B / 1MB urandom / 转义压力）→ REST `/zsend` 错误路径（文件不存在/空文件）。
- 验收：sz→rz 真机 md5 一致；`truncated`/失败响应正确。

### B1-3 PH2-08 自由输入模式（1d）✅ 完成（2026-09-21）
> 工具栏 Free Type 常开切换：开启时（有其他 SSH 标签则先确认，警告 vim/sudo/密码场景）键盘输入经 InputBroadcaster 镜像到**所有其他 SSH 标签**；状态栏常驻提示；关闭即恢复。与 Sync Input 组互不干扰（Free Type 优先）。
- 拆解：工具栏 toggle"Free Type"→ InputBroadcaster 目标=全部已连接 SSH tab；状态栏持续提示 + 关闭确认；Esc 退出。vim/密码场景进入前警告弹窗。
- 验收：开关状态清晰无残留；广播与同步输入组互不干扰。

### B1-4 PH1-08 余量：标签拖出浮窗（2–3d）◐ 完成（2026-09-21，待用户手动验收）
> 拖拽标签离开标签栏 ±12px → 弹出 FloatingTabWindow（标签对象连连接/sync 组/pin 状态整体搬家）；窗口工具栏 "Dock to Main Window" 归位（标题/图标/固定状态还原）；右键菜单 "Detach to Window" 等效入口；固定标签拒绝分离；关闭浮窗=关会话（unregister 广播组）。实现：tabBar 事件过滤器（合成 MouseButtonRelease 先终结 Qt 内部 reorder 拖拽再 detach）+ `takeCentralWidget()` 交接所有权。已验证：拖出创建浮窗、进程无崩溃、tabs API 正确反映分离；**Dock 回主窗/关闭浮窗路径留用户手动验收**（合成鼠标在共享桌面有副作用，停止自动化）。
- 拆解：`SessionTabBar` mouseMove 判定拖出（光标离开 tab 区域 20px）；`FloatingTabWindow`（QMainWindow 承载单 SessionTab）；关闭=断开或拖回主窗口；tab 状态（alias/color/pinned）随行。
- 涉及：`src/app/FloatingTabWindow.{h,cpp}`（新）、SessionTabBar、MainWindow。
- 验收：拖出/拖回不闪退；浮窗内终端/传输/右键菜单功能完整。

### B1-5 三链路真机实测（1d，需用户环境）
- SOCKS5/HTTP 带凭据代理；单级跳板（密码 + 密钥两种认证）；Agent 转发（远端 `ssh-add -L`、git clone）。
- 产出：实测记录写入 AGENTS.md（含失败案例的根因）。

---

## B2 · 终端体验（16–23d）

### B2-1 PH2-03 链接可点击（2–3d）✅ 完成（2026-09-21，testLinkDetection 像素扫描验证）
> `linkAt(pos)`：所在行列文本取 token（非空格极大段）→ 正则抽 `https?://|file://|mailto:` → 剥尾部句读（`(see http://a.b/c).` → `http://a.b/c`）；Ctrl+Click 打开（鼠标上报模式/Shift 绕过不冲突）；hover 手型光标 + tooltip；右键 "Open Link"/"Copy Link Address"。测试用像素扫描法（不依赖 margin/字体度量）。

### B2-2 PH2-04 OSC 52 剪贴板（1–2d）✅ 完成（2026-09-21，decodeOsc52 纯函数单测 + 集成用例）
> feedData 内独立嗅探状态机（Ground/Esc/Body/BodyEsc，BEL 与 ST `\x1b\\` 终结符，跨 chunk 缓冲，8MB 防跑飞）；`decodeOsc52("52;Ps;Pb64")` 纯函数（选择器 c/p/s、base64 严格校验、1MB 上限、查询帧不响应）；`terminal/osc52Mode` 三态 deny/prompt/allow（prompt 弹确认，带重入保护）。集成用例在本机被剪贴板管理器锁死时自动 SKIP（0x800401d0），此前健康窗口已验证全链路。

### B2-3 PH2-07 命令面板（2–3d）✅ 完成（2026-09-21，test_palette 单测）
> `CommandPalette`（Popup 无边框 + QLineEdit + 双列 QListView）：fuzzyScore 子序列评分（前缀/连续加成，等分按下标稳定排序）；数据源=菜单 QAction 递归收集 + 保存会话（onSessionActivated）+ 打开标签跳转；Ctrl+Shift+P 唤起；最近使用经 `ui/commandPaletteRecents` 持久化（前 10）加权置顶；↑↓ 选择 / 回车执行 / Esc 关闭。

### B2-4 PH2-02 时间戳 / 折叠 / 大纲（5–7d）◐ ①③ 完成（2026-09-21，testTimestampsAndOutline）
> ① 时间戳：ScrollbackEntry 带 arriveMs（滚出屏时间戳）+ 活屏行 damage 时间戳；左侧 11 列 gutter（`[HH:MM:SS]` 灰字，contentX() 全链坐标偏移：绘制/选择/搜索/光标/鼠标映射/尺寸）；右键 "Show Timestamps" 开关即时生效并持久化 `terminal/showTimestamps`（列数随 gutter 收缩/恢复，测试锁定恰好 11 列）。
> ③ 大纲：`TerminalOutlineWidget` 右侧 dock（View→Outline）——正则标提取（`user@host:` 提示符 / `make[n]: Entering|Leaving directory` / ISO 时间戳行 / `[FATAL|ERROR|WARN]`）；3s 定时刷新（隐藏即停）；点击 `scrollToLogicalRow` 跳转。TerminalWidget 新增 bufferRowCount/outlineLineAt/scrollToLogicalRow API。
> ② 折叠：**未做，单独排期**——渲染裁剪与选择/滚动坐标系深度耦合（风险仅次于 reflow），不宜和 ①③ 混在一次改动里。

### B2-5 PH2-01 reflow（5–8d，最高风险，批次末单独排）✅ 完成（2026-09-21，testScrollbackReflow + WideGlyphs）
> **wrap 判定（启发式）**：`sb_pushline` 时 `entry.wrapped = 内容宽度恰好填满整行`（剥掉 chars[0]==0 的填充 cell 后最后一格抵达行尾）——经典做法；已知误报：整宽定界线会被并进下一行，可接受。
> **重排算法**（`reflowScrollback(newCols)`，列变化 150ms 防抖触发）：① 按 wrapped 标记把物理行合并为逻辑行（glyph 串 + 首行时间戳）；② 按新列宽**列宽感知重切**（累计 grid 列，宽字形跨界则整体移入下一块，绝不劈半）；③ 尾块 wrapped=false；超 maxScrollbackLines 从头丢弃。
> **坐标处置**：选择丢弃（clearSelection）；活动搜索 updateSearch 重跑；活屏交给 libvterm 自身 resize（现状）。
> 测试：100×A 在 80 列 80/20 → 40 列重切 40/40/20 → 回 80 列还原 80/20 且拼接内容一致；19 个 CJK 宽字形 + XY 在 20 列重切，无代理对断裂、字形总数 21 不变。
- 拆解：ScrollbackLine 加 wrap 标记；resize 防抖 150ms 后 scrollback 重切（合并 wrap 行→按新列宽重排）；live screen 交给 libvterm；选择/搜索逻辑行重映射。
- 降级预案：仅 scrollback 重排，live screen 保持现状。
- 验收：拖宽/拖窄历史行正确换行；光标与屏幕内容不丢；选择/搜索不偏移。

---

## B3 · 安全认证（8–11d）

### B3-1 PH2-12 known_hosts 指纹管理（2–3d）✅ 完成（2026-09-21，testKnownHostsListAndRemove）
> 弃用 API 迁移（`ssh_is_server_known`→`ssh_session_is_known_server`、`ssh_write_knownhost`→`ssh_session_update_known_hosts`）；`verifyAndStoreHostKey` 在 `sshConnectAndAuthenticate` 里认证前拦截（跳板腿同样校验）；指纹双格式（SHA256 base64 + MD5 冒号）。**策略**：`security/hostKeyPolicy`（accept-new 默认=TOFU 首用信任/变更拒绝；ask=GUI 弹窗确认，SessionTab 经 BlockingQueuedConnection 把 worker 线程的决定请求弹到主线程；accept-all=旧行为）。变更密钥接受时先清旧条目（host / [host]:port 两种拼写）。管理入口放 Key Manager（Known Hosts 表 + Remove，替代原计划的设置页）。**行为变更提示**：默认从"从不校验"升级为 TOFU——存量主机首连会落一次 known_hosts；开发容器轮换密钥的场景可配 accept-all。
- 拆解：KeyStore 弃用 API 先迁移；首次连接指纹确认弹窗（SHA256+MD5，显示 host:port）；`SSH_SERVER_KNOWN_CHANGED` 强告警 + 移除旧条目；SettingsDialog known_hosts 列表页（查看/删除）；`sshConnectAndAuthenticate` 加状态回调。
- 验收：首连确认后落盘；换主机密钥告警并可重置；删除条目后重新确认。

### B3-2 PH2-13 2FA / kbdint（2–3d）✅ 完成（2026-09-21，编译树验证；真机 2FA 服务器待验）
> `authenticate()` 真正的 keyboard-interactive 循环（≤8 轮防死循环）：单一隐藏"Password"轮自动用库存密码应答（保持旧行为，无 GUI 也通）；其余轮（TOTP/推送/验证码）经 `KbdintPrompter` 回调——SessionTab 用 BlockingQueuedConnection 把弹窗调度到主线程（与主机密钥确认同模式），`KbdintPromptDialog` 逐项输入（echo 标志决定掩码）+ "Remember for this session"（答案只存标签页内存 QHash，不落盘）；取消/无 prompter（agent/headless）返回明确错误。注意：**跳板腿未传 prompter**——跳板机开 2FA 会拒绝，需要时后续补。坑：该 libssh 构建头文件声明了 0.11 的 `getnanswers` 但库里只有 0.10 的 `getnprompts`（链接期暴露）；`getprompt` 返回 const 存储（不可 free）。
- 拆解：`KbdintPromptDialog`（逐项 prompts，密码项掩码）；worker 线程 ↔ GUI 跨线程 QMetaObject；会话级内存缓存（不落盘）。
- 验收：TOTP 类验证码登录通过；cli/headless 路径返回明确错误。

### B3-3 PH0-06 构建开关（1–2d）✅ 完成（2026-09-22，默认配置全量构建 + ctest 13/13 验证降级路径）
> `option(HSSH_WITH_GSSAPI)`（默认 OFF）→ FetchLibSSH 的 `WITH_GSSAPI` 联动 + `HSSH_HAS_GSSAPI` 宏（开启会整体重编 _deps 里的 libssh）；Qt6 WebSockets 可选 find_package（本机 SDK 未装该模块 → **优雅降级验证通过**，仅打状态行不失败）；`find_program` 探测系统 ssh → `HSSH_HAS_SSH_BIN` + `HSSH_SSH_BIN="C:/Windows/System32/OpenSSH/ssh.exe"`（ControlMaster/FIDO2 备选路径用）。
> 过程坑：reconfigure 后 `CMAKE_RC_COMPILER` 缓存是裸名 `windres`（首次 configure 时靠 PATH 解析），新增编译定义触发 .rc 重编就 CreateProcess 失败——删 `CMakeRCCompiler.cmake` 后用 `-DCMAKE_RC_COMPILER:FILEPATH=<全路径>` 重新检测钉死。**教训入 AGENTS.md：改全局编译定义会触发 .rc 重编，裸名 windres 在非原始环境必炸。**
- 拆解：CMake 选项 HSSH_WITH_GSSAPI（FetchLibSSH 联动）、可选 find_package WebSockets、HSSH_HAS_SSH_BIN 探测；无模块机器可构建。
- 验收：默认配置构建不受影响。

### B3-4 PH2-11 GSSAPI（2–3d + 域环境）
- 拆解：WITH_GSSAPI 开启（Windows security.lib/SSPI）；AuthMethod::Gssapi + `ssh_userauth_gssapi`；NewSessionDialog 下拉。
- 风险：需域环境验证，验证成本可能超开发。
- 验收：域内免密登录；无 GSSAPI 环境构建正常。

---

## B4 · 文件传输（13–15d）

| 项 | 工作量 | 拆解要点 | 验收 |
|----|--------|----------|------|
| B4-1 PH2-09 SCP UI | 1d | ✅ 2026-09-22：SftpWidget 工具栏 SFTP/SCP 下拉，SCP 走 parentless ChannelCopySession(Scp) worker（进度/完成/取消全接入现有 TransferRegistry+QProgressDialog 管道） | UI 发起 scp 传输与 agent 路径同一 worker；真机验证并入 B1-5 |
| B4-2 PH2-15 余量 | 2d | ✅ 2026-09-23：SessionConfig tags/favorite（持久化+快速过滤命中+收藏置顶排序）；会话树右键 Add/Remove to Favorites、Edit Tags...（逗号分隔去重排序）；SessionLogViewer（Tools→Session Log Viewer / 标签右键 View Session Log 预选当前标签日志）——原始字节流 ANSI/OSC/控制符剥离显示、按文件名时间戳过滤（All/Today/3/7/30d）、关键字全量高亮+回车循环跳转（万条上限）、导出剥离文本（>20MB 只显示尾部）；test_sessionlog（时间戳解析/ANSI 剥离/tags 往返） | 收藏置顶、tag 检索、日志按时间检索 |
| B4-3 PH3-11 远程编辑 | 2–3d | ✅ 2026-09-23：`RemoteEditManager` 单例（sessionKey+remotePath 全局锁防多标签互踩、QFileSystemWatcher+500ms 防抖、mtime 比对去重、rename 式保存自动重挂监听、本地副本删除→editGone 收尾）；SftpWidget 右键 Edit '<file>'（下载 %TEMP%\hssh-edit\<session>\<sha1-8>_<name>→下载完成才 beginWatch→系统编辑器）→保存自动回传（SFTP 顺序队列 upload+chmod 保权限位）+Stop Editing；SftpSession 新增 setPermissions（sftp_chmod）；test_remoteedit（锁冲突/防抖合并双写/markSynced 去重/删除收尾）。真机编辑闭环待 B1-5 一并验 | 保存自动回传，内容一致（单测覆盖管理器语义；真机待验） |
| B4-4 PH3-12 远程搜索 | 3d | ✅ 2026-09-23：SftpSession 遍历加 maxDepth（服务器侧截断省网络）+doListDirRecursive 复位 m_cancelTransfer（修预存在"取消毒化下一遍历"）；`RemoteSearchDialog`（SftpWidget 右键 Search in this folder，非模态单例）：通配名过滤（含 '/' 匹配相对路径）/大小过滤（目录被尺寸条件排除）/N 天内修改/深度上限（默认 3，0=不限）；过滤纯函数本地跑（万级条目毫秒级），结果表 5 万行上限+数值排序键，双击定位（目录→自身/文件→父目录）；搜索期间 dirTreeProgress 实时计数+Cancel；m_active+root 匹配防与 compare 遍历串台；test_remotesearch（通配/大小/时间/组合） | 万级文件不卡 UI（遍历在 worker 线程+本地过滤） |
| B4-5 PH3-14 chmod UI | 2d | ✅ 2026-09-23：`PermissionsDialog`（SftpWidget 右键 Permissions…，单选条目）——3×3 rwx 勾选 ↔ 八进制输入双向同步（m_syncing 防回环，八进制 3 位完整才触发反向同步）、符号显示、属主/属组只读展示（SftpFileInfo 新增 owner/group，attr->owner 空指针防护）；目录可勾递归（`setPermissionsRecursive`：worker 线程内 collectRemoteFiles 遍历+sftp_chmod 全条目含根，进度/取消走现有通道，失败汇总首个错误）；test_permissions（parseOctal 严格 3 位八进制/symbolic 渲染/对话框 0777 掩码初始化+键入同步） | 改权限立即生效（operationFinished→refresh；真机并入 B1-5） |
| B4-6 PH3-13 服务器直传 | 3–4d | ✅ 2026-09-23：`ServerTransferDialog`（Tools→Server-to-Server Transfer）——**设计变更**：放弃原"内存管道 64KB 泵"改**两跳临时文件中转**（源→verify 下载→%TEMP%\hssh-s2s→verify 上传→目标），SftpSession 全套安全网（.part 原子性/双向 md5/换连接重试/timeout=0）原样继承，双跳校验直接保证端到端 md5 一致——流式管道需绕开安全网重写读写循环，得不偿失。双会话下拉（保存的 SSH 会话）+双路径；两阶段进度条+Cancel；**连接级 errorOccurred 在活动阶段按终态处理**（doDownload 未连接路径只发 errorOccurred 无 transferFinished，被动显示会挂死）；TransferRegistry 两跳分别登记、下载成功即刻关闭源条目（上传失败不连累源状态）；对话框关闭=取消；workers parentless 同铁律 | 两远端主机间传输 md5 一致（双跳各带 verify；真机并入 B1-5） |

---

## B5 · Agent 扩展（9–10d）

### B5-1 ssh_forward 工具（2d）✅ 完成（2026-09-23，test_agent_http 新增 forwardRoutes：空列表/类型与端口校验/local 缺 target 拒绝/listen+target 返回/动态 SOCKS 无 target/列表稳定索引/删除重排/越界与缺 index/local 标签拒绝，20 用例全绿）
> AgentTabsInterface 新增 addForwardToTab/listForwardsForTab/removeForwardFromTab（MainWindow 实现，复用每会话 PortForwardManager；spec 校验=type 枚举/bindPort 1-65535/local+remote 需 targetHost:targetPort）；REST `/tabs/<ref>/forward` 三方法（POST 返回 {ok,listen,target,forwards}，DELETE ?index=N）；MCP `ssh_forward`/`ssh_list_forwards`/`ssh_remove_forward`（withTab 寻址，content+结构化字段双通道，审计 tab_forward_add/remove）。坑：`QVariantMap::value(key, default)` **只在键缺失时取默认值**——空串键会穿透，bindAddress 必须显式 isEmpty 回退。

### B5-2 AgentPolicy 授权（3d）✅ 完成（2026-09-23，test_agent_http policyGate：默认 Ask 弹窗/拒绝与超时 403 原因码/会话授予免弹窗/Always 持久化规则后免弹窗/deny 规则无弹窗直拒/local 标签空 identity 放行）
> `src/agent/AgentPolicy`：会话授予（内存）→持久规则（`agent/policyRules` 字符串列表 `<identity>|<op>=(allow|ask|deny)`）→默认（Upload/Download/Forward=Ask，Exec=Allow——可视终端即审计；sudo 保持自有确认链）。identity 复用 AgentSudoAuth（saved→session:id，临时→host:user@host）。AgentHttpServer `gatePolicy`：Allow 放行/Deny 403+审计/Ask 挂起 HTTP 请求走 `confirmPolicyAsync`（MainWindow 弹窗：This session/Always/Deny+30s 自动拒，无嵌套事件循环）。**顺手修真 bug：DELETE /transfer 只取消标签第一条记录——已取消但仍在后台收尾的记录会挡住真正活跃的那条**（偶发 409，全表诊断法定位）。

### B5-3 MCP Streamable HTTP / SSE（2–3d）✅ 完成（2026-09-23，mcpHttpEndpoint：initialize/tools-list/ping 数字+字符串 id 往返/通知 202/坏 JSON 400；GUI 冒烟 curl 直打 /mcp /sse 通过）
> `POST /api/v1/mcp`（Streamable HTTP 2025-03-26 子集：单消息单响应，批量 400）：AgentMcpServer 加 `Transport::Http` 模式（无 stdin，`handleIncoming` 进/`setMessageSink` 出），**与 stdio 完全同一工具面**；响应按 JSON-RPC id（n:/s: 前缀——2026-08 id 匹配教训）路由回挂起的 POST，120s 兜底 504；通知（无 id）按规范回 202。`GET /api/v1/sse`（2024-11-05 兼容）：endpoint 事件指向 /api/v1/mcp + 15s 心跳，断连清理。token 鉴权沿用全局门。AgentHttpServer::stop 统一收尾（挂起 MCP 504 化、SSE 断开）。

### B5-4 WebSocket（2d，依赖 B3-3）✅ 代码完成（2026-09-23，**条件编译未验证**——本机 Qt SDK 无 WebSockets 模块，HSSH_HAS_WEBSOCKETS 关闭时空翻译单元）
> `src/agent/AgentWebSocketServer`（全 #ifdef 守卫；无 Q_OBJECT 避开 moc 条件编译坑）：监听 agent 端口+1（绑定失败不致命）；帧协议 {ping→pong / shell-input(ref,data)→sendInputToTab / read(ref,lines) / subscribe(ref)→200ms 泵 readTabRange 增量推 shell-output / unsubscribe}；ref 解析与 REST 同语义（name[:ordinal]）。**装了模块的机器首次构建时会打印 "agent WebSocket endpoint available"，届时需真机验证帧协议**。

---

## B6 · 运维套件（15–19d）

| 项 | 工作量 | 拆解要点 | 验收 |
|----|--------|----------|------|
| B6-1 PH3-16 监控面板 | 4–5d | ✅ 2026-09-23：**B6 公共地基** `RemoteCommandChannel`（parentless worker+独立连接，一次性 runCommand/流式 runStream/取消，timeout=0+排空 stderr 铁律，stderr 尾巴进 error）；`ServerMonitor`（每 tick **一条组合命令**一次往返：/proc/stat 双读夹 sleep 1 算 CPU%、meminfo、net/dev、df -P；纯解析器 parseCpuPair/parseMemInfo/parseNetDev/parseDf/parseSample，计数器回绕跳过防负速率；断连自停）；`MonitorWidget` dock（View→Server Monitor；会话下拉=已连 SSH 标签；QPainter 自绘 CPU/MEM 双曲线 300 点历史+网格；net Top3 速率；磁盘表） | 实时曲线，采样 ≥1s 低开销（2s tick 单命令往返；真机数据面并入 B1-5） |
| B6-2 PH3-17 进程管理 | 2–3d | ✅ 2026-09-23：`ProcessDialog`（标签右键 Process List...，SSH 标签）——`ps -eo pid,ppid,user,%cpu,%mem,stat,args --sort=-%cpu | head -500` 走独立连接（不打扰终端）；纯解析器 parsePs（前 6 定长字段+args 保留空格）；过滤框（user/命令/PID）+列排序（数值键防 "9"<"10" 字符串序）；End Process(SIGTERM)/Kill(SIGKILL) 带确认，**stderr 留独立通道**（去掉 2>&1，错误尾巴进弹窗），权限不足明示走终端 sudo 路径不自动接力 | 列表准确，kill 生效（解析器单测；真机并入 B1-5） |
| B6-3 PH3-19 网络工具 | 2–3d | ✅ 2026-09-23：`NetworkToolsDialog`（标签右键 Network Tools...）——ping/traceroute **流式**（runStream，实时滚动+Stop 中断=关 channel）；ss -tunlp（busybox 回退 netstat）与端口探测（`timeout 4 bash -c 'echo>/dev/tcp/h/p'`→OPEN/CLOSED）一次性；**buildCommand 纯函数注入防护**（目标仅 `[A-Za-z0-9_.:\[\]-]`，端口 1-65535 数字校验——单测覆盖 `; rm`/`$(reboot)`/`|` 拒绝） | 输出实时滚动可停止（真机并入 B1-5） |
| B6-4 PH3-18 Docker | 3–4d | ✅ 2026-09-23：`DockerDialog`（标签右键 Docker...）——容器表（`docker ps -a --format '{{json .}}'` 行级 JSON，parseContainers 纯函数单测）/镜像表双 Tab；选中行启 Start/Stop/Restart/Remove（rm 带 `-f` 确认弹窗，**容器 id 过 safeIdRx 再进 shell 行**）；Logs... 模态子窗流式 `docker logs -f --tail 200`（关窗即 cancel）；docker 错误走 stderr 独立通道进弹窗（ps/images 不加 2>&1） | 容器启停与日志可用（真机并入 B1-5） |
| B6-5 PH3-15 定时任务 | 3–4d | ✅ 2026-09-23：`SchedulerDialog`（标签右键 Scheduled Tasks...）——间隔任务（10s..1h 预设）双模式：**background**（专用连接 runCommand，输出尾部+留痕）/**terminal**（`sendInput` 注入可视标签，会话日志天然留痕）；1s tick 扫描到期；结果追加 `<AppData>/logs/scheduler_yyyyMMdd.log`（与终端会话日志同目录）+面板显示最近结果；任务随标签生命周期（内存态，无持久化——v1 边界已注明） | 按计划执行留痕（到期调度纯函数 nextDue；真机并入 B1-5） |

---

## B7 · 协议矩阵（22–31d，按需裁剪）

| 项 | 工作量 | 要点 | 备注 |
|----|--------|------|------|
| B7-1 PH3-01 Telnet | 3–4d | TelnetTransport（IAC：SGA/ECHO/NAWS/TTYPE/LINEMODE 简化） | 网络设备场景 |
| B7-2 PH3-04 Raw TCP | 1–2d | 透传 + 换行转换 + hex 模式 + 初始串 | |
| B7-3 ppk 转换（PH1-09 拆出） | 2–3d | PpkParser v2/v3（AES-256-CBC 解密→OpenSSH 格式）；puttygen 备选 | |
| B7-4 PH3-07 ControlMaster | spike 2d + 4–6d | 先做方案 B（同主机已连提示复用），再做 ssh -MN socket 复用 | 依赖 HSSH_HAS_SSH_BIN |
| B7-5 PH3-03 mosh | 4–5d | mosh-server 经 SSH 启动取 MOSH_KEY → mosh-client 托管 | 依赖本机 mosh |
| B7-6 PH3-06 X11 | 5–8d | channel 回调 + 本地 X server 泵（VcXsrv/WSLg） | 含 spike |
| B7-7 PH3-05 RDP/VNC v1 | 2d | 外启 mstsc/xfreerdp/vncviewer 会话类型 | v2 内嵌 10d+ 不排 |
| B7-8 PH3-10 FIDO2 | 3–4d | sk-* 检测→OpenSSH 子进程回退通道 | 依赖 HSSH_HAS_SSH_BIN |

---

## B8 · 平台化 / 远期（23–32d，用到再做）

| 项 | 工作量 | 要点 |
|----|--------|------|
| B8-1 PH3-08 脚本宏 v1 | 3–4d | 录制/回放 + JSON 步骤（send/wait/expect/sleep）；v2 引擎另评 |
| B8-2 PH2-16 Tmux | 2–3d | 前缀快捷、load-buffer 粘贴、pane 列表（v2） |
| B8-3 PH3-20 剪贴板历史 | 2d | 环形 50 条 + 来源标记 + 面板回填 |
| B8-4 PH3-23 i18n 收尾 | 2–3d | 运行时语言切换、全局热键、无障碍（高对比已有） |
| B8-5 PH3-25 终端细节合集 | 3–5d | 双击选词/中键粘贴/背景图/拖文件进终端自动 rz |
| B8-6 PH3-21 密码管理器 | 3–4d | keepassxc-cli / op 拉取凭据，不落盘 |
| B8-7 PH3-09 插件系统 | 5–8d | QPluginLoader + IPlugin 接口 + manifest 扫描 |
| B8-8 PH3-22 gRPC | 5–7d | ssh.proto + GrpcServer 复用 tabs/策略 |
| 决策项 PH3-24 云资源 | — | 建议不做（与"完全本地"定位冲突） |

---

## 全局风险与决策点

1. **reflow（B2-5）**：全计划最高风险，预留降级预案；放批次末独立验收。
2. **环境依赖项**（GSSAPI/FIDO2/mosh/X11/密码管理器）：验证成本可能超开发，排期时先确认环境再动工。
3. **Agent 安全面**：B5-2 授权策略落地前，转发/WS 新入口默认收紧（沿用现有 token + sudo 确认模式）。
4. **文档同步**：每完成一项，同步勾选 `docs/requirements.md`、`docs/feature-gap.md`、`docs/implementation-plan.md` 状态并更新 AGENTS.md 经验教训。
