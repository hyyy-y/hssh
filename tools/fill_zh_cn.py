# -*- coding: utf-8 -*-
"""One-shot: fill zh_CN translations for B3-B6 GUI strings.

Discipline (AGENTS.md 2026-09-20): GUI-visible strings are translated;
inter-machine contract messages (transfer verification / connection errors /
ZMODEM terminal banners, referenced by SKILL.md in English) stay English.
"""
import xml.etree.ElementTree as ET

KEEP = {
    'hssh::ChannelCopySession', 'hssh::SftpSession', 'hssh::SshConnect',
    'hssh::SerialShellProcess', 'hssh::ZModemEngine', 'hssh::LocalShellProcess',
    'TransportFactory',
}

M = {}

def add(ctx, src, dst):
    M[(ctx, src)] = dst

add('QObject', 'Key Store', '密钥库')
add('QObject', 'The key store is empty. Generate or import a key via Tools > Key Manager first.',
    '密钥库为空。请先通过 工具 > 密钥管理器 生成或导入密钥。')
add('QObject', 'Select Key', '选择密钥')
add('QObject', 'Name', '名称')
add('QObject', 'Type', '类型')
add('QObject', 'SHA256 Fingerprint', 'SHA256 指纹')
add('QObject', 'encrypted', '已加密')

add('hssh::AppearanceDialog', 'Appearance', '外观')
add('hssh::AppearanceDialog', 'Terminal &font:', '终端字体(&F)：')
add('hssh::AppearanceDialog', 'Font &size:', '字号(&S)：')
add('hssh::AppearanceDialog', 'Dark', '深色')
add('hssh::AppearanceDialog', 'Light', '浅色')
add('hssh::AppearanceDialog', 'High contrast', '高对比')
add('hssh::AppearanceDialog', '&Theme:', '主题(&T)：')
add('hssh::AppearanceDialog', 'Window opacity — below 100% the whole window (including text) becomes translucent',
    '窗口不透明度——低于 100% 时整个窗口（含文字）变半透明')
add('hssh::AppearanceDialog', 'Window &opacity:', '窗口不透明度(&O)：')

add('hssh::CommandPalette', 'Type a command, session or tab...', '输入命令、会话或标签页…')
add('hssh::FileCompareWidget', '🌁', '🌁')

add('hssh::FloatingTabWindow', 'Dock to Main Window', '回到主窗口')
add('hssh::FloatingTabWindow', 'Pinned Tab', '已固定的标签页')
add('hssh::FloatingTabWindow', 'This tab is pinned — unpin it before closing.',
    '此标签页已固定——关闭前请先取消固定。')

add('hssh::ImportSshConfigDialog', 'Import OpenSSH config', '导入 OpenSSH 配置')
add('hssh::ImportSshConfigDialog', 'Browse...', '浏览…')
add('hssh::ImportSshConfigDialog',
    'Hosts parsed from the config. Wildcard patterns are skipped; ProxyJump is stored but not used until jump-host support lands.',
    '从配置解析出的主机。通配符条目已跳过；ProxyJump 已存储，待跳板功能生效后使用。')
add('hssh::ImportSshConfigDialog', 'Import', '导入')
add('hssh::ImportSshConfigDialog', 'Name', '名称')
add('hssh::ImportSshConfigDialog', 'Host', '主机')
add('hssh::ImportSshConfigDialog', 'Port', '端口')
add('hssh::ImportSshConfigDialog', 'User', '用户')
add('hssh::ImportSshConfigDialog', 'OpenSSH config', 'OpenSSH 配置')

add('hssh::KbdintPromptDialog', 'Two-Factor Authentication', '双因素认证')
add('hssh::KbdintPromptDialog', 'Remember answers for this session', '本次会话内记住答案')

