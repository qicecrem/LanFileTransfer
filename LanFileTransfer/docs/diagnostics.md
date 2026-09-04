# 统一日志、崩溃捕获与诊断包

## 运行日志

日志写入应用数据目录的 `diagnostics/logs/landrop.log`，每行是一个独立 JSON 对象，字段包括 UTC 时间、级别、Qt 分类、线程和消息。Qt/C++、QML、网络、传输、Qt Multimedia/FFmpeg 分类日志统一进入此文件。

- 单文件上限 2 MB；
- 保留当前文件和最多 4 个轮转文件；
- Windows 用户目录、应用数据目录、其他绝对路径和 `content://` URI 自动脱敏；
- 业务数据库、聊天内容、可信设备密钥不会加入诊断包。

## 崩溃与异常退出

- 每次启动创建 `session.json`，正常退出时标记为 `clean`；下次启动发现上次仍为 `running` 时生成异常退出记录。
- 未处理 C++ 异常会在日志中记录原因并触发平台崩溃捕获。
- Windows 未处理结构化异常生成 `native-crash.dmp` 和简要文本信息。
- Android/Linux 原生致命信号生成崩溃标记，同时继续交给系统默认处理，以保留系统 tombstone 能力。
- Android 未捕获 Java 异常保存线程、系统版本、设备型号和完整 Java 堆栈。
- QML 引擎警告和对象创建错误统一写入运行日志。

## 导出诊断包

设置页的“导出诊断包”生成标准 ZIP 文件，包含：

- `metadata.json`：应用、Qt、系统、内核、架构和本次会话信息；
- `logs/`：当前及轮转日志；
- `crashes/`：异常退出、Java 崩溃、原生崩溃文本和可用的 Windows minidump；
- `session.json` 与隐私说明。

桌面端保存到下载目录并打开所在文件夹；Android 端生成后打开系统分享面板。单个超过 16 MB 的崩溃文件不会自动加入，避免诊断包过大。

## 排障建议

复现问题后不要清理应用数据，直接导出诊断包。自动重连问题重点查看 `Peer connection state`、`Adopted control session` 和 `Ignoring duplicate control session`；媒体问题查看 `Allowed media peer`、`Incoming media socket` 和 `Rejected unpaired media peer`；传输问题按任务 ID 串联 `connecting`、`negotiating`、`resuming`、`transferring` 与校验结果。
