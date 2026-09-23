# HSSH 未实现功能实施计划（总纲）

> 版本：v1
> 覆盖：docs/requirements.md 全部 [ ] 项（29 项）+ docs/feature-gap.md 对标发现的未列出功能（43 项），全部转为可执行工作项。
> 每项格式：目标 / 方案 / 涉及文件 / 依赖 / 验收 / 工作量。ID 编号 PHx-xx，实施完成一项即在 requirements.md 或 feature-gap.md 勾选并注明 PH 编号。
> 技术基线：C++20 + Qt6（Core/Widgets/Network/Sql）+ libssh 0.10.6（FetchContent 构建，mbedTLS 后端，当前 WITH_GSSAPI=OFF）+ libvterm。测试走 tests/CMakeLists.txt 现有 COMMON_SOURCES 模式。

---

## 0. 总路线图与依赖关系

| 阶段 | 内容 | 预估工时 | 前置 |
|------|------|----------|------|
| Phase 0 架构地基 | 配置系统、传输抽象、输入注入器、Tab 持久化、构建依赖调整 | 2 周 | 无 |
| Phase 1 P0 核心体验 | 断点续传、代理、Jump Host、鼠标协议、主题、快速连接、ssh config 导入、标签页增强、密钥管理器 | 4-6 周 | Phase 0 |
| Phase 2 P1 追平主流 | reflow、时间戳/折叠/大纲、链接/OSC52/告警、Quickbar、命令面板、同步输入、SCP/ZMODEM、GSSAPI、known_hosts、2FA、Agent 扩展、会话增强 | 6-10 周 | Phase 0 |
| Phase 3 P2 远期按需 | Telnet/Serial/mosh/Raw/RDP/VNC/X11/ControlMaster、脚本宏、插件、FIDO2、运维套件、gRPC、i18n 等 | 持续 8-16 周 | Phase 1 |

核心依赖图：

- 传输抽象 PH0-02 ← 出站代理 PH1-02、Jump Host PH1-03、Telnet PH3-01、串口 PH3-02、mosh PH3-03、Raw TCP PH3-04
- 配置系统 PH0-01 ← 主题 PH1-05、代理 PH1-02、告警 PH2-05、密钥 PH1-09 等全部新配置项
- 输入注入器 PH0-03 ← 命令面板 PH2-07、同步输入 PH2-08、自由输入 PH2-08、脚本 PH3-08
- Tab 持久化 PH0-04 ← 会话恢复 PH2-15
- KeyStore 封装 PH0-05 ← 密钥管理器 PH1-09、known_hosts PH2-12
- Agent 策略 PH2-14 ← 白名单授权、敏感操作二次确认
- 会话增强 PH2-15 依赖 SessionRepository 扩展（tags/克隆/快速命令字段）

---

## 1. Phase 0 —— 架构地基（先行，所有后续依赖）

### PH0-01 配置系统升级 + 设置对话框（落地 Settings 空壳） ✅ 已完成（2026-08-14，test_config 14/14 通过）
- 现状：Config 仅 10 个键；主菜单 Settings 和工具栏 Settings 都是空壳（MainWindow.cpp:153、229 无 handler）。
- 目标：所有新功能配置项集中管理，提供可视化设置界面。
- 方案：
  1. utils/Config.h 增加"键注册表"：struct ConfigKey { key, type(Enum: Bool/Int/String/Password/Color/Font/Enum), defaultValue, group, label, enumOptions }；静态注册表 + 访问器。
  2. 新增 SettingsDialog（QDialog + QTreeWidget 分组 + 动态表单，按注册表自动生成）。
  3. 配置变更发信号（Config::valueChanged(key)），各组件订阅即时生效。
  4. 预留键：terminal/fontFamily、terminal/fontSize、terminal/backgroundOpacity、theme/name、terminal/colorScheme、net/proxyType、net/proxyHost、net/proxyPort、net/proxyUser、net/proxyPassEnc（主密码加密）、session/quickConnect、agent/policy、alerts/... 等（后续各 PH 各自注册）。
- 涉及：src/utils/Config.{h,cpp}、src/app/dialogs/SettingsDialog.{h,cpp}、src/app/MainWindow.cpp、CMakeLists.txt。
- 依赖：无。
- 验收：Settings 菜单打开设置框；新键注册即出现；改动即时生效；密码键走 Crypto 加密存储。
- 工作量：3-4 天。

### PH0-02 传输抽象层（ITransport） ✅ 已完成（2026-08-14，SshSession 实现 ITransport + TransportFactory + SessionType 扩展）
- 现状：SessionType 只有 Ssh/Local；SshSession 直接绑定 libssh；SessionTab/TerminalSession 直接持有 SshSession。
- 目标：连接类功能（代理、Jump、Telnet、Serial、Raw）可插拔，SessionTab 不感知具体协议。
- 方案：
  1. 新接口 src/core/transport/ITransport.h：信号 dataReceived/stateChanged/errorOccurred/connectionLost；方法 connectToHost/disconnect/write/resize/isConnected。
  2. SshSession 实现 ITransport（或加 SshTransport 适配器，最小改动：让 SshSession 继承 ITransport）。
  3. TerminalSession/SessionTab 改为持有 ITransport*（工厂 ITransportFactory::create(SessionConfig)）。
  4. SessionConfig::SessionType 扩展 Telnet/Serial/Raw（占位，对应 transport 在 PH3 实现）；isValid/displayName/toMap 同步。
- 涉及：src/core/transport/（新目录）、SessionConfig.{h,cpp}、SshSession.{h,cpp}、SessionTab.cpp、TerminalSession.{h,cpp}、SshShellProcess.cpp（改为走 ITransport 或保持 SshSession 直用）。
- 依赖：无。
- 验收：现有 SSH/本地终端功能回归通过（tests/test_sshsession、test_terminal）；新增协议不影响 SSH 路径。
- 工作量：3-5 天。

### PH0-03 输入注入器（InputBroadcaster） ✅ 已完成（2026-08-14，命令发送器已改走广播）
- 现状：命令发送器在 MainWindow::onSendCommand 单点实现；无统一分发。
- 目标：命令发送器/同步输入/自由输入/脚本共用一套"向 N 个 tab 注入输入"框架。
- 方案：src/app/InputBroadcaster.{h,cpp}（单例）：注册活跃 tab（SessionTab 提供 sendInputForBroadcast），支持目标过滤（全部/选中组/白名单）；注入前可做确认弹窗；返回成功/失败明细。MainWindow 的 onSendCommand 改为调用它。
- 涉及：InputBroadcaster、MainWindow.cpp、SessionTab（暴露广播接口）。
- 验收：现有"发送命令到所有终端"行为不变；可编程过滤。
- 工作量：1-2 天。

### PH0-04 Tab 状态持久化与崩溃恢复 ✅ 已完成（2026-08-14）
- 现状：退出不保存打开标签；崩溃后无恢复。
- 目标：重启可选恢复上次会话（SSH 会话 id 列表 + 本地终端 shell 类型 + 分屏布局）。
- 方案：MainWindow 退出时写 QSettings ui/openTabs（[ {type, sessionId|shellType} ]）+ 各 tab 终端滚动位置（可后补）；启动时弹"恢复上次会话？"；崩溃检测（启动时存在 crash marker 则提示）。
- 涉及：MainWindow.{h,cpp}、SessionRepository（按 id 批量取 SessionConfig）。
- 验收：重启恢复标签并自动重连；本地终端恢复 shell。
- 工作量：2-3 天。

