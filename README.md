# HSSH

> 一款完全开源、跨平台、高性能的 GUI SSH 客户端，内置 Agent 能力，可被 Claude Code 及其他外部工具调用。

## 项目状态

早期开发阶段，已完成以下功能：

- **会话管理**：分组树形结构、快速搜索、导入/导出（JSON）、最近会话历史、SQLite 存储
- **终端**：libvterm 仿真（滚动回滚、选择复制、右键粘贴、Ctrl+F 搜索高亮）、多标签页、本地 Shell（PowerShell/CMD/WSL/bash/zsh/fish）、自动会话日志
- **SSH 连接**：密码/公钥/键盘交互/SSH Agent 认证、KeepAlive 心跳、断线自动重连、登录后命令
- **SFTP**：上传/下载/删除/重命名/拖拽/递归传输/传输队列
- **文件对比**：本地/远程双栏对比与递归目录对比
- **端口转发**：本地 / 远程 / 动态（SOCKS5）
- **高级**：命令发送器（多终端广播）、专注模式、屏幕锁定
- **Agent**：MCP (stdio) 与本地 REST API（`127.0.0.1:8222`）、`hssh cli exec` 命令行，详见 [docs/agent.md](./docs/agent.md)

## 文档

- [需求文档](./docs/requirements.md)
- [技术选型分析](./docs/tech-stack.md)
- [Agent 使用说明（MCP / REST API）](./docs/agent.md)

## 目标

- 完全开源（Apache-2.0）
- 跨平台（Windows / Linux / macOS）
- 主流 SSH 工具功能全覆盖
- 原生性能，低资源占用
- 内置 Agent，支持 MCP / REST API / CLI 调用

## 技术栈

- **语言**：C++20
- **GUI 框架**：Qt6（QML + Widgets 混合）
- **构建系统**：CMake
- **SSH 库**：libssh（待集成）
- **终端仿真**：libvterm（待集成）

## 构建步骤

### 1. 安装依赖

#### Windows

- 安装 [Qt6 开源版](https://www.qt.io/download-qt-installer)（建议 Qt 6.5 LTS 或更高）
- 安装 [CMake](https://cmake.org/download/)
- 安装 Visual Studio 2022 或 MinGW（Qt 自带）

#### Linux (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install build-essential cmake qt6-base-dev qt6-base-dev-tools libqt6widgets6
```

#### macOS

```bash
brew install cmake qt@6
```

### 2. 克隆并构建

```bash
git clone https://github.com/yourname/hssh.git
cd hssh
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

### 3. 运行

```bash
./hssh
```

## 目录结构

```
hssh/
├── CMakeLists.txt          # CMake 构建配置
├── README.md               # 本文件
├── docs/                   # 文档
│   ├── requirements.md     # 需求文档
│   └── tech-stack.md       # 技术选型分析
├── src/                    # 源码
│   ├── main.cpp            # 程序入口
│   ├── app/                # GUI 主程序
│   ├── core/               # SSH/SFTP 核心逻辑
│   ├── terminal/           # 终端仿真
│   ├── agent/              # Agent 服务
│   └── utils/              # 工具类
├── include/                # 公共头文件
├── resources/              # 图标、主题、i18n
└── tests/                  # 单元测试/集成测试
```

## 贡献

欢迎提交 Issue 和 PR。

## License

Apache-2.0