add('hssh::KeyManagerDialog', 'encrypted', '已加密')
add('hssh::KeyManagerDialog', 'Key Manager', '密钥管理器')
add('hssh::KeyManagerDialog', 'Name', '名称')
add('hssh::KeyManagerDialog', 'Type', '类型')
add('hssh::KeyManagerDialog', 'SHA256 Fingerprint', 'SHA256 指纹')
add('hssh::KeyManagerDialog', '&Generate...', '生成(&G)…')
add('hssh::KeyManagerDialog', '&Import...', '导入(&I)…')
add('hssh::KeyManagerDialog', '&Export Public Key', '导出公钥(&E)')
add('hssh::KeyManagerDialog', '&Delete', '删除(&D)')
add('hssh::KeyManagerDialog', 'Known Hosts', '已知主机')
add('hssh::KeyManagerDialog', 'Host', '主机')
add('hssh::KeyManagerDialog', 'Key Type', '密钥类型')
add('hssh::KeyManagerDialog', 'Remove &Host', '移除主机(&H)')
add('hssh::KeyManagerDialog', 'unlock to show', '解锁后显示')
add('hssh::KeyManagerDialog', 'Generate Key', '生成密钥')
add('hssh::KeyManagerDialog', 'Name:', '名称：')
add('hssh::KeyManagerDialog', 'Ed25519 (recommended)', 'Ed25519（推荐）')
add('hssh::KeyManagerDialog', 'Type:', '类型：')
add('hssh::KeyManagerDialog', 'Passphrase (optional):', '口令（可选）：')
add('hssh::KeyManagerDialog', 'Confirm passphrase:', '确认口令：')
add('hssh::KeyManagerDialog',
    'Keys are stored in the hssh key store. Use "Export Public Key" to copy the authorized_keys line for the remote host.\n\nOnly Ed25519 can be generated here (crypto backend limitation); RSA/ECDSA keys can be imported via "Import...".',
    '密钥存储在 hssh 密钥库中。使用“导出公钥”可复制 authorized_keys 行到远程主机。\n\n此处仅能生成 Ed25519（加密后端限制）；RSA/ECDSA 密钥请通过“导入…”导入。')
add('hssh::KeyManagerDialog', 'Passphrases do not match.', '两次输入的口令不一致。')
add('hssh::KeyManagerDialog', 'Key generation failed.', '密钥生成失败。')
add('hssh::KeyManagerDialog', 'Key generated:\n%1', '密钥已生成：\n%1')
add('hssh::KeyManagerDialog', 'Import Private Key', '导入私钥')
add('hssh::KeyManagerDialog', 'Private key files (id_* *.pem *.key);;All Files (*)',
    '私钥文件 (id_* *.pem *.key);;所有文件 (*)')
add('hssh::KeyManagerDialog', 'Private key files (id_* *.pem *.key);;All files (*)',
    '私钥文件 (id_* *.pem *.key);;所有文件 (*)')
add('hssh::KeyManagerDialog', 'Import Key', '导入密钥')
add('hssh::KeyManagerDialog', 'Key imported:\n%1', '密钥已导入：\n%1')
add('hssh::KeyManagerDialog', 'Key import failed.', '密钥导入失败。')
add('hssh::KeyManagerDialog', 'The key is passphrase-protected. Passphrase:', '该密钥受口令保护。口令：')
add('hssh::KeyManagerDialog', 'Export Public Key', '导出公钥')
add('hssh::KeyManagerDialog', 'Passphrase for %1:', '%1 的口令：')
add('hssh::KeyManagerDialog', 'Export failed.', '导出失败。')
add('hssh::KeyManagerDialog', '(unlock to show)', '（解锁后显示）')
add('hssh::KeyManagerDialog', 'Copied to the clipboard:\n\n%1\n\nFingerprint: %2',
    '已复制到剪贴板：\n\n%1\n\n指纹：%2')
add('hssh::KeyManagerDialog', 'Delete Key', '删除密钥')
add('hssh::KeyManagerDialog', 'Delete %1?\n\nSessions using it will need to be updated.',
    '删除 %1？\n\n引用它的会话需要更新。')
add('hssh::KeyManagerDialog', 'Delete failed.', '删除失败。')
add('hssh::KeyManagerDialog', 'Remove Known Host', '移除已知主机')
add('hssh::KeyManagerDialog',
    'Remove the stored host key for %1?\nThe next connection will ask for confirmation again.',
    '移除 %1 的已存主机密钥？\n下次连接将再次弹出确认。')
add('hssh::KeyManagerDialog', 'Remove failed.', '移除失败。')

