# hssh Agent 使用说明

hssh 内置 Agent 能力，可通过 MCP (stdio) 或本地 REST API 被 Claude Code、IDE 和脚本调用。

## 1. MCP 服务器（推荐 Claude Code 使用）

以无界面模式启动 MCP stdio 服务器：

```bash
hssh --agent-mcp
```

### Claude Code 接入配置

在 Claude Code 的 MCP 配置（如 `~/.claude.json` 或项目 `.mcp.json`）中加入：

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

> 如果 `hssh` 不在 PATH 中，请使用绝对路径。

### MCP 工具

| 工具 | 参数 | 说明 |
|------|------|------|
| `list_sessions` | - | 列出当前管理的 SSH 会话 |
| `ssh_connect` | `sessionName`/`sessionId`（库内凭据，首选）；或 `host, port?, username?, authMethod?, passwordCipher?, privateKeyPath?, keyPassphraseCipher?` | 建立 SSH 连接，返回 `sessionId` |
| `ssh_exec` | `session_id`, `command` | 在会话上执行命令，返回 `{output, exitCode}` |
| `ssh_upload` | `session_id`, `local_path`, `remote_path` | 通过 SFTP 上传文件 |
| `ssh_download` | `session_id`, `remote_path`, `local_path` | 通过 SFTP 下载文件 |
| `ssh_disconnect` | `session_id` | 断开并释放会话 |
| `get_public_key` | - | 返回 Agent RSA 公钥 `{publicKeyPem, fingerprint}` |
| `ssh_sudo` | `session_id`, `command` | headless 一律报错：sudo 需要 GUI 用户确认 |

> 明文 `password` 参数一律拒绝，密码只用 `passwordCipher`（公钥见 `get_public_key` / `/api/v1/keys`）。

示例：在 Claude Code 中说"通过 hssh 在 192.168.1.10 上执行 uname -a"，Claude 会调用 `ssh_connect` + `ssh_exec`。

## 2. REST API

```bash
# 无界面模式，默认 127.0.0.1:8222
hssh --agent-http [--port 8222]

# 或在 GUI 中通过 工具 → Start Agent 启动
```

### 接口

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/keys` | Agent RSA 公钥 `{publicKeyPem, fingerprint, cipher}`（用于生成密文） |
| GET | `/api/v1/status` | 服务状态、版本、活跃会话 |
| GET | `/api/v1/sessions` | 会话列表 |
| POST | `/api/v1/sessions` | 创建会话：`{sessionName}` / `{sessionId}`（库内凭据，首选）；或 `{host, port?, username?, authMethod?, passwordCipher?, privateKeyPath?, keyPassphraseCipher?}`，返回 `{sessionId}` |
| POST | `/api/v1/sessions/<id>/exec` | 执行命令，body: `{command}`，返回 `{output, exitCode}` |
| POST | `/api/v1/sessions/<id>/upload` | 上传文件，body: `{localPath, remotePath}` |
| POST | `/api/v1/sessions/<id>/download` | 下载文件，body: `{remotePath, localPath}` |
| DELETE | `/api/v1/sessions/<id>` | 断开并删除会话 |

> **密码安全**：明文 `password` 字段一律 400 拒绝。密码只能用 `passwordCipher`（`base64(RSA-OAEP-SHA256(明文))`，公钥见 `/api/v1/keys`）或会话名引用库内凭据。可用 `hssh cli cipher <text>` 在本地产密文。

```bash
curl http://127.0.0.1:8222/api/v1/status
curl -X POST http://127.0.0.1:8222/api/v1/sessions \
     -H "Content-Type: application/json" \
     -d '{"sessionName":"root"}'
curl -X POST http://127.0.0.1:8222/api/v1/sessions/<id>/exec \
     -H "Content-Type: application/json" -d '{"command":"uname -a"}'
