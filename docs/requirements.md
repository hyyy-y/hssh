# HSSH 需求文档

> **项目代号**：hssh  
> **目标**：打造一款完全开源、跨平台、功能全面的 GUI SSH 客户端，并内置 Agent 能力，可被 Claude Code 及其他外部工具调用。

---

## 1. 项目概述

### 1.1 项目背景

当前主流 SSH 客户端存在以下痛点：
- **WindTerm**：部分开源，主程序源码未完全开放，存在闭源风险。
- **Tabby**：Electron 架构，资源占用高，启动慢。
- **PuTTY / KiTTY**：UI 老旧，缺少现代终端特性（标签页、文件管理、美观主题）。
- **MobaXterm / Xshell / FinalShell**：商业软件或含广告，非完全开源。
- **OpenSSH**：纯命令行，无 GUI，新手不友好。

### 1.2 项目目标

开发一款名为 **hssh** 的 GUI SSH 客户端，具备：
1. **主流 SSH 工具的所有核心功能**。
2. **原生性能**，低内存、低延迟、快速启动。
3. **完全开源**，采用 Apache-2.0 协议。
4. **内置 Agent 能力**，提供标准化 API，可被 Claude Code、IDE、脚本等外部工具调用。

### 1.3 目标平台

| 平台 | 优先级 |
|------|--------|
| Windows 10/11 | P0 |
| Linux (Ubuntu/Debian/CentOS) | P0 |
| macOS (Intel/Apple Silicon) | P1 |

---

## 2. 参考竞品

| 工具 | 类型 | 开源 | 优点 | 缺点 |
|------|------|------|------|------|
| WindTerm | GUI | 部分开源 | 功能全面、高性能 | 主程序闭源 |
| Tabby | GUI | MIT | 现代 UI、插件丰富 | Electron、资源占用高 |
| PuTTY | GUI | MIT | 轻量、老牌 | 无标签页、UI 老旧 |
| KiTTY | GUI | 开源 | PuTTY 增强版 | 仅 Windows |
| MobaXterm | GUI | 商业 | 功能全 | 非开源、免费版有限制 |
| Xshell | GUI | 商业 | 稳定 | 收费 |
| FinalShell | GUI | 商业 | 集成文件管理 | 含广告、非开源 |
| OpenSSH | CLI | 开源 | 标准实现 | 无 GUI |
| Windows Terminal | GUI | MIT | 官方、现代 | 只是终端，无 SSH 管理 |

---

## 3. 核心功能需求

### 3.1 连接与会话管理

- [x] 支持 SSH v2 协议。
- [x] 支持多种认证方式：
  - 密码认证
  - 公钥认证（RSA / ECDSA / ED25519）
  - 键盘交互认证（keyboard-interactive，当前走密码通道）
  - GSSAPI / Kerberos 认证（未实现）
  - SSH agent 转发（未实现）
- [x] 会话管理器：
  - [x] 分组/标签树形结构
  - [x] 快速搜索会话
  - [x] 会话导入/导出（JSON，SessionRepository::importSessionsFromJson / exportSessionsToJson）
  - [x] 会话历史记录（session_history 表 + 最近会话菜单）
- [x] 自动登录：
  - [x] 保存用户名/密码（可选主密码加密）
  - [x] 自动执行登录后命令（SessionConfig::postLoginCommands）
- [x] 连接复用：
  - [ ] SSH ControlMaster 支持
  - [x] 连接保活（KeepAlive，SshSession::startKeepAliveLibSsh）
  - [x] 断线自动重连（SshSession 自动重连 + 终端回车重连）

### 3.2 终端仿真

- [x] 支持 VT100 / VT220 / VT340 / VT420 / VT520 / Xterm / Xterm-256color（libvterm）。
- [x] 支持 Unicode、Emoji、True Color（libvterm）。
- [ ] 支持鼠标协议。
- [x] 支持滚动回滚（可配置行数，无限制）。
- [x] 支持多标签页（分屏未实现）。
- [x] 支持搜索、高亮、选中复制、右键粘贴。
- [ ] 支持时间戳、输出折叠、大纲视图。
- [ ] 支持自定义字体、颜色主题、背景透明度。
- [ ] 调整宽度时内容重排（reflow）。
- [x] 支持 Powerline / Oh-My-Zsh / Oh-My-Posh 字体渲染（libvterm）。
- [x] 支持本地 Shell：
  - Windows：Cmd / PowerShell / WSL（PowerShell 管理员未实现）
  - Linux/macOS：bash / zsh / fish

### 3.3 文件传输

- [x] 内置 SFTP 客户端：
  - [x] 上传 / 下载 / 删除 / 重命名
  - [x] 拖拽传输（SftpWidget 拖放上传）
  - [x] 断点续传（PH1-01，单文件与目录均支持，取消/失败保留断点文件）
  - [x] 批量队列（TransfersWidget）
