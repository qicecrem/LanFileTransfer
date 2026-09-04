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

脚本默认配置、编译、调用 `windeployqt` 复制 Qt/QML 插件与 MinGW 运行库，然后运行全部测试。构建目录因此可以脱离开发机的 Qt `PATH` 直接运行。常用选项：

- `-Configuration Debug`：生成调试版本；
- `-Clean`：删除项目 `build` 目录内对应的构建目录后重新构建；
- `-SkipTests`：只编译，不执行测试；
- `-BuildDirectory <path>`：指定构建目录。相对路径以项目目录为基准。

环境变量 `QT_ROOT` 和 `MINGW_BIN` 可替代相应参数。参数优先级高于环境变量。

## Android

Android 构建建议通过 Qt Creator 的 Android Kit 或 Qt 的 Android CMake 工具链配置。仓库中的 `android/` 只保存应用清单、资源、Gradle 模板和 Java 桥接代码；Qt 路径、SDK 路径、生成的库及 Gradle 缓存不得提交。

至少验证以下目标：

```powershell
cmake --build <android-build-directory> --target apk
```

正式发布还需要在本机或 CI 的安全变量中配置签名文件。`.keystore`、`.jks` 和 `keystore.properties` 已被忽略，不能提交到仓库。

## 提交前检查

```powershell
git status --short
.\build-desktop.ps1 -Clean
```

预期结果：构建产物、IDE 用户配置和签名材料不会出现在 Git 状态中；桌面程序和全部测试成功完成。