```

### Token 鉴权

在 hssh 配置（`agent/token` 键）中设置 Token 后，除 `/api/v1/status` 外所有请求需携带：

```
Authorization: Bearer <token>
```

## 3. GUI 标签页控制（AI 驱动 SSH 窗口）

GUI 模式下启动 Agent 后（Tools → Start Agent，或勾选 Auto-start Agent with GUI），外部 AI 工具（Claude Code、kimi、自定义脚本）可以直接**读取和控制已打开的 SSH/本地终端标签页**：

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/tabs` | 列出打开的终端标签 `[{index,title,type,connected}]` |
| POST | `/api/v1/tabs` | `{shellType?}` 打开本地终端（CMD/PowerShell/WSL/...）；`{session:"名字"}` 打开已保存的 SSH 会话（按会话名/主机/id 匹配），返回 `{index}` |
| GET | `/api/v1/tabs/<i>/text?lines=N&from=M` | 读取终端缓冲区文本（`from` 跳过前 M 行，`lines` 限制末尾 N 行，缺省全部） |
| POST | `/api/v1/tabs/<i>/send` | 发送命令（自动追加回车），body: `{command}` |
| POST | `/api/v1/tabs/<i>/secure-input` | 密文写入终端（密码输入用），body: `{cipher}` |
| POST | `/api/v1/tabs/<i>/sudo` | sudo 协议：弹窗向用户确认后执行并自动输密码，body: `{command, passwordCipher?, useStoredCredential?, timeout?}` |
| DELETE | `/api/v1/tabs/<i>` | 关闭标签页（断开并移除） |

### sudo 协议（用户确认门禁）

AI 工具的 sudo 命令必须走 `/api/v1/tabs/<i>/sudo`：

1. hssh 弹窗向用户展示**完整命令**和目标会话，用户点 Allow 才执行；同一标签页生命周期内只弹一次；30 秒未操作视为拒绝（403）
2. 自动处理密码提示（密文 `passwordCipher` 或库内凭据 `useStoredCredential`，均可缺省）
3. 默认 60 秒等待输出，返回 `{output, timedOut}`；出现密码提示但无凭据时返回 428 `passwordRequired`

MCP（headless）下 `ssh_sudo` 一律报错——sudo 必须有 GUI 弹窗确认。

**长输出注意**：终端滚动缓冲区上限 10000 行，超出丢弃。一次性长输出请用 `/sessions/<id>/exec`（完整 stdout 无上限）；持续性数据流用 `from=<游标>&lines=0` 轮询排水。

典型用法（在 GUI 里开好 SSH 窗口后，让 AI 接管）：

```bash
# AI 找到 SSH 标签
curl http://127.0.0.1:8222/api/v1/tabs
# 读取终端内容
curl "http://127.0.0.1:8222/api/v1/tabs/1/text?lines=50"
# 发送命令到 SSH 会话
curl -X POST http://127.0.0.1:8222/api/v1/tabs/1/send \
     -H "Content-Type: application/json" -d '{"command":"uname -a"}'
# 再读一次，AI 即可拿到命令输出
curl "http://127.0.0.1:8222/api/v1/tabs/1/text?lines=10"
```

> Claude Code 可通过 Bash 工具直接调用这些接口；也可以配置 `hssh --agent-mcp` 作为 MCP 服务器（注意 MCP 进程管理的是它自己创建的会话，与 GUI 标签页相互独立）。

## 4. CLI 子命令

非交互式命令执行，JSON 输出（适合脚本解析）：

```bash
hssh cli exec --host HOST [--port 22] [--username USER] \
              [--auth-method password|publickey|agent] [--password-cipher CIPHER] [--private-key K] \
              --command "uname -a"

# 用公钥本地生成密文
hssh cli cipher <text>
# {"cipher":"...","fingerprint":"..."}
```

密码只允许密文（`--password-cipher`）；TTY 交互模式下不传密码参数则无回显提示输入。

输出示例：

```json
{"ok":true,"exitCode":0,"output":"Linux web-1 6.8.0-45-generic #45-Ubuntu SMP ...\n"}
```

连接失败时退出码为 1，输出 `{"ok":false,"error":"..."}`。

## 5. 安全模型

- 默认仅监听 `127.0.0.1`，不暴露到公网。
- Token 鉴权可随时开启。
- 所有会话数据仅保存在本机。