- [ ] 支持 SCP。
- [ ] 支持 ZMODEM / YMODEM / XMODEM（rz/sz）。
- [x] 本地文件管理器集成。
- [x] 文件对比与同步（FileCompareWidget：目录对比、Diff 预览、批量同步，额外实现）。

### 3.4 端口转发与代理

- [x] 本地端口转发（Local Forward）。
- [x] 远程端口转发（Remote Forward）。
- [x] 动态端口转发（SOCKS5 Proxy / Dynamic Forward）。
- [ ] HTTP / SOCKS5 代理支持。
- [ ] Jump Host / ProxyJump / ProxyCommand。
- [ ] X11 转发。

### 3.5 高级功能

- [ ] 命令面板（Command Palette）。
- [x] 命令发送器（批量发送命令到多个会话）。
- [ ] 同步输入（Sync Input，多会话同时输入）。
- [ ] 自由输入模式（Free Type Mode）。
- [x] 专注模式（Focus Mode）。
- [x] 会话日志记录（自动记录到 AppData/logs，Config `session/logging` 开关）
- [x] 屏幕锁定（Lock Screen）。
- [ ] 脚本与宏（Script / Macro）。
- [ ] 插件系统（Plugin System）。
- [ ] Tmux 集成。

### 3.6 安全

- [x] 主密码保护（AES-256-GCM 加密用户数据）。
- [ ] SSH 密钥管理器（生成、导入、导出、Agent 转发）。
- [ ] 支持 FIDO2 / YubiKey（未来扩展）。
- [x] 不收集用户数据，完全本地运行。

---

## 4. Agent 功能需求（重点）

### 4.1 设计目标

hssh 不仅是一个 GUI 客户端，还应该是一个**可被外部工具调用的 SSH Agent 服务**。外部工具（如 Claude Code、VS Code、自定义脚本）可以通过标准化接口让 hssh 执行远程操作，而无需自己实现 SSH 协议。

### 4.2 接口方式

#### 4.2.1 MCP 服务器（Model Context Protocol）

- [x] 实现 MCP 服务器（stdio），暴露以下能力：
  - [x] `ssh_connect(host, user, auth)`：建立 SSH 连接
  - [x] `ssh_exec(session_id, command)`：执行远程命令
  - [ ] `ssh_shell(session_id, command)`：启动交互式 Shell
  - [x] `ssh_upload(session_id, local_path, remote_path)`：上传文件
  - [x] `ssh_download(session_id, remote_path, local_path)`：下载文件
  - [ ] `ssh_forward(session_id, type, local, remote)`：端口转发
  - [x] `ssh_disconnect(session_id)`：断开连接
  - [x] `list_sessions()`：列出活跃会话
  - [x] `get_public_key()`：获取 Agent 加密公钥（用于密码加密传输）
  - [x] `ssh_sudo(session_id, command)`：sudo 提权执行（GUI 确认弹窗）
- [x] 支持 STDIO（SSE 待实现）MCP 传输方式。
- [x] 提供 MCP 配置文件示例，方便 Claude Code 接入（见 docs/agent.md）。

#### 4.2.2 REST API

- [x] 本地 HTTP 服务（默认 `127.0.0.1:8222`，可配置）。
- [x] 提供 RESTful API，返回 JSON。
- [x] API 鉴权：Token / Basic Auth / mTLS（Token 已实现）。
- [ ] 支持 WebSocket 进行实时 Shell 交互。

#### 4.2.3 gRPC 接口（可选）

- [ ] 定义 `.proto` 文件。
- [ ] 提供 gRPC 服务，适合高性能脚本调用。

#### 4.2.4 CLI 子命令

- [x] 提供 `hssh cli exec --host ... --command ...` 等非交互式命令。
- [x] 支持 JSON 输出，方便脚本解析。
- [x] 提供 `hssh cli cipher <text>` 子命令（用 Agent 公钥加密密码）。

### 4.3 Agent 安全模型

- [x] Agent 默认关闭，需用户手动启用（可勾选随 GUI 自启）。
- [x] 仅监听 `127.0.0.1`，不暴露到公网。
- [x] 密码不明文传输（RSA 密文通道 / 会话名引用 / 库内凭据不出进程）。
- [x] sudo 操作需用户确认（GUI 弹窗，会话内免重复）。
- [ ] 所有远程操作需用户授权（首次使用时弹窗确认，可配置白名单）。
- [ ] 敏感操作（上传/下载/端口转发）需要二次确认。
- [x] 操作审计日志：记录所有通过 Agent 执行的命令和文件传输（AppData/logs/agent_audit.log）。

### 4.4 与 Claude Code 集成示例

用户配置 Claude Code 的 MCP 服务器后，可以在 Claude Code 中直接说：