add('hssh::MainWindow', 'Server-to-Server Transfer...', '服务器间传输…')
add('hssh::MainWindow', '&Key Manager...', '密钥管理器(&K)…')
add('hssh::MainWindow', 'Session &Log Viewer...', '会话日志查看器(&L)…')
add('hssh::MainWindow', 'Free Type', '自由输入')
add('hssh::MainWindow', 'Free Type Mode', '自由输入模式')
add('hssh::MainWindow',
    'Keyboard input will be sent to %1 other SSH tab(s) at the same time.\nBeware of vim, sudo and password prompts in mirrored tabs. Continue?',
    '键盘输入将同时发送到其他 %1 个 SSH 标签页。\n镜像标签中的 vim、sudo 与密码提示存在风险。继续吗？')
add('hssh::MainWindow', 'Free Type ON — input is mirrored to every other SSH tab',
    '自由输入已开启——输入镜像到其他所有 SSH 标签页')
add('hssh::MainWindow', 'Free Type OFF', '自由输入已关闭')
add('hssh::MainWindow', 'Outline', '大纲')
add('hssh::MainWindow', 'Server Monitor', '服务器监控')
add('hssh::MainWindow', 'Ctrl+Shift+P', 'Ctrl+Shift+P')
add('hssh::MainWindow', 'Edit Tags', '编辑标签')
add('hssh::MainWindow', "Tags for '%1' (comma separated):", '“%1”的标签（逗号分隔）：')
add('hssh::MainWindow', "Tags updated for '%1'", '已更新“%1”的标签')
add('hssh::MainWindow', 'Detach to Window', '分离为独立窗口')
add('hssh::MainWindow', 'ZMODEM Send File...', 'ZMODEM 发送文件…')
add('hssh::MainWindow', 'Send via ZMODEM', '通过 ZMODEM 发送')
add('hssh::MainWindow', 'Cannot start ZMODEM send (a transfer is active, or the file is unreadable/empty)',
    '无法启动 ZMODEM 发送（有传输进行中，或文件不可读/为空）')
add('hssh::MainWindow', 'View Session Log...', '查看会话日志…')
add('hssh::MainWindow', 'Process List...', '进程列表…')
add('hssh::MainWindow', 'Network Tools...', '网络工具…')
add('hssh::MainWindow', 'Docker...', 'Docker…')
add('hssh::MainWindow', 'Scheduled Tasks...', '定时任务…')
add('hssh::MainWindow', "Tab '%1' is pinned — unpin it before detaching.",
    '标签页“%1”已固定——分离前请先取消固定。')
add('hssh::MainWindow', "'%1' detached to its own window", '“%1”已分离为独立窗口')
add('hssh::MainWindow', "'%1' docked back", '“%1”已回到主窗口')
add('hssh::MainWindow',
    'Failed to start agent: %1\n\nAnother hssh instance (or MCP auto-launched GUI) may already own the port — check GET http://127.0.0.1:8222/api/v1/health for its pid.',
    '启动 agent 失败：%1\n\n另一个 hssh 实例（或 MCP 自动拉起的 GUI）可能已占用该端口——用 GET http://127.0.0.1:8222/api/v1/health 查其 pid。')
add('hssh::MainWindow', 'Command', '命令')
add('hssh::MainWindow', 'Session', '会话')
add('hssh::MainWindow', 'Go to Tab', '跳转到标签页')
add('hssh::MainWindow', 'Agent Request', 'Agent 请求')
add('hssh::MainWindow', 'An AI agent requests %1 on "%2":', 'AI agent 请求在“%2”上执行 %1：')
add('hssh::MainWindow',
    'Allow it? "Always" remembers this host in agent/policyRules; "This session" lasts until the app closes. Auto-closes in 30 seconds.',
    '允许吗？“始终允许”会记住该主机（写入 agent/policyRules）；“本次会话”持续到程序关闭。30 秒后自动关闭。')
add('hssh::MainWindow', 'This session', '本次会话')
add('hssh::MainWindow', 'Always', '始终允许')

add('hssh::NewSessionDialog', 'SSH', 'SSH')
add('hssh::NewSessionDialog', 'Serial (COM/tty)', '串口 (COM/tty)')
add('hssh::NewSessionDialog', 'Type:', '类型：')
add('hssh::NewSessionDialog', 'Key Store...', '密钥库…')
add('hssh::NewSessionDialog', 'COM3 / /dev/ttyUSB0', 'COM3 / /dev/ttyUSB0')
add('hssh::NewSessionDialog', 'Serial Port:', '串口：')
add('hssh::NewSessionDialog', 'Baud Rate:', '波特率：')
add('hssh::NewSessionDialog', 'Forward local ssh-agent to this host (needs the ssh-agent service)',
    '转发本地 ssh-agent 到该主机（需要 ssh-agent 服务）')
