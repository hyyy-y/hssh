---
name: hssh-agent
description: 通过 hssh 的本地 Agent 接口（REST API / MCP）控制 SSH 会话与 GUI 终端窗口。当用户要求在远程服务器上执行命令、上传/下载文件、读取 SSH 终端输出、或让 AI 接管 hssh 里打开的 SSH 窗口时触发此 skill。
userInvocable: true
---

# hssh Agent 使用指南

hssh 是本地运行的 SSH 客户端，内置 Agent 能力。本 skill 描述 AI 如何通过接口控制 SSH 连接和终端窗口。

**前提**：Agent 只监听 `127.0.0.1`，需要在 hssh GUI 中通过 `Tools → Start Agent` 启动（勾选 "Auto-start Agent with GUI" 后自动启动），或命令行 `hssh --agent-http [--port 8222]`。

**两种会话模型，先确认用哪个**：

| 模型 | 场景 | 说明 |
|------|------|------|
| **GUI 标签页控制** | 用户在 hssh GUI 里已经开好 SSH 窗口，AI 接管它 | `/api/v1/tabs/*` 接口，直接读写终端缓冲区 |
| **Agent 托管会话** | AI 自己创建新的 SSH 连接执行命令 | `/api/v1/sessions/*` 接口（或 MCP 工具） |

---

## 一、快速开始（最常用流程）

> AI 接管用户在 GUI 里已经打开的 SSH 窗口：

```powershell
# 1. 确认 agent 在运行
Invoke-RestMethod http://127.0.0.1:8222/api/v1/status

# 2. 列出打开的终端标签（找到 SSH 标签的 index）
Invoke-RestMethod http://127.0.0.1:8222/api/v1/tabs

# 3. 发送命令到该标签（自动追加回车）
Invoke-RestMethod -Method POST http://127.0.0.1:8222/api/v1/tabs/1/send `
    -ContentType "application/json" -Body '{"command":"uname -a"}'

# 4. 等待执行完成，读取终端输出
Start-Sleep -Seconds 2
Invoke-RestMethod "http://127.0.0.1:8222/api/v1/tabs/1/text?lines=50"
```

bash/curl 同理（`curl -X POST ... -d '{"command":"..."}'`）。

**关键原则**：tabs 接口是"读终端缓冲区"，不是同步 RPC——发送命令后必须等输出回来（sleep 后读 text，必要时轮询），再解析结果。

## 二、密码安全模型（必读）

**任何接口都不接受明文密码**。模型/工具只能使用密文或会话名：

| 方式 | 说明 | 适用 |
|------|------|------|
| **会话名引用** | `{"sessionName":"moowbot"}`，hssh 用库内凭据，凭据不出进程 | **首选**，模型连密文都不用见 |
| **密文** | `passwordCipher` = `base64(RSA-OAEP-SHA256(明文))`，公钥见 `GET /api/v1/keys` | 无库存凭据的新主机 |
| 明文 `password` | **一律 400 拒绝** | — |

- 公钥获取：`GET /api/v1/keys` → `{publicKeyPem, fingerprint}`；私钥只存在 hssh 本机（GCM 加密落盘）
- 本地产密文可用 `hssh cli cipher <text>`（输出 JSON 含 cipher 字段）
- 主密码模式下 hssh 未解锁时，密文接口返回 503

## 三、sudo 协议（用户确认门禁）

```powershell
POST /api/v1/tabs/<i>/sudo
{
  "command": "apt update",
  "passwordCipher": "...",        # 可选：密文密码
  "useStoredCredential": true     # 可选：用库内凭据（与上一项二选一，也可都不给）
}
```

流程：
1. **hssh 弹窗向用户展示完整命令和目标会话，用户必须点 Allow 才执行**；同一标签页生命周期内只弹一次；30 秒未操作视为拒绝（403）
2. 发送 `sudo <命令>`；检测到密码提示时自动用密文/库内凭据写入（no-echo，不落缓冲）
3. 等待命令结束（默认 60s，`timeout` 可调），返回 `{output, timedOut}`

特殊返回：
- `403 {"error":"User rejected the sudo request"}` — 用户拒绝/超时
- `428 {"error":"passwordRequired","passwordRequired":true}` — 出现密码提示但无可用凭据，调用方应改用密文重试
- MCP（headless）下 `ssh_sudo` 一律报错：sudo 需要 GUI 确认，headless 不支持

**规则（写入本 skill 强制遵守）**：AI 工具执行任何 sudo 命令**必须走 `/sudo` 接口**，不得用 `/send` 直接发送 `sudo xxx` 绕过用户确认。

---

## 四、REST API 完整参考（默认 `http://127.0.0.1:8222`）

### 4.1 密钥与状态

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/keys` | `{publicKeyPem, fingerprint, cipher:"RSA-OAEP-SHA256+base64"}` |
| GET | `/api/v1/status` | `{status, name, version, sessions}` |

### 4.2 托管会话

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/sessions` | Agent 托管会话列表 |
| POST | `/api/v1/sessions` | `{sessionName}` 或 `{sessionId}`（库内凭据，首选）；或 `{host, port?, username?, authMethod?, passwordCipher?, privateKeyPath?, keyPassphraseCipher?}`，返回 `{sessionId}` |
| POST | `/api/v1/sessions/<id>/exec` | `{command}` → `{output, exitCode}`（同步等待结果） |
| POST | `/api/v1/sessions/<id>/upload` | `{localPath, remotePath}`（SFTP） |
| POST | `/api/v1/sessions/<id>/download` | `{remotePath, localPath}`（SFTP） |
| DELETE | `/api/v1/sessions/<id>` | 断开并释放 |