```
通过 hssh 在 192.168.1.10 上执行 uname -a
```

Claude Code → 调用 hssh MCP → hssh 使用已保存的会话连接 → 执行命令 → 返回结果。

---

## 5. 技术选型建议

### 5.1 编程语言

| 方案 | 优点 | 缺点 |
|------|------|------|
| **C++ + Qt6** | 原生性能、成熟 GUI、跨平台 | 开发效率较低 |
| **Rust + Tauri / Iced / egui** | 安全、现代、高性能 | 生态较新 |
| **Python + PyQt/PySide6** | 开发快、生态丰富 | 性能一般、打包大 |
| **Go + Fyne / Wails** | 并发强、编译快 | GUI 生态较弱 |

**推荐**：C++ + Qt6 或 Rust + Tauri/Iced。

### 5.2 SSH 库

- **libssh**：功能全面，C 库，Qt/Rust 都可绑定。
- **libssh2**：更轻量，但功能不如 libssh。
- **russh**：Rust 原生 SSH 库（如果选 Rust）。
- **OpenSSH 可执行文件**：调用 ssh.exe / ssh 命令（简单但不优雅）。

**推荐**：C++ 方案用 libssh，Rust 方案用 russh。

### 5.3 终端仿真

- **libvterm**：成熟终端仿真库（Neovim 使用）。
- **xterm.js**：如果基于 Web 技术。
- 自研终端仿真器（工作量大）。

**推荐**：libvterm。

### 5.4 配置与数据存储

- 配置文件：YAML / TOML / JSON。
- 用户数据：SQLite（会话、历史、审计日志）。
- 密钥：系统密钥环（Windows Credential / macOS Keychain / Linux Secret Service）。

---

## 6. 非功能性需求

- [ ] **性能**：启动时间 < 1s（Windows 冷启动），大文件传输速度不低于 FileZilla。
- [ ] **内存**：空闲内存占用 < 150MB。
- [ ] **稳定性**：7x24 小时连接不崩溃，断网自动重连。
- [ ] **可扩展性**：插件 API 设计清晰，支持第三方扩展。
- [ ] **可维护性**：代码结构清晰，单元测试覆盖率 > 60%。
- [ ] **国际化**：支持中文、英文，架构上易于扩展其他语言。
- [ ] **无障碍**：支持高对比度主题、键盘快捷键。

---

## 7. 里程碑规划

| 阶段 | 目标 | 时间 |
|------|------|------|
| **MVP** | 基本 SSH 连接、终端仿真、会话管理 | 2-3 个月 |
| **v0.2** | SFTP、端口转发、多标签/分屏 | 1-2 个月 |
| **v0.3** | 主题、快捷键、插件系统 | 1-2 个月 |
| **v0.4** | Agent 功能（MCP + REST API） | 2 个月 |
| **v1.0** | 稳定性优化、跨平台完善、文档 | 2 个月 |

---

## 8. 目录结构建议

```
hssh/
├── docs/                   # 文档
│   └── requirements.md     # 本文件
├── src/
│   ├── app/                # GUI 主程序
│   ├── core/               # 核心逻辑（SSH、SFTP、端口转发）
│   ├── terminal/           # 终端仿真
│   ├── agent/              # Agent 服务（MCP/REST/gRPC）
│   ├── plugins/            # 插件系统
│   └── utils/              # 工具类
├── tests/                  # 单元测试/集成测试
├── thirdparty/             # 第三方依赖
├── resources/              # 图标、主题、i18n
├── CMakeLists.txt          # 或 Cargo.toml
└── README.md
```

---

## 9. 待决策事项

1. 是否采用 Qt6 还是 Rust + Tauri？
2. Agent 默认端口是否使用 `8222`？
3. 是否内置 SFTP 双栏文件管理器？
4. 是否支持插件热加载？
5. 是否优先支持 Windows 再扩展 Linux/macOS？

---

## 10. 附录

### 10.1 术语表

| 术语 | 说明 |
|------|------|
| SSH | Secure Shell，安全外壳协议 |
| SFTP | SSH File Transfer Protocol |
| SCP | Secure Copy Protocol |
| MCP | Model Context Protocol，Anthropic 提出的模型上下文协议 |
| Agent | 可被外部程序调用的服务进程 |
| ControlMaster | OpenSSH 连接复用机制 |
| ProxyJump | SSH 跳转主机 |

### 10.2 参考链接

- WindTerm: https://github.com/kingToolbox/WindTerm
- Tabby: https://github.com/Eugeny/tabby
- PuTTY: https://www.putty.org/
- KiTTY: https://www.9bis.net/kitty/
- OpenSSH: https://www.openssh.com/
- MCP: https://modelcontextprotocol.io/
- libssh: https://www.libssh.org/
- libvterm: https://www.leonerd.org.uk/code/libvterm/