add('hssh::NewSessionDialog', 'Proxy && Jump Host', '代理与跳板机')
add('hssh::NewSessionDialog', 'None (direct)', '无（直连）')
add('hssh::NewSessionDialog', 'Proxy:', '代理：')
add('hssh::NewSessionDialog', 'Proxy Host:', '代理主机：')
add('hssh::NewSessionDialog', 'Proxy Port:', '代理端口：')
add('hssh::NewSessionDialog', 'Proxy Username:', '代理用户名：')
add('hssh::NewSessionDialog', 'Proxy Password:', '代理密码：')
add('hssh::NewSessionDialog', 'empty = direct connection', '留空 = 直连')
add('hssh::NewSessionDialog', 'Jump Host:', '跳板主机：')
add('hssh::NewSessionDialog', 'Jump Port:', '跳板端口：')
add('hssh::NewSessionDialog', 'empty = same as target username', '留空 = 同目标用户名')
add('hssh::NewSessionDialog', 'Jump Username:', '跳板用户名：')
add('hssh::NewSessionDialog', 'Jump Password:', '跳板密码：')
add('hssh::NewSessionDialog', 'empty = password auth for jump', '留空 = 跳板使用密码认证')
add('hssh::NewSessionDialog', 'Jump Key Path:', '跳板密钥路径：')

add('hssh::SessionLogViewer', 'Session Log Viewer', '会话日志查看器')
add('hssh::SessionLogViewer', 'Show:', '显示：')
add('hssh::SessionLogViewer', 'All time', '全部')
add('hssh::SessionLogViewer', 'Today', '今天')
add('hssh::SessionLogViewer', 'Last 3 days', '最近 3 天')
add('hssh::SessionLogViewer', 'Last 7 days', '最近 7 天')
add('hssh::SessionLogViewer', 'Last 30 days', '最近 30 天')
add('hssh::SessionLogViewer', 'Refresh', '刷新')
add('hssh::SessionLogViewer', 'Export...', '导出…')
add('hssh::SessionLogViewer', 'Find in log (Enter = next match)', '在日志中查找（回车 = 下一个匹配）')
add('hssh::SessionLogViewer', 'Cannot open %1: %2', '无法打开 %1：%2')
add('hssh::SessionLogViewer', '[%1 MB log truncated to the last %2 MB]\n\n',
    '【%1 MB 日志已截断为最后 %2 MB】\n\n')
add('hssh::SessionLogViewer', '> %1 matches', '超过 %1 个匹配')
add('hssh::SessionLogViewer', '%1 match(es)', '%1 个匹配')
add('hssh::SessionLogViewer', '%1 / %2', '%1 / %2')
add('hssh::SessionLogViewer', 'Export', '导出')
add('hssh::SessionLogViewer', 'Select a log file first.', '请先选择一个日志文件。')
add('hssh::SessionLogViewer', 'Export Log', '导出日志')
add('hssh::SessionLogViewer', 'Text Files (*.txt);;All Files (*)', '文本文件 (*.txt);;所有文件 (*)')
add('hssh::SessionLogViewer', 'Cannot write %1: %2', '无法写入 %1：%2')
add('hssh::SessionLogViewer', 'Exported to %1', '已导出到 %1')

add('hssh::SessionManagerWidget', 'Remove from Favorites', '取消收藏')
add('hssh::SessionManagerWidget', 'Add to Favorites', '收藏')
add('hssh::SessionManagerWidget', 'Edit Tags...', '编辑标签…')

add('hssh::SessionTab', 'Host Key CHANGED', '主机密钥已变更')
add('hssh::SessionTab', 'Unknown Host Key', '未知主机密钥')
add('hssh::SessionTab',
    'The host key for %1 has CHANGED!\nThis could indicate a man-in-the-middle attack, or the server\nwas reinstalled. Verify the fingerprint out-of-band before\ncontinuing.\n\nKey type: %2\nSHA256 fingerprint: %3\nMD5 fingerprint: %4\n\nAccept and replace the stored key?',
    '%1 的主机密钥已变更！\n这可能是中间人攻击，也可能是服务器被重装。\n继续前请通过带外渠道核实指纹。\n\n密钥类型：%2\nSHA256 指纹：%3\nMD5 指纹：%4\n\n接受并替换已存储的密钥？')
