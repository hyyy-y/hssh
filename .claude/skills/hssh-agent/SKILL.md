---
name: hssh-agent
description: 通过 hssh 的本地 Agent 接口（REST API / MCP）控制 SSH 会话与 GUI 终端窗口。当用户要求在远程服务器上执行命令、上传/下载文件、读取 SSH 终端输出、或让 AI 接管 hssh 里打开的 SSH 窗口时触发此 skill。
userInvocable: true
---

# hssh Agent 使用指南

hssh 是本地运行的 SSH 客户端，内置 Agent 能力。本 skill 描述 AI 如何通过接口控制 SSH 连接和终端窗口。

**架构（2026-09 起）：一切操作都在 GUI 可视标签页里执行**。原来的 headless 托管会话池（`/api/v1/sessions/*`、MCP 自有连接池、会话收割器）已整体删除——AI 执行的每条命令用户都能在窗口里看到。Agent 只监听 `127.0.0.1`。

**前提**：agent 跑在 hssh GUI 进程里。启动方式：
- GUI 菜单 `Tools → Start Agent`（勾选 "Auto-start Agent with GUI" 后随 GUI 自动启动）
- `hssh --agent`（启动 GUI 并自动开 agent）
- **MCP 模式下不用手动启动**：MCP 进程连不上 agent 时会自动拉起 GUI（`hssh --agent`），最多等 45 秒
- `hssh --agent-http [--port 8222]` 是无 GUI 的 headless 模式，现在只剩 `/keys`、`/status`、`/health` 可用（tabs 全部 501）——除了取公钥基本没用，别用它跑任务

---

## 一、快速开始（最常用流程）

```powershell
# 1. 确认 agent 在运行
Invoke-RestMethod http://127.0.0.1:8222/api/v1/status

# 2. 列出打开的终端标签（找到目标机器的 ref）
Invoke-RestMethod http://127.0.0.1:8222/api/v1/tabs

# 3. 在标签里执行命令（同步返回输出，用户在窗口里能看到命令跑）
Invoke-RestMethod -Method POST http://127.0.0.1:8222/api/v1/tabs/board-a/exec `
    -ContentType "application/json; charset=utf-8" -Body '{"command":"uname -a"}'
# → {output:"Linux ...", exitCode:0, timedOut:false}