### PH0-05 KeyStore / knownhosts 封装（libssh pki 基座） ✅ 已完成（2026-08-14，编译通过；knownhosts 用弃用 API，PH2-12 迁移）
- 目标：为密钥管理器、known_hosts、ppk 导入提供统一底层。
- 方案：src/core/KeyStore.{h,cpp}：密钥生成（ssh_pki_generate：ED25519/RSA/ECDSA）、导入导出（ssh_pki_import_privkey_file / ssh_pki_export_pubkey 到 authorized_keys 格式）、指纹计算（ssh_get_publickey_hash + ssh_get_hexa）、knownhosts 读写（ssh_options_set SSH_OPTIONS_KNOWNHOSTS + ssh_is_server_known + ssh_write_knownhost）；私钥目录 HSSH_KEYS_DIR（Windows %APPDATA%/hssh/keys，其余 ~/.hssh/keys），主密码加密。
- 涉及：src/core/KeyStore.{h,cpp}、CMakeLists.txt。
- 验收：单测覆盖生成/导入/指纹/knownhosts 判定。
- 工作量：3-4 天。

### PH0-06 构建与第三方依赖调整 ✅ 完成（2026-09-22：HSSH_WITH_GSSAPI 选项联动 libssh、Qt6 WebSockets 可选探测、HSSH_HAS_SSH_BIN/HSSH_SSH_BIN 探测；SerialPort 槽位已被串口原生 API 实现替代不需要）
- 目标：为后续特性预置依赖与开关。
- 方案：
  1. CMakeLists：find_package Qt6 COMPONENTS 增加 SerialPort、WebSockets（可选，find 不到则 disable 对应特性宏）；Qt Charts 可选项（监控面板 PH3-16 用自绘曲线则不需要）。
  2. FetchLibSSH.cmake：WITH_GSSAPI 改为选项 HSSH_WITH_GSSAPI（默认 OFF，开启时 Windows 链 security.lib / Linux 链 krb5），为 PH2-11 准备。
  3. 新增特性宏：HSSH_HAS_SERIAL、HSSH_HAS_WEBSOCKETS、HSSH_HAS_GSSAPI、HSSH_HAS_SSH_BIN（探测系统 ssh.exe/ssh 路径，Jump 备选与 ControlMaster 用）。
- 涉及：CMakeLists.txt、cmake/FetchLibSSH.cmake、cmake/（新增 Find 逻辑）。
- 验收：无 Qt 模块的机器上仍可构建（特性自动关闭）。
- 工作量：1-2 天。



## 2. Phase 1 —— P0 核心体验

### PH1-01 SFTP 断点续传 ✅ 已完成（2026-08-14，下载/上传/目录续传 + 保留断点文件）
- 目标：传输中断（断网/取消）后可续传，不重传已完成部分。
- 方案：SftpSession::doDownload/doUpload 改造：
  1. 下载：先 sftp_stat 远端文件大小；本地目标不存在则从 0 开始，存在且小于远端则 sftp_seek(remote, localSize) 续传（写入 .part 文件，完成改名）；大于/等于视为已完（可选校验大小）。
  2. 上传：sftp_stat 远端 + sftp_seek(remote, localOffset) 续传；远端不存在则覆盖写入。
  3. 传输前冲突处理：SftpWidget 弹"覆盖 / 跳过 / 续传 / 重命名"对话框（新增 TransferConflictDialog）。
  4. TransferRegistry/TransfersWidget 增加 offset 展示（已传 x / 断点 y）。
- 涉及：src/core/SftpSession.cpp（doDownload/doUpload）、src/app/widgets/SftpWidget.cpp、src/app/TransferRegistry.{h,cpp}、新 src/app/dialogs/TransferConflictDialog。
- 依赖：无。
- 验收：构造 50MB 文件传一半断网重连，续传字节数与校验和正确；tests/test_sftp.cpp 增加续传用例（sftp_test_server.py 支持 seek 场景）。
- 工作量：3-4 天。

### PH1-02 出站 HTTP/SOCKS5 代理 ✅ 已完成（2026-09-20，编译树验证；真机带凭据链路待用户实测）
- 目标：SSH 连接可走 HTTP CONNECT / SOCKS5 出站代理（需求 3.4 未实现项）。
- 方案：
  1. SessionConfig 加 netProxy 字段组（type: None/Http/Socks5、host、port、user、pass 加密）；NewSessionDialog 增加代理页。
  2. 新 src/core/ProxyConnector.{h,cpp}：QTcpSocket 实现 HTTP CONNECT（含 Proxy-Authorization Basic）与 SOCKS5（RFC 1928：握手/认证/连接请求）。
  3. SshConnect.cpp：代理开启时先 ProxyConnector 连到目标，成功后取 socket 句柄，ssh_options_set(session, SSH_OPTIONS_FD, &fd) 再 ssh_connect（libssh 0.10.6 支持直接使用已有 fd 完成握手）。注意句柄交接：Qt 端 detach（setSocketDescriptor(-1) 或先 close），防止双关。
  4. 备选路径（FD 不可用时）：SSH_OPTIONS_PROXYCOMMAND 调外部 ncat/socat（探测 HSSH_HAS_*）。
- 涉及：SessionConfig.{h,cpp}、NewSessionDialog、SshConnect.cpp、ProxyConnector（新）、SshSession.cpp（连接路径复用）。
- 依赖：PH0-01（配置键）、PH0-02（传输抽象不强制）。
- 验收：HTTP 代理与 SOCKS5 代理下 ssh_connect 成功；无代理时回归正常；错误信息包含代理阶段。
- 工作量：3-4 天。

### PH1-03 Jump Host / ProxyJump ✅ 已完成（2026-09-20，编译树验证；真机带凭据链路待用户实测）
- 目标：经跳板机连内网主机（需求 3.4 未实现项）。
- 方案（纯 libssh 双会话 + 本地转发桥，不依赖 OpenSSH）：
  1. SessionConfig 加 jumpHost/jumpUser/jumpPort/jumpAuthMethod/jumpPassword(加密)/jumpPrivateKey；NewSessionDialog 增加跳板页。
  2. SshConnect.cpp 重构为两步：worker 线程上先建"跳板机 ssh_session"（复用现有 authenticate），在其上开 Local Forward（本机 127.0.0.1 随机端口 → 目标 host:port）；再对目标机 ssh_session 用 SSH_OPTIONS_FD 连接该本地端口完成握手。两段都在同一 worker 线程阻塞执行。
  3. 失败路径：跳板认证失败/转发失败给出分段错误信息。
  4. 支持多级跳板（v1 只做单级，字段设计为列表以便扩展）。
- 涉及：SessionConfig.{h,cpp}、NewSessionDialog、SshConnect.{h,cpp}、PortForward.cpp（复用 addForward 或内部直接 ssh_channel_open_forward）。
- 依赖：PH1-02 的 ProxyConnector/SSH_OPTIONS_FD 基建。
- 验收：经跳板机连接内网主机成功；跳板密码/密钥两种认证可用；失败提示定位到跳板段。
- 工作量：4-5 天。

