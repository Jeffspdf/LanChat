# LanChat

基于 **C++17 / Qt Widgets / TCP / SQLite / Qt Concurrent** 的局域网即时通讯桌面应用。

本仓库由个人手敲学习项目 `Qt_own` 整理而来，保留了源码中的中文注释，适合结合代码理解 Qt 网络、事件循环、持久化和异步任务。客户端与服务端使用同一个可执行文件，通过参数区分。

## 已实现功能

- 注册、登录，拒绝重复在线登录；账号使用盐与 PBKDF2-HMAC-SHA256 派生值存储。
- 显示同一服务器的注册用户及在线状态，支持一对一中文、多行文本聊天。
- TCP 长连接，使用 **4 字节大端包体长度 + UTF-8 JSON** 定义消息边界，处理拆包和粘包。
- 服务端 SQLite 保存消息；客户端保存后回复 ACK，未确认消息重新登录后补发。
- 使用消息 UUID 和本地主键去重，避免重复保存和展示。
- 按会话查看最近 200 条本地历史，提供未读计数、深浅主题及系统托盘提醒。
- 将密码派生计算放入线程池，网络和数据库操作保留在各自所属事件线程。

联系人列表是服务器用户目录，尚未实现好友申请、群聊、文件传输和自动重连。

## 开发环境

本地验证环境：Windows、Qt 6.8.3 MinGW 64-bit、MinGW 13.1、C++17。需要 Qt 的 Core、Widgets、Network、Sql、Concurrent 模块及 QSQLITE 插件。

Qt 库和编译器必须匹配，不能用 MinGW 编译器链接 MSVC 版 Qt。

## 构建

### Qt Creator

打开 `LanChat.pro`，选择匹配的 Desktop kit，运行 qmake 后构建。默认不带参数运行客户端；在运行参数中添加 `--server` 运行服务端。

### Windows 本地脚本

脚本默认使用：

- Qt：`C:\Qt\6.8.3\mingw_64`
- MinGW：`C:\Qt\Tools\mingw1310_64`

先运行：

```bat
build.cmd
```

脚本将 Release 程序生成到 `build\release\LanChat.exe`。安装路径不同时，可在同一终端设置 `LANCHAT_QT_DIR`、`LANCHAT_MINGW_DIR` 后调用脚本；也可以使用 Qt Creator。

```bat
set "LANCHAT_QT_DIR=D:\Qt\6.8.3\mingw_64"
set "LANCHAT_MINGW_DIR=D:\Qt\Tools\mingw1310_64"
build.cmd
```

所有脚本通过 `qt-env.cmd` 配置当前进程的运行环境，不修改系统 PATH。

## 同一电脑演示

1. 双击 `run-server.cmd`，保持服务端终端打开，默认监听 TCP 45454。
2. 双击 `run-client.cmd` 两次，打开两个客户端窗口。
3. 服务器填 `127.0.0.1`，端口 `45454`，分别注册两个不同账号，再分别登录。
4. 选择联系人发送文本。让收件方退出登录后再发送，重新登录验证补发。

账号需为 3-24 位 ASCII 字母、数字或下划线，密码长度为 8-128 个 QString 代码单元。请使用测试密码。

脚本不自动编译，修改代码后需要重新构建。可传递附加程序参数，例如：

```bat
run-server.cmd --port 45455 --data .\server-data
run-client.cmd --self-test
run-client.cmd --ui-self-test
```

跨机聊天需要填服务端局域网 IPv4 地址并允许防火墙访问相应端口；`127.0.0.1` 只表示当前电脑。运行脚本依赖已安装的 Qt 开发运行环境，并非独立部署包。

## 数据保存位置

默认使用 `QStandardPaths::AppDataLocation`。当前应用标识为 `LanChat/LanChat`，Windows 通常为：

```text
%APPDATA%\LanChat\LanChat\
  server\server.db       注册用户和服务端消息
  <SHA-256>.db           按服务器地址、端口、账号隔离的客户端历史
```

`--data` 可以覆盖服务端目录。客户端上次使用的服务器地址和账号由 QSettings 保存，Windows 默认后端通常是注册表。相同应用标识的其他项目可能共享数据；本仓库不包含真实账号库或聊天记录。

## 代码结构

```text
LanChat.pro         qmake 项目配置
src/main.cpp        启动模式、参数和事件循环
src/protocol.*      长度帧编码、增量收包、密码派生、协议自检
src/server.*        连接会话、认证、持久化、ACK 与消息投递
src/client.*        登录页、聊天页、本地历史、主题和托盘
qt-env.cmd          本地 Qt/MinGW 环境配置
build.cmd           Release 构建
run-server.cmd      服务端启动
run-client.cmd      客户端启动
```

## 验证方式

- `--self-test`：长度头和 JSON 编码往返自检。
- `--ui-self-test`：使用内存数据库验证去重、历史顺序、缩放、失败保留草稿与会话清理；会建立测试窗口。
- 手动演示：两个账号聊天、离线补发、重启后历史查询。

自检不能替代完整跨机、系统通知或压力测试。此仓库不包含原项目目录中的 Python 集成测试脚本。

## 当前限制

本项目用于局域网学习与面试演示。传输未启用 TLS，登录密码通过普通 TCP 发送，聊天历史为明文 SQLite。服务器确认表示服务端已保存，收件 ACK 表示客户端已保存，都不表示已读。

每个收件连接一次等待一条确认；当前没有同一连接内的 ACK 超时重试，也没有客户端提交 requestId 幂等机制。发送确认前断线的结果可能未知。不宣称生产级安全、高并发性能或无条件不丢消息。