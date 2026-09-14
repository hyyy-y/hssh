# HSSH 功能差距分析（未实现清单 + 主流工具对标）

> 生成日期：本次分析基于 `docs/requirements.md` 与当前代码核验（`src/` 全量 grep）。
> 结论摘要：需求文档已列出未实现功能 **29 项**；主流 SSH 工具具备但需求文档**完全未列出**的功能 **约 30+ 项**（见第二部分）。

---

## 一、需求文档已列出但未实现（已逐项核验代码）

### 1.1 连接与会话

| 功能 | 说明 / 核验证据 |
|------|----------------|
| SSH ControlMaster 连接复用 | 代码无 ControlMaster 相关逻辑 |
| GSSAPI / Kerberos 认证 | 认证路径只有 password / publickey / keyboard-interactive / agent |
| SSH agent 转发（ForwardAgent） | README 的 "SSH Agent 认证" 指用本机 agent 里的私钥认证，**不是**把 agent 转发到远端；无 agent 转发实现 |
| 分屏（Split Panes） | 需求 3.2 已标注"分屏未实现" |
| PowerShell 管理员模式 | 本地 shell 支持 Cmd/PowerShell/WSL，无 UAC 管理员启动 |

### 1.2 终端仿真

| 功能 | 说明 / 核验证据 |
|------|----------------|
| 鼠标协议 | `TerminalWidget` 的 mouse 事件仅用于文本选择，未启用 libvterm mouse mode（`vterm_mouse_set_mode`），远端无法收到 `\x1b[<...M` 序列 |
| 时间戳 / 输出折叠 / 大纲视图 | 无实现 |
| 自定义字体、颜色主题、背景透明度 | `Config.cpp` 全量配置键仅 10 个（agent/autostart、lock/password 等），无字体/主题/透明度键；主题硬编码 `dark.qss` + QPalette |
| 宽度变化内容重排（reflow） | `reflow` 全代码库 0 命中；resize 只改行列数不重排 |

### 1.3 文件传输

| 功能 | 说明 / 核验证据 |
|------|----------------|
| SFTP 断点续传 | `SftpSession.cpp` 无 seek / offset / resume 逻辑 |
| SCP | 仅有 SFTP 通道，无 SCP 实现 |
| ZMODEM / YMODEM / XMODEM（rz/sz） | 0 命中 |

### 1.4 端口转发与代理

| 功能 | 说明 / 核验证据 |
|------|----------------|
| HTTP / SOCKS5 出站代理 | `PortForward` 只做本地/远程/动态（入站监听）转发，会话层无出站代理选项 |
| Jump Host / ProxyJump / ProxyCommand | 0 命中（`jump` 仅搜索框跳转） |
| X11 转发 | 0 命中 |

### 1.5 高级功能

| 功能 | 说明 / 核验证据 |
|------|----------------|
| 命令面板（Command Palette） | 有"发送命令到所有终端"，无面板 UI |
| 同步输入（Sync Input） | 0 命中 |
| 自由输入模式（Free Type Mode） | 0 命中 |
| 脚本与宏 | 0 命中 |
| 插件系统 | 需求规划的 `src/plugins/` 目录不存在 |
| Tmux 集成 | 0 命中 |

### 1.6 安全

| 功能 | 说明 / 核验证据 |
|------|----------------|
| SSH 密钥管理器 | 主菜单 **"Key Manager" 是空壳**（`MainWindow.cpp:154` addAction 无 handler）；无密钥生成/导入/导出/格式转换 |
| FIDO2 / YubiKey | 0 命中 |

### 1.7 Agent 功能

| 功能 | 说明 / 核验证据 |
|------|----------------|
| MCP `ssh_shell`（交互式 shell） | 未注册该 tool |
| MCP `ssh_forward`（端口转发） | 未注册该 tool |
| MCP SSE 传输 | 只有 stdio |
| REST WebSocket 实时 shell | 无 WS 端点 |
| gRPC | 无 .proto |
| 操作白名单授权 + 敏感操作二次确认 | 未实现（sudo 有 GUI 确认，但上传/下载/转发无二次确认） |

### 1.8 界面/入口占位（需求文档外、但菜单里是空壳）

- **Settings 菜单项**：`MainWindow.cpp:153` 无 handler，工具栏 229 行同样是空 lambda —— 目前没有任何设置界面。
- 非功能项（启动 <1s、内存 <150MB、7x24 稳定性、覆盖率>60%、无障碍）均未验证。

