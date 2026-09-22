# 发布检查清单

## 代码与测试

- [ ] `git status --short` 只包含本次计划内改动。
- [ ] `git diff --check` 无空白错误。
- [ ] Windows Debug 与 Release 构建成功。
- [ ] CTest 全部通过。
- [ ] Android arm64 Debug APK 构建成功。
- [ ] 正式发布使用私有上传密钥生成签名 APK/AAB。
- [ ] `docs/qa-matrix.md` 中本次发布要求的真机场景已填写。

## 版本与兼容性

- [ ] 更新 `project(... VERSION ...)`、Android versionCode/versionName。
- [ ] 更新 `CHANGELOG.md`。
- [ ] 两端使用相同协议版本；升级后按提示重新配对。
- [ ] 数据库和 QSettings 升级不会删除消息历史或已完成文件。

## Windows 产物

- [ ] 使用 `build-desktop.ps1 -Configuration Release` 生成 ZIP。
- [ ] 从 staging 目录运行 `--smoke-test`。
- [ ] ZIP 中存在 Qt、QML、平台插件、SQLite 插件、Multimedia/FFmpeg 和 MinGW 运行库。
- [ ] 在未安装 Qt 的 Windows 机器上启动并完成一次发送。
- [ ] 发布 ZIP 与 `.sha256`。

## Android 产物

- [ ] `targetSdk 36`、JDK 17、NDK 26.1 与构建文档一致。
- [ ] APK/AAB 使用正式密钥签名并通过签名验证。
- [ ] 安装升级不会清除 SQLite 历史、可信设备和 SAF 目录授权。
- [ ] 前台服务通知、摄像头、麦克风、文件和 MediaProjection 权限均验证。
- [ ] 上传 Google Play 时优先使用 AAB，不提交密钥或密码。

## 发布页面

- [ ] README 截图与当前版本一致。
- [ ] 已知限制明确写出：局域网使用、明文 TCP、真机支持范围。
- [ ] 附安装说明、变更摘要、系统要求和 SHA-256。
- [ ] 创建带注释的版本标签，例如 `v0.1.0`。