# 4. 想看终端当前内容
Invoke-RestMethod "http://127.0.0.1:8222/api/v1/tabs/board-a/text?lines=50"
```

bash/curl 同理。

## 二、"看谁 / 连谁"自动寻找连接（用户提到目标名时必走此流程）

当用户说"看下 XX"、"连 XX"、"XX 上执行..."等，**不要问用户地址**，按以下顺序自行定位：

1. **查已打开的标签页**：`GET /api/v1/tabs`——每条含 `{index, name, ref, title, type, connected, sessionName, host, port, username}`。**操作标签一律用 `ref`（机器名[:序号]）寻址**——全局 `index` 会随开关标签漂移，`ref` 永远指向同一台机器；纯数字 `ref` 仍按旧序号解析（兼容）。
2. **查已保存会话**：`GET /api/v1/saved-sessions`——返回 `[{name, displayName, host, port, username, locked}]`（不含任何凭据）。
3. **匹配到已保存会话但没开标签** → `POST /api/v1/tabs {"session":"<name>"}` 打开标签页（返回 `{index, name, ref}`），等几秒确认登录到 shell 提示符（`GET .../text?lines=5` 看 `user@host:~$`），然后就能 exec。
4. **匹配不到**：不要猜 IP/凭据，把 `saved-sessions` 的候选列表给用户确认；`locked=true` 说明主密码未解锁，提示用户先在 GUI 解锁。
5. 连接后先确认身份再操作：`GET /api/v1/tabs/<ref>/text?lines=5` 看提示符，或 `exec "whoami; hostname"`。

歧义处理：同名/相似名多个匹配时，列出候选让用户选；用户说"服务器"这类通用词时优先匹配 `connected=true` 的已开标签。

## 三、密码安全模型（必读）

**任何接口都不接受明文密码**。模型/工具只能使用密文或会话名：

| 方式 | 说明 | 适用 |
|------|------|------|
| **会话名引用** | `{"sessionName":"root"}` / `{"session":"名字"}`，hssh 用库内凭据，凭据不出进程 | **首选**，模型连密文都不用见 |
| **密文** | `passwordCipher` = `base64(RSA-OAEP-SHA256(明文))`，公钥见 `GET /api/v1/keys` | 无库存凭据的新主机 |
| 明文 `password` | **一律 400 拒绝** | — |

- 公钥获取：`GET /api/v1/keys` → `{publicKeyPem, fingerprint}`；私钥只存在 hssh 本机（GCM 加密落盘）
- 本地产密文可用 `hssh cli cipher <text>`（输出 JSON 含 cipher 字段）
- 主密码模式下 hssh 未解锁时，密文接口返回 503

## 四、sudo 协议（用户确认门禁）

> ⚠️ **复合命令陷阱（2026-09-11 两次实锤）**：`sudo cmd1 && cmd2` 只有 cmd1 是 root（`&&` 后的是普通用户身份）；`$PWD`、`~` 在 **sudo 运行之前**就被交互 shell 展开（展开的是当前用户的环境，不是 root 的）。**复合命令必须整体包 `bash -c` 并用绝对路径**：`sudo bash -c 'systemctl stop x && systemctl disable x'`。命令里需要 root 视角的路径时写绝对路径，不要依赖 `$PWD`/`~`。

```powershell
POST /api/v1/tabs/<ref>/sudo
{
  "command": "apt update",
  "passwordCipher": "...",        # 可选：密文密码
  "useStoredCredential": true     # 可选：用库内凭据（默认即为 true；与上一项二选一）
}
```

流程：
1. **hssh 弹窗向用户展示完整命令和目标会话，用户必须点 Allow 才执行**；同一标签页生命周期内只弹一次；30 秒未操作视为拒绝（403，响应带 `reason:"timeout"`，与用户点 No 的 `reason:"user_rejected"` 可区分）；窗口最小化时弹窗前会自动恢复窗口并闪任务栏，每次弹窗都写审计日志（`sudo_confirm`）。弹窗里还有 **"Always Allow"**——该机器**永久授权**（持久化在配置 `agent/sudoAlwaysAllow`，撤销=删该配置项；标识：已保存会话 `session:<id>`，临时连接 `host:<user@host>`），之后不再弹窗
2. 发送 `sudo <命令>`；检测到密码提示时自动用密文/库内凭据写入（no-echo，不落缓冲）
3. 等待命令结束（默认 60s，`timeout` 可调），返回 `{output, timedOut, executed, exitCode}`
4. **执行确认纪律**：内部已用完成标记确认真执行——**必须核对 `executed==true` 且 `exitCode==0` 才算生效**；`timedOut=true`/`executed=false` 说明弹窗延迟、终端繁忙或 sudo 卡在密码重试，命令**未确认执行**，应重试

特殊返回：
- `403 {"reason":"user_rejected"}` — 用户点了拒绝
- `403 {"reason":"timeout"}` — 30 秒无人应答（弹窗可能被遮挡）——不是用户拒绝，重试即可
- `404`（reason `unknown_tab`）— 标签不存在或序号漂移；用 `GET /tabs` 的 `ref` 重新寻址
- `428 {"passwordRequired":true}` — 出现密码提示但无可用凭据，改用密文重试

**规则（强制遵守）**：AI 执行任何 sudo 命令**必须走 `/sudo` 接口（或 MCP `ssh_sudo`）**，不得用 `/send` 直接发送 `sudo xxx` 绕过用户确认。

---

## 五、REST API 完整参考（默认 `http://127.0.0.1:8222`）