### 4.3 GUI 标签页控制（GUI 模式启动的 agent 才有）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/tabs` | `[{index, title, type("ssh"/"local"), connected}]` |
| POST | `/api/v1/tabs` | `{shellType?}` 打开本地终端；`{session:"名字"}` 打开已保存会话，返回 `{index}` |
| GET | `/api/v1/tabs/<i>/text?lines=N&from=M` | 读终端缓冲区；`from` 游标，`lines` 末尾行数上限；响应含 `{index, from, lines, text}` |
| POST | `/api/v1/tabs/<i>/send` | `{command}`，自动追加回车 |
| POST | `/api/v1/tabs/<i>/secure-input` | `{cipher}`，密文解密后写入终端（密码输入用） |
| POST | `/api/v1/tabs/<i>/sudo` | sudo 协议（见第三节，用户确认门禁） |
| DELETE | `/api/v1/tabs/<i>` | 关闭标签页（断开并移除） |

终端仿真器的滚动缓冲区**上限 10000 行**，超出的旧行会被丢弃（这是"agent 读不全"的根源）。按场景选择方式：

| 场景 | 正确做法 |
|------|---------|
| **一次性长命令输出**（构建日志、大文件 cat） | 用托管会话 `POST /sessions/<id>/exec`——返回**完整 stdout，无上限**，不受终端缓冲区影响 |
| **持续性数据流**（tail -f、实时监控） | 用游标轮询排水：记下上次读到的行数 cursor，每次 `GET /tabs/<i>/text?from=<cursor>&lines=0`，把新增行追加到自己的缓冲 |
| **全量存档** | 会话日志已自动写到 `%APPDATA%\hssh-project\hssh\logs\session_*.log`（无截断） |

游标轮询示例（PowerShell）：

```powershell
$cursor = 0
while ($true) {
    $r = Invoke-RestMethod "http://127.0.0.1:8222/api/v1/tabs/1/text?from=$cursor&lines=0"
    if ($r.lines -gt 0) {
        $cursor += $r.lines
        $r.text   # 新增内容
    }
    Start-Sleep -Milliseconds 500
}
```

### 2.3 Token 鉴权

配置 `agent/token` 后（hssh 配置文件 `%APPDATA%\hssh-project\hssh.ini`），除 `/api/v1/status` 外需带请求头：

```
Authorization: Bearer <token>
```

---

## 五、MCP 服务器（Claude Code 接入）

配置方式（Claude Code 的 MCP 配置）：

```json
{
  "mcpServers": {
    "hssh": {
      "command": "hssh",
      "args": ["--agent-mcp"]
    }
  }
}
```

工具：`list_sessions`、`ssh_connect`（sessionName/sessionId 或 host+passwordCipher）、`ssh_exec`、`ssh_upload`、`ssh_download`、`ssh_disconnect`、`get_public_key`、`ssh_sudo`（headless 一律报错，sudo 需 GUI 确认）。

> 注意：MCP 是独立进程，管理的是**它自己创建**的会话，看不到 GUI 里打开的窗口。要控制 GUI 窗口、执行 sudo 请用 REST tabs 接口。

---

## 六、CLI 子命令（一次性执行，脚本友好）

```bash
# 执行命令（密码走密文或交互输入，无明文参数）
hssh cli exec --host HOST --username USER --password-cipher CIPHER --command "uname -a"
hssh cli exec --host HOST --username USER --command "uname -a"   # TTY 下无回显输入密码
# {"ok":true,"exitCode":0,"output":"Linux ...\n"}
# 失败时退出码 1，输出 {"ok":false,"error":"..."}

# 用公钥本地生成密文
hssh cli cipher <text>
# {"cipher":"...","fingerprint":"..."}
```

可选参数：`--port`（默认 22）、`--auth-method password|publickey|agent`、`--private-key`、`--password-cipher`。

---

## 七、典型任务模式

### 在 GUI 的 SSH 窗口里执行并确认命令结果

1. `GET /api/v1/tabs` → 找到目标 SSH 标签的 `index`（`type` 为 `ssh`、`connected` 为 true）
2. 先 `GET .../text?lines=5` 记下当前输出末尾（作为基准）
3. `POST .../send` 发送命令
4. 轮询 `GET .../text?lines=50`，直到输出里出现命令回显 + 新的 shell 提示符
5. 解析结果

### 批量在远程机器上跑命令并取结构化结果

用 Agent 托管会话：`POST /sessions` → `POST /sessions/<id>/exec`（同步返回 output/exitCode）→ `DELETE` 清理。比 tabs 接口更适合脚本。

### 文件上传/下载

托管会话：`POST /sessions/<id>/upload` `{localPath, remotePath}`。上传内容体大时注意 HTTP 客户端超时。

---

## 八、错误处理

| 状态码 | 含义 | 处理 |
|--------|------|------|
| 400 | 缺参数（host/command/localPath 等） | 检查 body |
| 401 | Token 鉴权失败 | 带 `Authorization: Bearer` 头 |
| 404 | sessionId/tab index 不存在 | 先 list 确认 |
| 501 | tabs 接口在无 GUI 的 agent 模式下 | 改用托管会话接口 |
| 500 | SSH 连接/执行失败 | 看返回的 error 字段 |

## 九、注意事项

- 所有接口仅监听 `127.0.0.1`，不会暴露到网络。
- `send` 接口自动追加 `\n`（Enter）；需要发控制字符时用原始键值（暂不支持，计划加 `data` 字段）。
- `text` 接口返回终端**可见缓冲区**内容，包含 ANSI 控制序列已处理后的纯文本。
- 同一标签页并发 send 不保证顺序，串行调用即可。
- 托管会话一次只允许一个 exec（并发 exec 会返回错误）。