### PH1-04 终端鼠标协议 ✅ 已完成（2026-09-20，编译树验证 ctest 10/10）
- 目标：vim/tmux/htop 等支持鼠标的应用可点击、拖选、滚动（需求 3.2 未实现项）。
- 方案（libvterm）：
  1. TerminalWidget 跟踪鼠标模式：解析 \x1b[?1000;1002;1003;1006h/l 序列（或 libvterm 内部 mouse mode API），维护枚举 None/Click/Drag/Move + SGR 标志。
  2. mousePress/mouseMove/mouseRelease：模式非 None 时调用 vterm_mouse_send_button / vterm_mouse_send_move（含 modifier 与 1006 SGR 编码），输出走 dataToSend；模式为 None 时维持现有选择逻辑。
  3. 滚轮：Drag/Move 模式下发 \x1b[<64/65;...;...M（SGR 滚动），否则现有滚动。
  4. 配置项 terminal/mouseProtocol（开/关，默认开）。
- 涉及：src/terminal/TerminalWidget.{h,cpp}、utils/Config（PH0-01）。
- 依赖：PH0-01。
- 验收：vim 内鼠标点击移动光标、拖选可视块、滚轮翻页；普通 shell 下选择复制不受影响。
- 工作量：3-4 天。

### PH1-05 自定义字体 / 颜色主题 / 背景透明度 ✅ 已完成（2026-09-20，编译树验证 ctest 10/10）
- 目标：需求 3.2 未实现项。
- 方案：
  1. 字体：TerminalWidget 的 QFont 改为从 Config 读 terminal/fontFamily + terminal/fontSize（默认保留现状）；字体变更信号重算 m_cellWidth/m_cellHeight。
  2. 主题：QSS 主题切换（theme/name：dark/light），main.cpp applyDarkTheme 改为按配置加载 resources/styles/dark.qss 或新增 light.qss；ANSI 16 色 + 256 色调色板（terminal/colorScheme，可存 JSON 调色板文件）。
  3. 透明度：terminal/backgroundOpacity（0.6-1.0），TerminalWidget 背景色 QColor alpha 合成（用窗口透明需 Qt::WA_TranslucentBackground，v1 仅背景色 alpha，避免性能与合成问题）。
  4. SettingsDialog 提供字体选择器（QFontDialog）、主题下拉、透明度滑块、调色板编辑。
- 涉及：src/terminal/TerminalWidget.{h,cpp}、src/main.cpp、resources/styles/light.qss（新）、SettingsDialog、Config。
- 依赖：PH0-01。
- 验收：改字体/主题/透明度即时生效；深色/浅色两套主题无白底黑字错乱。
- 工作量：3-4 天。

### PH1-06 快速连接栏 ✅ 已完成（2026-09-20，编译树验证 ctest 10/10；含会话 Duplicate 与测试实例基建 --port/--no-restore/HSSH_DEBUG_LOG）
- 目标：不建会话直连（对标 gap#31）。
- 方案：MainWindow 工具栏加 QLineEdit（占位 "user@host:port"），回车解析 → 临时 SessionConfig（不落库）→ 走 SessionTab 创建连接；支持 history（QComboBox editable + completer，存 Config session/quickConnectHistory）。
- 涉及：MainWindow.cpp、SessionTab。
- 依赖：无。
- 验收：输入 root@10.0.0.1:2222 回车即连；历史可回选。
- 工作量：1 天。

### PH1-07 OpenSSH .ssh/config 导入 ✅ 已完成（2026-09-20，编译树验证 ctest 10/10；含会话 Duplicate 与测试实例基建 --port/--no-restore/HSSH_DEBUG_LOG）
- 目标：复用运维存量 ssh 配置（对标 gap#7，需求未列出）。
- 方案：
  1. 新 src/core/SshConfigParser.{h,cpp}：解析 ~/.ssh/config 与用户指定文件；支持 Host/HostName/User/Port/IdentityFile/IdentitiesOnly/ProxyJump/ProxyCommand/ServerAliveInterval/Compression/LocalForward/RemoteForward 等指令；Host 通配（* ?）与段继承（Include 展开）。
  2. 映射到 SessionConfig（含新 jump 字段 PH1-03）；IdentityFile 存在则 authMethod=PublicKey。
  3. SessionManagerWidget 导入菜单增加"从 SSH Config 导入…"：预览列表（勾选）→ 批量写入 SessionRepository。
- 涉及：SshConfigParser（新）、SessionRepository（importFromSshConfig）、SessionManagerWidget、MainWindow。
- 依赖：PH1-03（jump 字段映射）。
- 验收：典型 ssh config（含通配与 LocalForward）导入后连接可用；tests/test_sshconfig.cpp 单测覆盖解析。
- 工作量：3-4 天。

### PH1-08 标签页增强：分离/固定/着色/序号切换 ✅ 完成（2026-09-20 分组/固定/克隆/Ctrl+数字；2026-09-21 拖出浮窗 FloatingTabWindow + Dock 回归 + 右键 Detach 菜单）
- 目标：对标 gap#10、11、33。
- 方案：
  1. MainWindow 换用自定义 TabBar（QTabBar 子类 SessionTabBar）：拖出（mouseMove 到屏幕边缘→新 QMainWindow 浮窗承载该 SessionTab，支持拖回）、固定（右键 Pin，置顶且不可关闭）、着色（连接状态/告警变色）、Ctrl+1..9 切换。
  2. 浮窗类 FloatingTabWindow（QMainWindow，持有单个 SessionTab，关闭即断开或归位）。
  3. Tab 菜单：复制标签（clone，PH2-15）、重命名、颜色。
- 涉及：src/app/SessionTabBar.{h,cpp}（新）、src/app/FloatingTabWindow.{h,cpp}（新）、MainWindow.cpp。
- 依赖：无。
- 验收：拖出/拖回无闪退；固定标签不参与滚动关闭；Ctrl+数字跳转。
- 工作量：3-4 天。

### PH1-09 SSH 密钥管理器（含 ppk 导入） ◐ 核心完成（2026-09-21：KeyManagerDialog + KeyStore 高层 API + 会话引用 + test_keystore 8 用例；生成仅 Ed25519——libssh mbedTLS 后端 pki_private_key_to_pem 是返回 NULL 的桩，RSA/ECDSA 私钥导出不可用；ppk 导入拆到 B7）
- 现状：主菜单 Key Manager 空壳；需求 3.6 未实现项。
- 方案：
  1. 新 src/app/dialogs/KeyManagerDialog：列表（名称/类型/指纹/路径）、生成（ED25519/RSA-2048/4096/ECDSA-256/384，可设 passphrase）、导入（OpenSSH/PEM/ppk）、导出公钥（复制 authorized_keys 行）、删除；私钥存 KeyStore（PH0-05），主密码加密。
  2. ppk 支持：实现 ppk v2/v3 解析（读 PuTTY 私钥格式：magic 头、加密 AES-256-CBC、解密→转 OpenSSH 或直接用 libssh 导入）；无 puttygen 依赖（自实现），Windows 有 puttygen 时提供"用 puttygen 转换"备选。
  3. 会话认证增加"使用密钥管理器中的密钥"（authMethod=PublicKey + keyId 引用，或注册到 libssh agent 模拟）。
- 涉及：KeyManagerDialog（新）、src/core/KeyStore.{h,cpp}（PH0-05）、src/core/PpkParser.{h,cpp}（新）、NewSessionDialog、MainWindow（Key Manager 菜单接上）。
- 依赖：PH0-05。
- 验收：生成→复制公钥→远端登录成功；ppk v2/v3 导入后可直接认证；主密码锁定时不可导出私钥。
- 工作量：5-7 天。

### PH1-10 SSH Agent 转发（ForwardAgent） ✅ 已完成（2026-09-20，编译树验证；真机带凭据链路待用户实测）
- 现状：需求 3.1 未实现项；libssh 无 auth-agent channel API。
- 方案（两档）：
  1. v1（务实）：检测会话开启 ForwardAgent 时，回退用系统 OpenSSH 子进程承载该连接（ssh -A -t user@host），终端桥接子进程 IO —— 功能可用但绕开 libssh，仅该会话生效；标注依赖本机 ssh.exe（Windows 10+ / macOS / Linux 自带）。
  2. v2（纯 libssh 深水区）：自研 agent channel —— 监听 sshd 发来的 auth-agent-req@openssh.com 请求（channel request 回调），打开到本机 ssh-agent 的 unix socket / Windows named pipe（\\.\pipe\openssh-ssh-agent），双向泵数据。风险高，先做可行性 spike（1-2 天）再决定。
- 涉及：SessionConfig（forwardAgent 字段）、SshSession.cpp、新 src/core/AgentForward.{h,cpp}、NewSessionDialog。
- 依赖：PH0-02（可选）。
- 验收：远端 ssh-add -L 能列出本机 agent 密钥；git clone 走 agent 认证成功。
- 工作量：v1 2-3 天 / v2 5-8 天。



## 3. Phase 2 —— P1 追平主流

### PH2-01 宽度变化重排（reflow） ✅ 完成（2026-09-21：ScrollbackEntry.wrapped 启发式 + 列宽感知重切 + 150ms 防抖；选择丢弃、搜索重跑；宽字形不劈半。测试锁定往返一致性）
- 现状：需求 3.2 未实现项；libvterm 无 reflow API；resize 只改行列。
- 方案（懒重排，保留 live screen）：
  1. ScrollbackLine 增加 wrap 标记（行由哪个逻辑行 wrap 而来）；resize 时仅对 scrollback 部分按新列宽重新软换行（合并 wrap 行→重切），live screen 交由 libvterm 自身 resize（保留屏幕，溢出由 screenSbPushLine 接管）。
  2. 重新映射选择/搜索行号（逻辑行索引不变，物理行变化）。
  3. 时机：resizeEvent 防抖（150ms）后执行，避免拖动窗口卡顿。
- 涉及：src/terminal/TerminalWidget.{h,cpp}（ScrollbackLine、resizeEvent、选择/搜索坐标系）。
- 依赖：无。
- 验收：拖宽/拖窄窗口后历史行按新宽度正确换行；屏幕内容与光标不丢；选择/搜索定位不偏移。
- 工作量：5-8 天（本阶段风险最高项）。

### PH2-02 时间戳 / 输出折叠 / 大纲视图 ◐ ①时间戳+③大纲完成（2026-09-21：11 列 gutter + arriveMs/damage 时间戳 + TerminalOutlineWidget dock）；②折叠单独排期（选择/滚动坐标耦合，风险高）
- 现状：需求 3.2 未实现项。
- 方案：
  1. 时间戳：ScrollbackLine 加 arriveMs 字段；terminal/showTimestamps 开关 → 行首渲染 HH:MM:SS（渲染时前缀，不改 bufferText 输出）。
  2. 折叠：按"空行/提示符边界"切分为 Region；折叠区标题取首行；点击行首折叠标记展开/收起；折叠不改变数据，仅渲染裁剪。
  3. 大纲：新 TerminalOutlineDock（QDockWidget），正则提取标题行（默认：^\S+@\S+[:~] 提示符、make 的 Entering directory、日志时间前缀），点击滚动定位；TerminalWidget 提供 rowClicked 信号回跳。
- 涉及：src/terminal/TerminalWidget.{h,cpp}、新 src/app/widgets/TerminalOutlineWidget.{h,cpp}、MainWindow（dock 注册）。
- 依赖：PH0-01（开关键）。
- 验收：时间戳可开关；折叠/展开正确且滚动不乱；大纲点击定位准确。
- 工作量：5-7 天。

### PH2-03 链接可点击（URL / 路径） ✅ 完成（2026-09-21：linkAt + Ctrl+Click + hover 手型 + 右键菜单；路径类 token 不误报）
- 目标：对标 gap#12。
- 方案：TerminalWidget hover 时 cellAtPosition → lineTextRange 提取当前 token，正则匹配 http(s)://、file://、邮箱、/绝对路径、./相对路径；Ctrl+Click 触发（URL 用 QDesktopServices::openUrl；本地/远端路径：本地 QDesktopServices 打开，远端提示下载）；右键菜单加"打开链接"。hover 命中时切换手型光标。
- 涉及：TerminalWidget.{h,cpp}（eventFilter/hover 追踪）、MainWindow（信号处理）。
- 依赖：无。
- 验收：终端中 URL 可 Ctrl+点击打开；路径高亮提示。
- 工作量：2-3 天。

### PH2-04 OSC 52 剪贴板同步 ✅ 完成（2026-09-21：嗅探状态机 + decodeOsc52 纯函数 + deny/prompt/allow 三态；本地→远端查询方向有意不响应）
- 目标：对标 gap#13。
- 方案：TerminalWidget::feedData 解析 OSC 52 序列（\x1b]52;[c];base64(\x07|ESC\），按 terminal/osc52Mode（禁止/允许读/允许读写，默认允许读）决定是否写剪贴板；同时支持"本地复制 → 发 OSC 52 给远端"（配合 tmux set-clipboard）。
- 涉及：TerminalWidget.{h,cpp}、Config。
- 依赖：PH0-01。
- 验收：tmux 中复制内容进入本机剪贴板；模式为禁止时不生效。
- 工作量：1-2 天。

### PH2-05 关键字告警 + 命令完成通知
- 目标：对标 gap#14、15。
- 方案：
  1. src/core/AlertRule.{h,cpp}：规则（名称、正则、目标匹配流、动作：高亮/托盘通知/声音/标签页变色）；告警配置页并入 SettingsDialog。
  2. TerminalSession 输出流逐块跑规则；命中→高亮（TerminalWidget 加 alertHighlight 集合）+ QSystemTrayIcon::showMessage（新增托盘图标，最小化常驻）+ 标签变色（PH1-08 复用）。
  3. 命令完成通知：利用 screenBell 回调（已有）+ 静默判定（shell 提示符正则 \$|# 出现且距上次输入 >N 秒）触发系统通知；设置开关。
- 涉及：src/core/AlertRule.{h,cpp}、TerminalSession.cpp、TerminalWidget.{h,cpp}、MainWindow（托盘）、SettingsDialog。
- 依赖：PH0-01、PH1-08。
- 验收：规则命中变色+通知；长命令结束提醒一次。
- 工作量：3-4 天。

### PH2-06 快捷命令栏（Quickbar）
- 目标：对标 gap#9。
- 方案：SessionConfig 加 quickCommands（[{label, command, confirm(bool)}]）；SessionTab 底部 QuickCommandBar（QToolBar 可折叠），点击注入命令（支持变量 {host}/{user}/{port}）；全局快速命令（Config quickCommands 全局默认）。
- 涉及：SessionConfig.{h,cpp}、新 src/app/widgets/QuickCommandBar.{h,cpp}、SessionTab.cpp、NewSessionDialog（编辑快速命令）、SettingsDialog（全局）。
- 依赖：PH0-01、PH0-03（注入走广播）。
- 验收：按钮点击在目标终端执行命令；confirm 项弹确认；变量替换正确。
- 工作量：2-3 天。

### PH2-07 命令面板（Command Palette） ✅ 完成（2026-09-21：CommandPalette + Ctrl+Shift+P + 菜单/会话/标签三数据源 + 最近使用持久化加权）
- 目标：需求 3.5 未实现项。
- 方案：新 CommandPalette（QDialog 无边框 + QLineEdit + QListView，fuzzy 过滤）：注册项=所有 QAction + 快速命令 + 会话列表（跳转/连接）+ 命令发送器项；Ctrl+Shift+P 唤起；支持最近使用排序。
- 涉及：src/app/widgets/CommandPalette.{h,cpp}、MainWindow.cpp。
- 依赖：PH0-03。
- 验收：模糊搜索直达操作；回车执行、Esc 关闭。
- 工作量：2-3 天。

### PH2-08 同步输入 + 自由输入模式 ✅ 完成（2026-09-20 同步输入；2026-09-21 自由输入模式：工具栏 Free Type 切换 + 全 SSH 标签镜像 + 确认/状态提示）
- 目标：需求 3.5 两项未实现项。
- 方案：
  1. 同步输入（Sync Input）：会话右键/工具栏勾选多个 tab → 输入广播到选中组（InputBroadcaster 目标过滤）；组内每个终端各自回显，输入原样注入（警告：vim/密码场景不同步）。
  2. 自由输入模式（Free Type Mode）：进入后键盘输入同时发到所有已连接 SSH tab（Xshell 模式）；退出恢复；工具栏切换按钮 + 状态提示。
- 涉及：InputBroadcaster（PH0-03）、MainWindow.cpp、SessionTab。
- 依赖：PH0-03。
- 验收：多 tab 同步输入一致；自由输入模式开关清晰，退出不残留。
- 工作量：2 天。

### PH2-09 SCP 支持
- 目标：需求 3.3 未实现项。
- 方案：libssh ssh_scp 全套（ssh_scp_new SSH_SCP_WRITE/READ、push/pull、递归 -r 用 ssh_scp_accept 遍历）；封装 SshScpSession（与 SftpSession 同构：worker 线程 + 进度信号），SftpWidget 传输方式选项加"SCP"；目录传输走现有 listDirRecursive + scp 文件流。
- 涉及：src/core/SshScpSession.{h,cpp}（新）、SftpWidget.cpp、TransferRegistry。
- 依赖：无。
- 验收：scp 上传/下载/目录与 openssh scp 互通；进度正确。
- 工作量：3-4 天。

### PH2-10 ZMODEM / YMODEM / XMODEM（rz/sz） ✅ 接收方向完成（字节级验收全过）；◐ 发送方向（ZFILE/ZRPOS/ZDATA 链路已通，数据子包落盘细节待校准——ZNAK 根因已定位 ZCRCW→ZCRCX 已修，最后一步 rz 未写文件待查）
- 目标：需求 3.3 未实现项。
- 方案：
  1. 新 src/core/zmodem/ 目录：ZmodemSession 状态机（ZRQINIT/ZRINIT/ZFILE/ZDATA/ZCRC/ZFIN/子包编码/CRC32/超时重传/窗口），YMODEM 用同框架精简。
  2. TerminalSession 集成：输出流检测 ZRQINIT（rz 启动）或用户执行 sz 命令后进入二进制旁路（数据不渲染直接喂协议机）；完成退出旁路。
  3. 上传：文件选择 → 旁路写；下载：sz 触发 → 协议解析 → 本地保存，进度经 TransferRegistry。
  4. XMODEM 一并实现（简单校验和/CRC 模式）。
- 涉及：src/core/zmodem/{ZmodemSession,ZmodemProtocol}.{h,cpp}、TerminalSession.cpp、SftpWidget（入口按钮）、TransfersWidget。
- 依赖：无（协议纯实现）。
- 验收：rz 上传 / sz 下载与 lrzsz 互通，大文件（>10MB）校验一致；中断恢复重传（v1 不做断点，仅重传整文件）。
- 工作量：8-12 天（协议状态机，单独排期）。

### PH2-11 GSSAPI / Kerberos 认证
- 目标：需求 3.1 未实现项。
- 方案：
  1. FetchLibSSH 开启 WITH_GSSAPI（Windows 链 security.lib 用 SSPI；Linux 链 krb5 需 libkrb5-dev）；cmake 选项 HSSH_WITH_GSSAPI。
  2. SessionConfig::AuthMethod 加 Gssapi；SshConnect::authenticate 走 ssh_userauth_gssapi（libssh 0.10.6 支持 gssapi-with-mic）；NewSessionDialog 认证方式下拉增加。
- 涉及：cmake/FetchLibSSH.cmake、SessionConfig、SshConnect.cpp、NewSessionDialog。
- 依赖：PH0-06。
- 验收：域环境下 Kerberos 免密登录成功；无 GSSAPI 环境构建不受影响。
- 工作量：2-3 天 + 环境验证。

### PH2-12 known_hosts 指纹管理 ✅ 完成（2026-09-21：连接前拦截 + TOFU/ask/accept-all 三态 + 变更强告警与旧条目清除 + Key Manager 管理面板；管理入口放 Key Manager 而非设置页）
- 目标：对标 gap#36（需求未明确列出）。
- 方案：基于 KeyStore（PH0-05）：首次连接弹"主机指纹确认"（显示 SHA256/MD5 指纹，保存到 ~/.hssh/known_hosts，libssh ssh_write_knownhost）；主机变更（ssh_is_server_known 返回 SSH_SERVER_KNOWN_CHANGED）弹强告警并可移除旧条目；设置页可查看/删除条目。现有 sshConnectAndAuthenticate 加回调参数返回 knownhost 状态。
- 涉及：KeyStore.{h,cpp}、SshConnect.cpp、NewSessionDialog（指纹页）、SettingsDialog（known_hosts 管理）。
- 依赖：PH0-05。
- 验收：首次连接确认指纹；改主机密钥后提示变更；删除条目后可重新确认。
- 工作量：2-3 天。

### PH2-13 2FA / 键盘交互扩展 ✅ 完成（2026-09-21：kbdint 多轮循环 + 密码轮自动应答 + KbdintPromptDialog 逐项输入/会话级记忆 + headless 明确报错；真机 2FA 服务器验证待做）
- 现状：KeyboardInteractive 走密码通道（需求标注"当前走密码通道"）。
- 目标：支持服务器逐项提示（密码/TOTP/验证码）。
- 方案：SshConnect 增加 kbdint 回调：ssh_userauth_kbdint 循环中把 prompts 收集为列表 → 通过 QMetaObject 跨线程发 GUI 弹窗（KbdintPromptDialog：逐项输入，密码项掩码）→ 回填 answers；SessionConfig 加"记住本会话 2FA"（临时内存缓存，不落盘）。
- 涉及：SshConnect.cpp、新 src/app/dialogs/KbdintPromptDialog.{h,cpp}、SessionConfig。
- 依赖：无。
- 验收：Duo / Google Authenticator 类验证码登录通过；无交互（cli）时返回明确错误。
- 工作量：2-3 天。

### PH2-14 Agent 扩展：ssh_shell / ssh_forward / SSE / WebSocket / 授权
- 目标：需求 4.2/4.3 未实现项集中落地。
- 子项：
  1. MCP ssh_shell：新增工具 ssh_shell_open（返回 shellId，保持 shell channel 常驻，输出缓冲于 AgentSessionRegistry）、ssh_shell_write(shellId, input)、ssh_shell_read(shellId, maxLines)、ssh_shell_close；v2 用 MCP 服务端通知推送增量输出。
  2. MCP ssh_forward：ssh_forward(sessionId, type, local, remote) → 复用 PortForwardManager，返回监听地址与状态；配套 ssh_list_forwards / ssh_remove_forward。
  3. SSE 传输：AgentHttpServer 增加路由 GET /sse + POST /messages（传统 SSE）并实现 MCP Streamable HTTP（2025-03-26）作为首选；复用现有 token 鉴权。
  4. WebSocket：find_package Qt6 WebSockets，/ws 端点，帧协议 {type: shell-input|shell-output|ping}；AgentSession 桥接实时 shell。
  5. 授权白名单 + 二次确认：src/agent/AgentPolicy.{h,cpp}（规则表：host/operation/allow|ask|deny + 记住选项）；AgentHttpServer/AgentMcpServer 执行前查策略，ask 时经 MainWindow 弹确认（复用 confirmSudo 模式）；上传/下载/转发为敏感操作强制二次确认（可配"本次会话免确认"）。
- 涉及：AgentSessionRegistry.{h,cpp}、AgentMcpServer.cpp、AgentHttpServer.{h,cpp}、AgentPolicy（新）、MainWindow.cpp、CMakeLists.txt。
- 依赖：PH0-06（WebSockets 模块）。
- 验收：Claude Code 经 MCP 可 shell 交互与转发；SSE/WS 客户端连通；首次授权→白名单→敏感二次确认全流程。
- 工作量：6-8 天。

### PH2-15 会话增强：克隆 / 收藏 / 标签 / 日志查看器
- 目标：对标 gap#30、32、34。
- 方案：
  1. SessionConfig 加 tags(QStringList) 与 favorite(bool)；SessionRepository/SessionModel 支持（favorite 置顶排序、tags 参与快速搜索）；SessionManagerWidget 右键克隆（复制 SessionConfig 新 id）。
  2. 会话日志查看器 SessionLogViewer：读 AppData/logs/session_*.log，按时间过滤、关键字高亮、导出；从"工具"菜单与最近会话右键打开。
- 涉及：SessionConfig、SessionRepository、SessionModel、SessionManagerWidget、新 src/app/dialogs/SessionLogViewer.{h,cpp}、MainWindow。
- 依赖：无。
- 验收：克隆会话连接正常；收藏置顶；日志按时间检索。
- 工作量：3-4 天。



## 4. Phase 3 —— P2 远期 / 按需

### PH3-01 Telnet 连接
- 目标：对标 gap#1。
- 方案：TelnetTransport 实现 ITransport：QTcpSocket + Telnet 协商子集（IAC WILL/WONT/DO/DONT：SGA、ECHO、NAWS(31) 窗口尺寸、TTYPE(24) 终端类型、LINEMODE 简化）；回车换行转换选项；SessionType::Telnet + 端口默认 23；会话认证/密钥相关字段对 Telnet 隐藏。
- 涉及：src/core/transport/TelnetTransport.{h,cpp}（新）、SessionConfig、NewSessionDialog（协议切换分支）、ITransportFactory。
- 依赖：PH0-02。
- 验收：连 Cisco/网络设备正常交互；Ctrl+] 退出提示（可选）；NAWS 生效。
- 工作量：3-4 天。

### PH3-02 串口 Serial（COM/tty） ✅ 已完成（2026-09-20，编译树验证）
- 目标：对标 gap#2，贴合嵌入式调试场景（AGENTS.md RK3588 串口）。
- 方案：SerialTransport 实现 ITransport：QSerialPort（Qt6 SerialPort 模块）；SessionConfig 加 serialPort/baudRate/databits/parity/stopbits/flowcontrol + 换行转换；SessionType::Serial；NewSessionDialog 串口配置页（可枚举本机端口 QSerialPortInfo）。
- 涉及：src/core/transport/SerialTransport.{h,cpp}（新）、SessionConfig、NewSessionDialog、CMakeLists（HSSH_HAS_SERIAL）。
- 依赖：PH0-02、PH0-06。
- 验收：连接开发板串口收发正常；波特率/流控配置生效；拔插提示。
- 工作量：2-3 天。

### PH3-03 mosh
- 目标：对标 gap#5。
- 方案：依赖本机 mosh 二进制（HSSH_HAS_MOSH 探测）：连接建立后经 SSH 启动 mosh-server（取 MOSH_KEY 与端口），再启 mosh-client（托管子进程）；终端 IO 桥接 mosh-client 伪终端（复用 LocalShellProcess 通道）；断开重连由 mosh 自身保证；窗口尺寸同步（mosh-client 已处理）。
- 涉及：src/terminal/MoshShellProcess.{h,cpp}（新，类似 LocalShellProcess）、SessionConfig（mosh 开关）、TerminalSession。
- 依赖：PH0-02。
- 验收：弱网/切网不断线；Ctrl+C 等交互正常。
- 工作量：4-5 天（依赖外部二进制）。

### PH3-04 Raw TCP
- 目标：对标 gap#6。
- 方案：RawTransport 实现 ITransport：QTcpSocket 透传；选项：换行转换（CR/LF）、hex 显示模式、连接后发送初始化字符串。
- 涉及：src/core/transport/RawTransport.{h,cpp}、SessionConfig、NewSessionDialog。
- 依赖：PH0-02。
- 验收：调试自定义 TCP 协议可收发。
- 工作量：1-2 天。

### PH3-05 RDP / VNC 集成
- 目标：对标 gap#3、4。
- 方案：v1 外部进程：RDP 调 mstsc（Windows）/ xfreerdp（Linux，探测二进制）；VNC 调 vncviewer；会话类型 Rdp/Vnc 仅存配置与启动器。v2 内嵌窗口：FreeRDP 库嵌入 Qt（工作量大，单独评估）。
- 涉及：SessionConfig（Rdp/Vnc）、新 src/core/RemoteDesktopLauncher.{h,cpp}、SessionTab（启动外部进程）。
- 依赖：无。
- 验收：从 hssh 会话列表一键启动 RDP/VNC 客户端。
- 工作量：v1 2 天 / v2 10+ 天。

### PH3-06 X11 转发
- 目标：需求 3.4 未实现项。
- 方案：可行性先确认 libssh 0.10.6 X11 API（ssh_channel_open_x11 存在；服务端→客户端 x11-req 处理若无 API 则自研 channel request 回调 + 本地 X socket 双向泵）；DISPLAY 解析（Windows 下需用户提供 X server 地址，如 VcXsrv/WSLg，标注环境依赖）；SessionConfig x11Forward 开关。
- 涉及：src/core/X11Forward.{h,cpp}（新）、SshSession.cpp、NewSessionDialog。
- 依赖：PH0-05 的 channel 基建。
- 验收：xclock 等远程 GUI 显示到本机 X server。
- 工作量：5-8 天（含 spike）。

### PH3-07 SSH ControlMaster 复用
- 目标：需求 3.1 未实现项。
- 方案（务实评估）：libssh 无 ControlMaster → 方案 A：OpenSSH ControlMaster 后台进程（ssh -MN -S socket host）+ 新连接经 ControlPath socket（SSH_OPTIONS_FD 连接 unix socket，Windows 为 named pipe，OpenSSH 支持）；方案 B：降级为"同一 host 已连接时提示复用标签"。先做 B（零成本）+ A 的 spike。
- 涉及：SessionConfig（controlMaster 选项）、SshConnect.cpp、NewSessionDialog。
- 依赖：PH1-02 的 FD 基建、HSSH_HAS_SSH_BIN。
- 验收：多标签连同一主机仅一次认证。
- 工作量：spike 2 天 + A 4-6 天。

### PH3-08 脚本与宏
- 目标：需求 3.5 未实现项。
- 方案：v1 宏录制器（TerminalSession 记录用户输入时间轴，回放注入）+ 命令序列脚本（JSON 步骤：send/wait-for/regex/expect/sleep，导入导出）；v2 脚本引擎（QJSEngine，需 Qt6 Qml 模块，或轻量 Lua/sol2）提供 send/expect/sleep API 与 UI 编辑。
- 涉及：src/core/MacroRecorder.{h,cpp}（新）、src/core/ScriptRunner.{h,cpp}（新）、MainWindow（录制/回放工具栏）、SettingsDialog（脚本管理）。
- 依赖：PH0-03（注入器）。
- 验收：录制→回放一致；expect 超时控制；脚本执行可中断。
- 工作量：v1 3-4 天 / v2 5-8 天。

### PH3-09 插件系统
- 目标：需求 3.5 未实现项；对标 gap 可扩展性。
- 方案：QPluginLoader + 接口 IPlugin（initialize(hsshApi)、commands()、contextMenuProviders()、transportFactories()、terminalDecorators()）；插件 manifest.json（名称/版本/入口）；扫描目录（应用目录 plugins/ + %APPDATA%/hssh/plugins）；SettingsDialog 插件页（启用/禁用）；API 暴露受限（通过 IPluginContext 封装，禁止任意访问）。
- 涉及：src/plugins/plugin.h（接口）、src/app/PluginManager.{h,cpp}（新）、MainWindow、CMake（BUILD_PLUGINS）。
- 依赖：PH0-01（配置）。
- 验收：示例插件（如"一键部署"命令）加载/卸载生效。
- 工作量：5-8 天。

### PH3-10 FIDO2 / YubiKey（sk-* 密钥）
- 目标：需求 3.6 未实现项（远期）。
- 方案：libssh 不支持 sk-*；检测私钥为 sk- 前缀时回退 OpenSSH 子进程承载连接（同 PH1-10 v1 通道）；GUI 提示插入硬件并交互（设备 touch 由 OpenSSH 提示转发）。
- 涉及：SshConnect.cpp、SshSession.cpp（回退路径）。
- 依赖：HSSH_HAS_SSH_BIN。
- 验收：sk-ed25519 密钥经 OpenSSH 通道完成认证。
- 工作量：3-4 天。

### PH3-11 远程文件直接编辑
- 目标：对标 gap#20。
- 方案：SftpWidget 右键"编辑" → SftpSession.download 到 %TEMP% → QDesktopServices::openUrl（系统默认编辑器）→ QFileSystemWatcher 监视 mtime → 变化后弹"回传？"→ upload（保留远端权限位）。编辑中锁定该文件并发编辑提示。
- 涉及：SftpWidget.cpp、SftpSession.cpp（临时文件句柄）、新 src/core/RemoteFileEdit.{h,cpp}。
- 依赖：无。
- 验收：改文件保存自动回传；回传后远端内容一致。
- 工作量：2-3 天。

### PH3-12 远程文件搜索
- 目标：对标 gap#21。
- 方案：复用 SftpSession::listDirRecursive 目录树 + 名称/大小/时间过滤；新 SearchDialog（输入关键字、递归深度、大小范围）；后台线程 + 进度；结果双击定位 SftpWidget。
- 涉及：SftpSession.cpp（find 扩展）、新 src/app/dialogs/RemoteSearchDialog.{h,cpp}、SftpWidget。
- 依赖：无。
- 验收：万级文件目录搜索不卡 UI；结果可跳转。
- 工作量：3 天。

### PH3-13 服务器 ↔ 服务器直传
- 目标：对标 gap#22。
- 方案：双 SftpSession 内存管道（下载流→上传流，每 64KB 块泵送，进度取较小者）；UI：双远程地址选择；不支持则提示走本地中转（现已有）。大文件建议直传（SFTP 无 FXP，只能流式）。
- 涉及：src/core/DirectTransfer.{h,cpp}（新）、SftpWidget/TransfersWidget（入口）。
- 依赖：无。
- 验收：两台远程主机间文件传输内容一致。
- 工作量：3-4 天。

### PH3-14 chmod / chown UI
- 目标：对标 gap#23。
- 方案：SftpWidget 右键"权限…"：rwx 九位勾选 + 八进制输入 + 递归选项（sftp_chmod）；chown 走 sudo shell 命令（复用 Agent sudoExec 确认弹窗）v1 仅显示属主。
- 涉及：SftpWidget.cpp、SftpSession.cpp（chmod）、新 src/app/dialogs/ChmodDialog.{h,cpp}。
- 依赖：无。
- 验收：改权限立即生效；递归选项正确。
- 工作量：2 天。

### PH3-15 定时任务
- 目标：对标 gap#24。
- 方案：src/core/Scheduler.{h,cpp}：任务（名称、目标会话或主机、命令、间隔或 cron 表达式、是否需 sudo）；QTimer 后台触发 → exec 或注入终端；结果写会话日志；任务面板（列表/启停/日志）。
- 涉及：src/core/Scheduler.{h,cpp}、src/app/dialogs/SchedulerDialog.{h,cpp}、MainWindow。
- 依赖：PH0-03（可选）。
- 验收：定时命令按计划执行并留痕。
- 工作量：3-4 天。

### PH3-16 服务器监控面板（CPU/内存/磁盘/网络）
- 目标：对标 gap#25。
- 方案：监控采样线程 SSH exec（cat /proc/stat、/proc/meminfo、/proc/net/dev、df -P）+ 解析；图表用自绘 QPainter 曲线（避免 Qt Charts 依赖）或引入 Qt Charts；新 MonitorDock（QDockWidget 可停靠）；断开自动停止。
- 涉及：src/core/ServerMonitor.{h,cpp}（新）、src/app/widgets/MonitorWidget.{h,cpp}（新）、MainWindow。
- 依赖：无。
- 验收：实时曲线刷新；低开销（采样间隔 >=1s）。
- 工作量：4-5 天。

### PH3-17 进程管理 UI
- 目标：对标 gap#26。
- 方案：ps -eo pid,ppid,user,%cpu,%mem,stat,etime,cmd 解析表格；刷新/终止（SIGTERM）/强杀（SIGKILL）；需 sudo 走 sudoExec 确认。
- 涉及：src/app/widgets/ProcessWidget.{h,cpp}（新）、MainWindow（dock）。
- 依赖：无。
- 验收：进程列表准确；kill 生效。
- 工作量：2-3 天。

### PH3-18 Docker 管理
- 目标：对标 gap#27。
- 方案：封装 docker CLI over SSH（docker ps -a --format json、images、logs --tail、start/stop/restart/rm、exec 命令）；新 DockerWidget 面板；连接复用现有会话。
- 涉及：src/app/widgets/DockerWidget.{h,cpp}（新）、MainWindow。
- 依赖：无。
- 验收：容器启停与日志查看可用。
- 工作量：3-4 天。

### PH3-19 网络工具套件
- 目标：对标 gap#28。
- 方案：面板集成 ping/traceroute/ss -tunlp/netstat/端口探测（nc 或 bash /dev/tcp）；参数表单 → SSH exec → 输出面板（可中断）；与 Agent exec 复用。
- 涉及：src/app/widgets/NetworkToolsWidget.{h,cpp}（新）、MainWindow。
- 依赖：无。
- 验收：工具输出实时滚动、可停止。
- 工作量：2-3 天。

### PH3-20 剪贴板历史
- 目标：对标 gap#42。
- 方案：QClipboard 监听环形缓冲（默认 50 条，终端复制项带来源 tab 标记）；托盘/面板查看与回填；可选加密。
- 涉及：src/app/ClipboardHistory.{h,cpp}（新）、MainWindow。
- 依赖：无。
- 验收：历史可检索回填；隐私开关。
- 工作量：2 天。

### PH3-21 密码管理器集成
- 目标：对标 gap#38。
- 方案：v1 KeePassXC（keepassxc-cli 或 KeePassXC 浏览器协议）与 1Password CLI（op）：NewSessionDialog 按钮"从密码管理器获取"；凭据不落盘。探测二进制可用性。
- 涉及：NewSessionDialog.cpp、src/core/PasswordManagerClient.{h,cpp}（新）。
- 依赖：外部 CLI。
- 验收：从 KeePass/1Password 拉取密码完成连接。
- 工作量：3-4 天。

### PH3-22 gRPC 服务
- 目标：需求 4.2.3 未实现项。
- 方案：grpc C++ + protobuf（cmake find_package(gRPC)）：ssh.proto（Connect/Exec/Shell/Upload/Download/Forward/Disconnect/ListSessions）；服务端监听 127.0.0.1:8223，复用 AgentSessionRegistry 与 AgentPolicy；token 元数据鉴权。
- 涉及：proto/ssh.proto（新）、src/agent/GrpcServer.{h,cpp}（新）、CMakeLists。
- 依赖：PH2-14（策略）。
- 验收：grpcurl 调通 Exec/Upload。
- 工作量：5-7 天。

### PH3-23 i18n / 无障碍 / 全局热键
- 目标：需求 6 非功能项。
- 方案：i18n：补全 zh_CN.ts 翻译、运行时语言切换（QTranslator reload）；无障碍：高对比主题、tab 焦点顺序、控件 accessibleName；全局热键：Windows RegisterHotKey / Linux X11（引入 QHotkey 单头文件库），配置"全局唤起/锁定"。
- 涉及：resources/i18n/*.ts、src/main.cpp、MainWindow、SettingsDialog。
- 依赖：PH0-01。
- 验收：中文界面无英文残留；高对比主题可用；全局热键唤起。
- 工作量：3-5 天。

---


### PH2-16 Tmux 集成
- 目标：需求 3.5 未实现项；对标 gap 中 tmux 场景。
- 方案：
  1. 会话快捷操作：一键发送 tmux 前缀（Ctrl+B）、tmux new/window/select 快捷按钮（Quickbar 预置模板）。
  2. tmux 感知粘贴：检测会话内 tmux（TERM=tmux* 或标题提示）时，多行粘贴走 tmux load-buffer 模式（避免多行逐行回显错乱）。
  3. pane 感知：可选解析 tmux status-left/right（\x1b]2; 标题）展示 pane 列表，点击切换（v2）。
- 涉及：src/core/TmuxSupport.{h,cpp}（新，检测与命令构造）、QuickCommandBar（PH2-06）、TerminalSession.cpp。
- 依赖：PH2-06。
- 验收：tmux 会话内快捷键/粘贴行为正确；非 tmux 不受影响。
- 工作量：2-3 天。

### PH3-24 云资源浏览器（低优先，可决策不做）
- 目标：对标 gap#8（AWS S3 / 云主机列表）。
- 方案：v1 仅"云主机一键导入会话"（AWS CLI / hcloud 等探测 + 生成会话）；S3 浏览器不做。产品决策：与"完全本地"定位冲突时关闭本项。
- 涉及：SessionRepository（import 通道）、NewSessionDialog。
- 依赖：外部云 CLI。
- 验收：AWS 主机列表导入为会话。
- 工作量：3-4 天。

### PH3-25 终端细节体验补充（合并小项）
- 目标：对标 gap#16（连字）、#17（背景图/模糊）、#19（双击选词/中键粘贴）、#41（拖文件到终端即 rz 上传）、#43（SSH 服务端模式，默认不做）。
- 方案：
  1. 连字：QFont::setStyleStrategy(PreferNoShaping 关闭) / 用 Qt 支持的连字字体（仅当字体含连字表）——v1 标注"依赖字体"，低优先。
  2. 背景图：TerminalWidget 背景绘制 QPixmap（平铺/拉伸 + 透明度合成），配置项 terminal/backgroundImage；与 PH1-05 透明度合并实现。
  3. 双击选词（单词级）、中键粘贴（X11/Wayland 下 Qt 原生支持，Windows 关）。
  4. 拖文件到终端 → 检测活动 SSH 会话 → 自动执行 rz 并上传（复用 PH2-10 协议机）；无 rz 则提示。
  5. SSH 服务端模式（本机被外部连入）：默认不做，标记为产品决策项。
- 涉及：TerminalWidget.{h,cpp}、TerminalSession.cpp、SftpWidget/SftpSession（rz 上传复用）。
- 依赖：PH1-05、PH2-10。
- 验收：各项按开关生效；默认关闭不改变现有行为。
- 工作量：3-5 天。

## 5. 依赖与第三方组件变更清单

| 依赖 | 用途 | 引入时机 | 备注 |
|------|------|----------|------|
| Qt6 SerialPort | 串口 PH3-02 | PH3-02 | find_package 可选，缺省编译仍通过 |
| Qt6 WebSockets | WS 端点 PH2-14 | PH2-14 | 同上 |
| OpenSSH 客户端（系统自带） | Agent 转发 v1 / FIDO2 / ControlMaster 备选 | PH1-10 | Windows 10+ / macOS / Linux 均自带；HSSH_HAS_SSH_BIN 探测 |
| libssh WITH_GSSAPI | GSSAPI PH2-11 | PH2-11 | Windows 链 security.lib（SSPI） |
| mosh（外部二进制） | PH3-03 | PH3-03 | 不可用时该特性灰显 |
| puttygen（可选） | ppk 转换备选 | PH1-09 | 优先自研 PpkParser |
| Qt6 Qml/QJSEngine 或 Lua/sol2 | 脚本引擎 PH3-08 | PH3-08 | 可选，v1 先宏+JSON 步骤 |
| grpc/protobuf | PH3-22 | PH3-22 | 可选编译 |

## 6. 测试与验收策略

- 单元测试（tests/ 现有 COMMON_SOURCES 模式，add_test + TEST_ENV）：
  - PH1-01 续传：tests/test_sftp.cpp 增补 seek 用例（配合 tests/sftp_test_server.py）。
  - PH1-07：tests/test_sshconfig.cpp（解析/通配/继承）。
  - PH1-09：tests/test_keystore.cpp（生成/导入/指纹/ppk 解析向量）。
  - PH2-10：tests/test_zmodem.cpp（协议状态机 + 内存管道对拍 lrzsz 样本）。
  - PH2-04/03：tests/test_terminal.cpp 增补 OSC52 捕获、链接正则。
  - PH0-01：tests/test_config.cpp 增补键注册表。
- 集成测试：SshSession/SftpSession 对 tests/sftp_test_server.py 回归；新增 sshd 容器（可选 CI）。
- GUI 手工验收清单：每个 PH 附"验收"行，纳入 docs/manual-test-checklist.md（实施时补充）。
- 非功能（需求 6）：启动时间/内存基准脚本（Phase 3 维护项）、断网重连自动化（现有 test_sshsession 扩展）、覆盖率（Linux gcov / Windows OpenCppCoverage）。

## 7. 里程碑汇总

| 里程碑 | 内容 | 交付物 | 预估 |
|--------|------|--------|------|
| M1 | PH0-01..06 全部 | 配置系统+设置框、传输抽象、输入注入器、Tab 恢复、KeyStore、构建开关 | 2 周 |
| M2 | PH1-01..10 | 续传/代理/Jump/鼠标/主题/快速连接/ssh config/标签页/密钥管理器/Agent 转发 | 4-6 周 |
| M3 | PH2-01..15 | 终端增强+协议+安全+Agent 扩展+会话增强 | 6-10 周 |
| M4 | PH3-01..23 | 协议矩阵+运维套件+插件+gRPC+i18n | 持续 8-16 周 |

## 8. 文档同步任务

- 每完成一个 PH，同步勾选：docs/requirements.md（对应 [ ] 项）、docs/feature-gap.md（对应对标项）、README.md（功能列表）、docs/implementation-plan.md（本表状态）。
- 新增 docs/manual-test-checklist.md 作为 GUI 验收基线。
- 需求文档建议补录"主流工具未列出"功能（feature-gap.md 第二部分的 43 项）进 requirements.md 第 3 节对应小节，标注优先级。