### 5.1 密钥与状态

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/keys` | `{publicKeyPem, fingerprint, cipher:"RSA-OAEP-SHA256+base64"}` |
| GET | `/api/v1/status` | `{status, name, version, pid}` |
| GET | `/api/v1/health` | `{ok, pid, version, pendingOps, tabs}`——agent 活性自检 |

### 5.2 标签页（唯一的工作面）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/tabs` | `[{index, name, ref, title, type("ssh"/"local"), connected, target, sessionName?, host?, port?, username?, peer?, stale?, savedTarget?, peerMismatch?}]`；**`ref` = 机器名[:序号]，寻址首选**；`target` = `user@host:port`（有 peer 时取**真实 socket 对端**，否则取配置 host）；`stale` = 同名已存会话已改指别的端点（见下） |
| GET | `/api/v1/saved-sessions` | 已保存会话列表（无凭据）`[{name, displayName, host, port, username, locked}]`，"连谁"先查它 |
| POST | `/api/v1/tabs` | 打开标签页：`{shellType?}` 本地终端；`{session:"名字"}` 已保存会话；`{host, port?, username?, authMethod?, passwordCipher?, privateKeyPath?, keyPassphraseCipher?}` **临时 SSH 连接**（拒明文密码，空 cipher 400）。返回 `{index, name, ref}` |
| GET | `/api/v1/tabs/<ref>/text?lines=N&from=M` | 读终端缓冲区；`from` 游标，`lines` 末尾行数上限；响应含 `{index, from, lines, text}` |
| POST | `/api/v1/tabs/<ref>/exec` | **结构化命令执行**（见 5.3） |
| POST | `/api/v1/tabs/<ref>/reconnect` | 重连断线的 SSH 标签（等价"按回车重新连接"手势；连接中的标签 400 拒绝——不会误杀前台程序）。之后轮询 `/tabs` 等 `connected=true` |
| POST | `/api/v1/tabs/<ref>/send` | `{command}`，自动追加回车；`{data}` 原始输入（不追加回车，用于 Ctrl+C=`{"data":"\u0003"}` 等控制字符） |
| POST | `/api/v1/tabs/<ref>/secure-input` | `{cipher}`，密文解密后写入终端（密码输入用） |
| POST | `/api/v1/tabs/<ref>/sudo` | sudo 协议（见第四节，用户确认门禁；响应含 `executed`/`exitCode`） |
| POST | `/api/v1/tabs/<ref>/upload` | `{localPath, remotePath, verify?, method?}`——用该标签的连接参数**另开一条独立连接**上传（不占终端通道），仅 SSH 标签 |
| POST | `/api/v1/tabs/<ref>/download` | `{remotePath, localPath, verify?, method?}`——同上，下载；同一标签同时只允许一个传输（冲突 409） |
| DELETE | `/api/v1/tabs/<ref>` | 关闭标签页（断开并移除） |

`<ref>` 寻址：纯数字 = 旧式全局序号（会漂移，仅兼容）；**`<名称>` 或 `<名称>:<序号>`** = 按机器寻址（名称匹配 sessionName/host/title，序号为同名标签按打开顺序的 1 基编号；裸名称多匹配时取第 1 个）。名称含空格等需 URL 编码。

**`/api/v2/*` 前缀**：路由与 v1 相同，但原本返回顶层 JSON 数组的接口（`/tabs`、`/saved-sessions`）改为对象包装（`{"tabs":[...]}` / `{"savedSessions":[...]}`），避免 PowerShell 5.1 把单元素数组解包成标量的坑。v1 保持原样兼容。

### 5.2.1 传输方式（`method` 参数）与完整性校验

| method | 说明 | 适用 |
|--------|------|------|
| `sftp`（默认） | SFTP 子系统，支持断点续传（失败原样重试从断点继续） | 绝大多数 OpenSSH 服务器 |
| `scp` | 经典 scp 协议（libssh），需远端 `scp` 二进制；单文件、无续传 | dropbear 等常带 scp 但无 sftp-server 的嵌入式目标 |
| `shell` | `base64`/`base64 -d` 走 exec 通道，只要有个 POSIX shell 就能传（~33% 带宽开销）；无续传 | 极简 busybox / 无 sftp-server 无 scp 的目标 |
| `auto` | 依次尝试 sftp → scp → shell，**只在零字节失败时**回退（子系统不可用类错误），字节已动的真错误（权限、磁盘满）直接报告 | 不确定目标支持什么时 |

**完整性校验（两个方向默认都开）**：传完后对比远端 md5（远端 `md5sum`，无 shell 时 SFTP 回读）与本地 md5，不一致则**自动换连接整文件重试一次**，仍不一致才报错。下载侧额外检测"传输期间远端文件被改动"（size/mtime 前后比对）——这就是 2026-08 偶发 null 坏块的根因（远端文件被 ftruncate+重写时，空洞区间读出全零，libssh 如实搬运）；修复后这种损坏会被校验抓住并重试，不再被"尺寸相同 = Already up to date"永远固化。`verify:false` 仅在你自己做哈希校验时关闭。成功响应的 `message` 带传输方式和 md5（如 `downloaded via scp, md5 verified: ...`）。

**远端文件在传输期间被并发改写**（周期性再生的状态文件等）时校验会一直失败——这不是 bug，是在如实报告"拿不到稳定快照"；等生成间歇期再拉，或用 `verify:false` 自担风险。

### 5.3 exec 语义（`/tabs/<ref>/exec`）

把命令**打进可视终端**（用户在窗口里实时看到），用唯一开始/结束标记（`__HSSH_EXEC_B_<token>__` / `__HSSH_EXEC_E_<token>__<退出码>`）圈定输出，150ms 轮询缓冲区直到结束标记出现：