---

## 二、主流 SSH 工具具备、但需求文档未列出的功能

对标：WindTerm / Xshell / MobaXterm / FinalShell / Termius / SecureCRT / Tabby / PuTTY 系。

### 2.1 协议支持类（结构性缺口，需规划）

| # | 功能 | 代表工具 | 说明 |
|---|------|----------|------|
| 1 | **Telnet** | WindTerm/Xshell/MobaXterm/FinalShell/SecureCRT | 老设备、网络设备调试刚需 |
| 2 | **串口 Serial（COM/tty）** | WindTerm/Xshell/MobaXterm/FinalShell/SecureCRT | 嵌入式/开发板调试刚需 —— 本项目 AGENTS.md 就在用串口调 RK3588 |
| 3 | **RDP 远程桌面** | MobaXterm/FinalShell/Termius | 运维 Windows 服务器 |
| 4 | **VNC** | MobaXterm/Termius/FinalShell | Linux 桌面运维 |
| 5 | **mosh** | Termius/MobaXterm | 弱网/漫游场景 |
| 6 | **Raw TCP 连接** | SecureCRT/MobaXterm/WindTerm | 调试自定义协议 |
| 7 | **OpenSSH `.ssh/config` 导入解析** | Termius/Tabby/Electerm/Royal TS | 复用运维存量配置；hssh 只有自研 JSON 导入导出，无法读现有 ssh 配置 |
| 8 | 云资源浏览器（AWS S3 / 云主机列表） | MobaXterm/Termius | 可选，优先级低 |

### 2.2 终端交互增强类

| # | 功能 | 代表工具 | 说明 |
|---|------|----------|------|
| 9 | **快捷命令栏 / Quickbar（可定制按钮一键执行）** | WindTerm/Xshell/FinalShell | 高频操作按钮化，比命令发送器更细粒度 |
| 10 | **标签页拖出为独立窗口 / 多窗口** | Xshell/WindTerm/MobaXterm/Tabby | 多显示器场景 |
| 11 | **标签页固定 / 着色 / 图标 / 重命名** | Xshell/Tabby/Termius | 会话状态可视化（如报错变红） |
| 12 | **链接可点击（URL / 路径 Ctrl+Click）** | Tabby/Termius/WindTerm | 终端里点网址/文件路径 |
| 13 | **OSC 52 剪贴板同步** | Tabby/iTerm2/Termius | 远端复制内容直接进本机剪贴板 |
| 14 | **关键字告警（输出匹配错误/关键字变色、弹窗、声音）** | Xshell/FinalShell | 日志监控刚需：跑任务盯报错 |
| 15 | **命令完成通知（响铃/系统通知）** | Termius/Tabby | 长任务完成提醒 |
| 16 | 字体连字（Ligatures） | Tabby | 现代终端卖点 |
| 17 | 终端背景图 / 背景模糊 | WindTerm/MobaXterm/Tabby | 主题定制的一部分 |
| 18 | 全局热键 / 快捷唤起 | Xshell/Tabby | |
| 19 | 双击选词、中键粘贴（X11）、拖拽文本进终端 | 多数工具 | 细节体验 |

### 2.3 文件传输 / 运维操作类

| # | 功能 | 代表工具 | 说明 |
|---|------|----------|------|
| 20 | **远程文件直接编辑**（右键远程文件 → 本地编辑器打开保存回传） | FinalShell/Xftp/MobaXterm | 高频刚需 |
| 21 | **远程文件搜索** | FinalShell | 大目录找文件 |
| 22 | **服务器 ↔ 服务器直传** | FinalShell | |
| 23 | chmod / chown / 软链 UI 化 | Xftp/WinSCP | SFTP 面板只有增删改重命名 |
| 24 | **定时任务**（定时执行命令/备份） | FinalShell | |
| 25 | **服务器监控面板**（CPU/内存/磁盘/网络实时曲线） | FinalShell | |
| 26 | 进程管理 UI | FinalShell | |
| 27 | Docker 容器管理 | FinalShell/Tabby 插件 | |
| 28 | 网络工具套件（ping/traceroute/netstat/端口扫描） | MobaXterm/FinalShell | 可与内置 Agent 结合 |

### 2.4 会话 / 数据管理类

