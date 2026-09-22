# 可复现构建

## Windows 桌面端

要求：

- CMake 3.16 或更高版本；
- Qt 6 桌面 MinGW 套件，包含 Core、Gui、QML、Quick、Network、SQL、Widgets、Multimedia 和 Concurrent；
- 与 Qt 套件匹配的 MinGW 编译器。

如果 `qmake.exe` 和 `g++.exe` 已加入 `PATH`，在项目目录执行：

```powershell
.\build-desktop.ps1
```

也可以显式指定环境，避免依赖机器路径：

```powershell
.\build-desktop.ps1 `
  -QtRoot "C:\Qt\6.8.3\mingw_64" `
  -MinGwBin "C:\Qt\Tools\mingw1310_64\bin" `
  -Configuration Release
```

CMake 默认通过 `landrop_deploy_windows_runtime` 目标调用所选 Qt 套件中的
`windeployqt`，把 Qt DLL、QML 模块、平台插件和 MinGW 运行库复制到可执行文件
目录，并检查 `qwindows.dll`、`qsqlite.dll` 等关键文件。该目标属于默认构建，
即使程序本身无需重新链接，也会重新补齐被误删的运行库。因此从 Qt Creator、
命令行或其他 CMake 前端构建时，输出目录都可以脱离开发机的 Qt `PATH` 直接
运行。也可以单独执行以下命令修复已有运行目录：

```powershell
cmake --build <构建目录> --target landrop_deploy_windows_runtime
```

常用选项：

- `-Configuration Debug`：生成调试版本；
- `-Clean`：删除项目 `build` 目录内对应的构建目录后重新构建；
- `-SkipTests`：只编译，不执行测试；
- `-SkipPackage`：Release 构建后不生成分发压缩包；
- `-BuildDirectory <path>`：指定构建目录。相对路径以项目目录为基准。

如只需要内部开发构建、明确不希望复制运行库，可在配置时传入
`-DLANDROP_DEPLOY_WINDOWS_RUNTIME=OFF`；发布包和日常可运行构建应保持默认值 `ON`。

环境变量 `QT_ROOT` 和 `MINGW_BIN` 可替代相应参数。参数优先级高于环境变量。

Release 构建默认在 `dist/` 生成 `LanDrop-<版本>-windows-x64.zip` 和对应的
`.sha256` 文件。打包过程使用干净的 staging 目录重新运行 `windeployqt`，复制
第三方字体许可证，并从分发目录实际启动一次 `--smoke-test`；因此通过的不只是
开发构建目录，而是最终交付内容本身。

## Android

Android 构建建议通过 Qt Creator 的 Android Kit 或 Qt 的 Android CMake 工具链配置。仓库中的 `android/` 只保存应用清单、资源、Gradle 模板和 Java 桥接代码；Qt 路径、SDK 路径、生成的库及 Gradle 缓存不得提交。

当前 Android 构建基线为：

- `minSdk 28`，`compileSdk 36`，`targetSdk 36`；
- Android Gradle Plugin 8.9.1；
- Gradle 8.11.1；
- NDK 26.1.10909125。

API 36 至少需要 Android Gradle Plugin 8.9.1，后者至少需要 Gradle 8.11.1；
不要单独降低其中一个版本。`targetSdk 36` 同时满足 2026 年 8 月 31 日起
Google Play 对新应用和应用更新的目标 API 要求。

开发调试包可直接执行：

```powershell
.\build-android.ps1 `
  -QtAndroidRoot "C:\Qt\6.8.3\android_arm64_v8a" `
  -AndroidSdkRoot "C:\Android\Sdk"
```

脚本使用独立的 Ninja 构建目录，检查 API 36、NDK、JDK 17 和 Qt Host Tools，
并支持 `-Package Apk`、`-Package Aab` 或 `-Package Both`。Release 默认拒绝
生成未签名包；仅诊断时可显式使用 `-AllowUnsignedRelease`。

正式发布时先在当前终端或 CI 的密钥存储中提供以下环境变量：

```powershell
$env:QT_ANDROID_KEYSTORE_PATH = "C:\secure\upload-key.jks"
$env:QT_ANDROID_KEYSTORE_ALIAS = "upload"
$env:QT_ANDROID_KEYSTORE_STORE_PASS = "<从安全存储读取>"
$env:QT_ANDROID_KEYSTORE_KEY_PASS = "<从安全存储读取>"

.\build-android.ps1 `
  -Configuration Release `
  -Package Both `
  -Sign `
  -QtAndroidRoot "C:\Qt\6.8.3\android_arm64_v8a" `
  -AndroidSdkRoot "C:\Android\Sdk"
```

签名值不会作为命令行参数传递或写入工程；脚本会验证 APK/AAB 签名，随后把
发布包及 SHA-256 文件复制到 `dist/`。`.keystore`、`.jks` 和
`keystore.properties` 已被忽略，不能提交到仓库。Google Play 分发应优先上传
AAB；APK 主要用于本地设备测试或直接分发。若只想验证签名流水线而不把测试包
复制到 `dist/`，可额外传入 `-SkipDist`。

## 提交前检查

```powershell
git status --short
.\build-desktop.ps1 -Clean
```

预期结果：构建产物、IDE 用户配置和签名材料不会出现在 Git 状态中；桌面程序和全部测试成功完成。