- 请求：`{command, timeout?, reset?}`（timeout 毫秒，默认 120000，**0=不限**；`reset:true` 先送 Ctrl+C 等 400ms 再打命令——清理卡住的续行提示符 `>` 或半行输入，**会打断前台程序**，慎用）
- 响应：`{output, exitCode, timedOut, target, warning?, hint?, truncated?}`
  - **`target`**：`user@host:port`，取真实 socket 对端（peer）——**破坏性操作前必须核对这是你以为的那台机器**（2026-09-11 事故：会话改名后按名路由把命令打到了旧机器）
  - **`warning`**：上一条 exec 超时后命令仍在跑，其迟到输出可能混进本次捕获——**写操作必须独立回读验证**（回读文件/状态），别信 output 里的成功字样
  - **`hint`**：仅 timedOut 时附带（提示检查续行提示符/用 reset:true）
  - `truncated:true`：开始标记已滚出 10000 行缓冲区，输出必不完整
- 命令回显会被自动剥离（输入前导一个空行，吸收半行残留，防首字符被吃）；行尾空格被裁剪，行首缩进保留
- **一个标签同时只允许一个 exec**（冲突 409）；超时返回 `timedOut:true` + 部分输出，**命令继续在终端里跑**（用户看得见、可手动 Ctrl+C）；**exec 中途断线立即报错并释放锁**（不会 409 卡死），重连用 `/tabs/<ref>/reconnect`
- **标签必须停在 shell 提示符**：输入进前台程序。vim/top/交互程序开着时 exec 会把按键喂给它们——exec 前先 `text?lines=5` 确认提示符；疑似卡在续行提示符（`>`）时用 `reset:true`
- exec 期间用户在同一个窗口打字会和 AI 的输入混在一起——别在用户正在操作的窗口里跑 exec
- 终端缓冲区上限 10000 行：超过 10000 行的输出拿不全（`truncated:true`）。**长输出命令重定向到文件再下载**：`exec "cmd > /tmp/x.log 2>&1"` → `POST .../download`；全量原始字节永远可以读会话日志 `%APPDATA%\hssh-project\hssh\logs\session_*.log`（无截断）

**"我以为在操作哪台机"核对清单（跨机操作前必做）**：
1. `GET /tabs` 看目标条目的 `target`（真实 peer）与 `stale` 标记——`stale:true` 表示同名会话已改指 `savedTarget`，按名路由会落到这台旧机器上（MCP 会拒绝，REST 直连你自己负责）
2. exec 响应里的 `target` 再核对一次
3. 重大操作前先 `exec "hostname -I; uptime"` 对号

### 5.4 Token 鉴权

配置 `agent/token` 后（`%APPDATA%\hssh-project\hssh.ini`），除 `/api/v1/status` 外需带请求头 `Authorization: Bearer <token>`。

---

## 六、MCP 服务器（Claude Code / OpenCode 接入）

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

**MCP 进程是 GUI agent 的桥**：自己不建任何 SSH 连接，所有操作经 REST 落到 GUI 可视标签页。连不上 `127.0.0.1:8222` 时自动 `hssh --agent` 拉起 GUI（最多等 45s，首轮调用会慢）。

工具（`session` 参数 = 标签 ref 或已保存会话名，没开标签会自动开）：

| 工具 | 说明 |
|------|------|
| `list_sessions` | 已保存会话 + 已打开标签（含 drift-safe ref、`target` 真实对端、`stale` 标记） |
| `ssh_connect` | 打开可视标签（sessionName 或 host+passwordCipher），返回 ref+target；GUI 没在跑会自动拉起 |
| `ssh_exec` | 在可视标签里执行命令，返回 `{output, exitCode, timedOut, target, warning?, hint?}`（语义同 5.3；timeout 默认 120s、0=不限；可选 `reset` 清场）。断线标签会**自动触发重连**并等连接恢复；按名命中 stale 标签会**拒绝执行**并说明（用显式 ref 或先 ssh_disconnect） |
| `ssh_send` | 发原始输入（按键、Ctrl+C 等控制字符） |
| `ssh_read` | 读终端缓冲区（lines/from 游标） |
| `ssh_upload` / `ssh_download` | 独立连接传输，不占终端；两个方向默认 MD5 校验（不符自动换连接整文件重试一次）。可选 `method`: `sftp`（默认）/ `scp` / `shell`（base64，无 sftp-server 也能传）/ `auto`（回退链）；可选 `verify` |
| `ssh_sudo` | sudo（弹窗确认在 **GUI 窗口**；密码来自库存凭据或用户亲手输入，永不进 AI 上下文；"Always allow" 永久授权）。务必核对 `executed`+`exitCode`。**复合命令必须 `sudo bash -c '...'` + 绝对路径**——`sudo a && b` 只有 a 是 root，`$PWD`/`~` 在 sudo 前就被外层 shell 展开 |
| `ssh_disconnect` | 关闭标签页 |
| `get_public_key` | 取 RSA 公钥（本地操作，不需要 agent） |