| # | 功能 | 代表工具 | 说明 |
|---|------|----------|------|
| 29 | **跨设备同步 + 端到端加密（Vault）** | Termius | 需账号体系，与"完全本地"定位冲突，可做可选 |
| 30 | 会话标签（Tags）、收藏/置顶 | Termius | |
| 31 | **快速连接栏**（直接输入 host:port 即连，不建会话） | Xshell/WindTerm | 轻量入口 |
| 32 | 会话克隆 / 复制 | 多数工具 | |
| 33 | 标签页序号快捷键切换（Ctrl+数字） | Xshell | |
| 34 | **会话日志查看/检索回放** | SecureCRT | hssh 有自动日志（AppData/logs）但无查看器 |
| 35 | 崩溃后会话恢复（reopen tabs） | Tabby/Xshell | |

### 2.5 安全类

| # | 功能 | 代表工具 | 说明 |
|---|------|----------|------|
| 36 | **known_hosts / 主机密钥指纹可视化与管理 UI** | 多数工具 | 首次连接指纹展示、变更告警 |
| 37 | 连接 2FA（服务器支持时） | Termius | |
| 38 | 密码管理器集成（1Password/KeePass） | Termius/SecureCRT | |
| 39 | **密钥格式转换**（OpenSSH ↔ PuTTY ppk 导入） | Xftp/WinSCP/PuTTY | 老用户迁移刚需 |
| 40 | SSH 证书认证（CA 签发证书） | Xshell/SecureCRT | 企业级 |

### 2.6 其它

| # | 功能 | 代表工具 | 说明 |
|---|------|----------|------|
| 41 | 终端拖拽文件即上传（rz 自动触发） | FinalShell/Xshell | 与 2.3 的编辑配套 |
| 42 | 剪贴板历史 | SecureCRT | 可选 |
| 43 | 内置 SSH 服务端模式（本机被管理） | MobaXterm | 低优先 |

---

## 三、建议优先级（结合本项目定位：跨平台 + Agent + 嵌入式调试场景）

### P0 —— 补核心体验，先做（工作量可控、使用频率高）
1. **串口 Serial 支持**（贴合 AGENTS.md 的 RK3588 串口调试场景，WindTerm/Xshell 均内置）
2. **OpenSSH `.ssh/config` 导入解析**（复用运维存量配置，几乎零成本高收益）
3. **分屏（Split Panes）**
4. **出站 HTTP/SOCKS5 代理 + Jump Host**（网络穿透场景）
5. **SFTP 断点续传**
6. **远程文件直接编辑**（右键 → 本地编辑器 → 回传）
7. **标签页分离/固定/着色**
8. **把 Settings 和 Key Manager 两个空壳菜单落地**（密钥管理器需求已列出）

### P1 —— 追平主流
9. 鼠标协议、reflow、自定义字体/主题/透明度（这三项是终端"现代感"的基础）
10. 命令面板 + 同步输入 + 自由输入模式（三者可共用一套输入分发框架）
11. 链接可点击 + OSC 52 + 关键字告警/命令完成通知
12. SCP / ZMODEM（rz/sz）
13. X11 转发
14. known_hosts 指纹管理 + 密钥格式转换（ppk 导入）
15. 快捷命令栏（Quickbar）
16. 脚本与宏（可与 Agent 的 sudoExec/exec 复用）

### P2 —— 远期 / 按需
17. Telnet / mosh / Raw TCP / RDP / VNC（协议矩阵扩展，需架构预留 SessionType 扩展）
18. 服务器监控面板、进程/Docker 管理、定时任务
19. 会话日志查看器、崩溃恢复、会话克隆
20. 插件系统、FIDO2、gRPC / WebSocket、Agent 白名单授权
21. 云资源浏览器、跨设备同步 Vault（与"完全本地"定位冲突，需产品决策）

---

## 四、架构提示（给后续实现）

- **SessionType 已是 enum**（SessionConfig.h），协议矩阵（SSH/Telnet/Serial/Raw）扩展有基础，但 `SshSession`/Agent 层目前只认 SSH，需要一层"传输抽象"。
- **Config 键极少**，建议趁早引入"设置对话框 + 键注册表"，避免主题/字体/代理等新键散落各处。
- **终端与远端交互**（鼠标协议、reflow、OSC52、链接点击）都在 `TerminalWidget` + libvterm 边界，宜集中扩展。
- **发送器/同步输入/自由输入** 建议统一抽象为"输入注入器"，避免三套实现。