add('hssh::SessionTab',
    'The authenticity of host %1 cannot be established.\n\nKey type: %2\nSHA256 fingerprint: %3\nMD5 fingerprint: %4\n\nTrust this host and store its key?',
    '无法确认 %1 的真实性。\n\n密钥类型：%2\nSHA256 指纹：%3\nMD5 指纹：%4\n\n信任该主机并存储其密钥？')

add('hssh::SftpWidget', 'SFTP', 'SFTP')
add('hssh::SftpWidget', 'SCP', 'SCP')
add('hssh::SftpWidget',
    'Transfer method for uploads and downloads.\nSFTP supports resume and directory sync; SCP is a single-channel legacy fallback.',
    '上传/下载的传输方式。\nSFTP 支持断点续传与目录同步；SCP 是单通道的传统回退方式。')
add('hssh::SftpWidget', 'Search in this folder…', '在此文件夹中搜索…')
add('hssh::SftpWidget', "Stop Editing '%1'", '停止编辑“%1”')
add('hssh::SftpWidget', "Edit '%1'", '编辑“%1”')
add('hssh::SftpWidget', 'Permissions…', '权限…')
add('hssh::SftpWidget', 'Already editing', '已在编辑')
add('hssh::SftpWidget', "'%1' is already being edited in another tab:\n%2",
    '“%1”已在另一个标签页中编辑：\n%2')
add('hssh::SftpWidget', 'Stopped editing %1 (the local copy stays in the temp folder)',
    '已停止编辑 %1（本地副本保留在临时文件夹）')
add('hssh::SftpWidget', 'Uploading edited %1…', '正在上传编辑后的 %1…')
add('hssh::SftpWidget', 'Local copy of %1 disappeared — edit stopped',
    '%1 的本地副本已消失——编辑已停止')
add('hssh::SftpWidget', 'Editing %1 — saves upload automatically', '正在编辑 %1——保存即自动上传')
add('hssh::SftpWidget', 'Could not fetch %1 for editing: %2', '无法获取 %1 用于编辑：%2')
add('hssh::SftpWidget', 'Uploaded edited %1', '已上传编辑后的 %1')
add('hssh::SftpWidget', 'Auto-upload of %1 failed: %2 — saving again retries',
    '%1 自动上传失败：%2——再次保存可重试')

add('hssh::TerminalOutlineWidget', 'Prompts, build steps and log headers of the current terminal. Click to jump.',
    '当前终端的提示符、构建步骤与日志标题。点击跳转。')
add('hssh::TerminalWidget', 'Open Link', '打开链接')
add('hssh::TerminalWidget', 'Copy Link Address', '复制链接地址')
add('hssh::TerminalWidget', 'Show Timestamps', '显示时间戳')
add('hssh::TerminalWidget', 'The remote host wants to put %n byte(s) into your clipboard. Allow?',
    '远程主机想将 %n 字节写入剪贴板。允许吗？')
add('hssh::TerminalWidget', 'Remote Clipboard Access', '远程剪贴板访问')

# ---- apply ----
path = 'resources/i18n/hssh_zh_CN.ts'
tree = ET.parse(path)
root = tree.getroot()
filled, left, kept = 0, [], 0
for ctx in root.findall('context'):
    name = ctx.find('name').text
    for msg in ctx.findall('message'):
        tr = msg.find('translation')
        if tr is None or tr.get('type') != 'unfinished':
            continue
        src = msg.find('source').text or ''
        if name in KEEP:
            kept += 1
            continue  # intentional English
        key = (name, src)
        if key in M:
            tr.text = M[key]
            del tr.attrib['type']
            filled += 1
        else:
            left.append(f'{name}\t{src[:80]}')

tree.write(path, encoding='utf-8', xml_declaration=True)
print(f'filled={filled} kept_english={kept} still_unfinished={len(left)}')
for line in left:
    print('LEFT:', line)