出错时 `error.data` 保留 `output`/`exitCode`/`timedOut` 等部分结果字段。

---

## 七、CLI 子命令（一次性执行，脚本友好）

```bash
hssh cli exec --host HOST --username USER --password-cipher CIPHER --command "uname -a"
hssh cli exec --host HOST --username USER --command "uname -a"   # TTY 下无回显输入密码
# {"ok":true,"exitCode":0,"output":"Linux ...\n"}

hssh cli cipher <text>
# {"cipher":"...","fingerprint":"..."}
```

可选参数：`--port`（默认 22）、`--auth-method password|publickey|agent`、`--private-key`、`--password-cipher`。

---

## 八、典型任务模式

### 在远程机器上跑命令取结果（最常用）

`POST /api/v1/tabs/<ref>/exec {"command":"..."}` → 核对 `exitCode`/`timedOut`。标签没开就先 `POST /tabs {"session":"名字"}`。

### 长输出/大日志

`exec "somebuild > /tmp/build.log 2>&1"`（timeout 调大）→ `download` 拉回本地分析。不要指望 exec 的 output 装下整个构建日志（10000 行上限 + `truncated`）。

### 持续性数据流（tail -f、实时监控）

exec 不适合。用游标轮询排水：

```powershell
$cursor = 0
while ($true) {
    $r = Invoke-RestMethod "http://127.0.0.1:8222/api/v1/tabs/board-a/text?from=$cursor&lines=0"
    if ($r.lines -gt 0) { $cursor += $r.lines; $r.text }
    Start-Sleep -Milliseconds 500
}
```

### 文件上传/下载

`POST /tabs/<ref>/upload {localPath, remotePath}`（默认 MD5 校验）。HTTP 客户端超时必须大于预期耗时（PS `Invoke-RestMethod` 默认 100 秒，大文件 `-TimeoutSec` 调大）；SFTP 方式支持断点续传，失败原样重试。目标没有 sftp-server（部分嵌入式 dropbear/busybox）时加 `"method":"shell"` 或 `"auto"`。

## 九、错误处理

| 状态码 | 含义 | 处理 |
|--------|------|------|
| 400 | 缺参数 / 明文密码 / body 非合法 UTF-8 JSON（提示 `charset=utf-8`） | 检查 body；PS5.1 显式加 charset 头 |
| 401 | Token 鉴权失败 | 带 `Authorization: Bearer` 头 |
| 403 | sudo 未获授权（`reason` 区分 `user_rejected`/`timeout`） | 见第四节 |
| 404 | tab ref 不存在（或序号漂移） | `GET /tabs` 拿当前 ref 重试 |
| 409 | 该标签已有 exec/传输在跑 | 等它结束或换个标签；别并发打同一个窗口 |
| 501 | tabs 接口打在无 GUI 的 `--agent-http` 进程上 | 起 GUI（`hssh --agent`） |
| 503 | 凭据未解锁 / GUI agent 不可达（MCP 侧） | 解锁主密码；MCP 会自动拉起 GUI，超时重试即可 |

## 十、注意事项

- 所有接口仅监听 `127.0.0.1`，不会暴露到网络。
- `send` 的 `{command}` 自动追加 `\n`；控制字符/原始字节用 `{data}`。
- `text` 返回的是 ANSI 序列处理后的纯文本（用户看到的内容）。
- exec/sudo/传输都是同步长请求：HTTP 客户端超时要大于预期耗时 + exec 自身 timeout。
- upload/download 默认 MD5 内容校验（下载另带 size/mtime 改动检测），失败自动换连接整文件重试一次，仍失败返回 500；`"verify":false` 仅在自己做哈希校验时用。成功 `message` 里带 md5 与传输方式。
- 只有 SFTP 方式支持断点续传；scp/shell 方式失败重试从头开始。
- 需要 polkit 授权的命令（`nmcli` 断连、部分 `systemctl` 用户态操作等）在普通 exec 下会报 "not authorized"——这类命令走 `/sudo`（或 MCP `ssh_sudo`）。
- 多实例归属：Agent 只监听 8222，先到先得；`GET /health` 返回的 `pid` 就是当前 owner。GUI 起不来 agent 时（端口被占）会弹窗提示，运行中 accept 错误会写审计并显示在状态栏。
- 板上长命令/嵌套引号在 exec 里不用担心（exec 是整段输入进终端的），但 `send` 拼复杂命令时引号仍会被各层吃掉——复杂脚本一律 base64 落 /tmp 再跑。
