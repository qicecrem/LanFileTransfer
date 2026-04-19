
#  LanFileTransfer (飞传)



**LanFileTransfer（飞传）** 是一款基于 Qt6 和 QML 开发的跨平台局域网文件与文本传输工具。无需繁琐的配置，无需借助外网，只要设备处于同一个 Wi-Fi 或局域网下，即可实现 PC 与 PC、PC 与手机、手机与手机之间的极速文件互传和文本消息共享。

## ✨ 核心特性

*   🔍 **零配置自动发现**：基于 UDP 组播（Multicast）技术，打开应用即可自动扫描并显示局域网内的在线设备。
*   💻📱 **双端自适应 UI**：
    *   **PC 端**：采用左右分栏的现代桌面设计，支持全局**拖拽文件发送**。
    *   **移动端**：采用沉浸式 SwipeView 配合底部导航栏，符合单手操作直觉。
*   📁 **高速文件互传**：基于 TCP 底层 Socket 的点对点（P2P）传输，突破百兆/千兆带宽极限，支持多任务并发与进度实时显示。
*   💬 **局域网剪贴板共享**：内置文本聊天功能，支持一键复制到系统剪贴板，方便在多设备间传递链接、验证码等文本。
*   ✏️ **个性化设备名称**：自动生成随机设备名，并支持随时修改，改名后局域网内立即同步刷新。
*   🛡️ **网络诊断辅助**：Windows 端内置“一键修复防火墙”功能，自动提权配置 Inbound 规则，解决设备互相看不见的问题。
*   📱 **Android 深度适配**：完美解决 Android 11+ 的 Scoped Storage 权限问题，并利用 JNI 底层正确解析 `content://` URI 获取真实文件名。

---

## 📸 界面预览

*(提示：建议截图后将图片放入项目的 `docs` 或 `img` 文件夹，并替换以下链接)*

- Windows 桌面端展示 
![](assets/pic1(1).png)
![](assets/pic1(2).png)


 - Android 移动端展示 
 ![](assets/pic2%20(1).jpg)
 ![](assets/pic2%20(2).jpg)
 ![](assets/pic2%20(3).jpg)

---

## 🛠️ 技术栈与架构

*   **GUI 框架**：Qt 6.5+ (Qt Quick / QML)
*   **UI 风格**：Qt Quick Controls - Material Design
*   **编程语言**：C++ 17, ECMAScript (QML JS)
*   **构建系统**：CMake 3.16+
*   **核心服务**：
    *   `DiscoveryService`: UDP Socket, 组播地址 `239.255.43.21`，端口 `45454`。
    *   `TransferManager`: TCP Server/Socket，动态分配端口，自定义二进制协议（Type + Size + Payload）解决粘包问题。

---

## 🚀 编译与运行指南

### 1. 环境准备
*   安装 [Qt Creator](https://www.qt.io/download) 和 Qt 6.5 或更高版本。
*   安装时需勾选相应的组件：`MSVC` / `MinGW` (PC)、`Android`。
*   若编译 Android 版本，需提前在 Qt Creator (Edit -> Preferences -> Devices -> Android) 中配置好 JDK 11+、Android SDK 和 NDK。

### 2. PC 端 (Windows) 编译与打包
1. 用 Qt Creator 打开项目根目录下的 `CMakeLists.txt`。
2. 选择 `Desktop Qt 6.x.x MinGW/MSVC 64-bit` 构建套件。
3. 选择 **Release** 模式，点击运行（`Ctrl+R`）验证程序。
4. **打包发布 (windeployqt)**：
    * 新建一个空文件夹 `LanFileTransfer_Release`，将编译生成的 `appLanFileTransfer.exe` 复制进去。
    * 打开 Qt 对应的命令行工具（如 *Qt 6.5.3 (MinGW 64-bit)*）。
    * 执行打包命令：
      ```cmd
      cd /d C:\你的空文件夹路径
      windeployqt --qmldir "C:\你的项目源码路径" --release appLanFileTransfer.exe
      ```
    * 此时文件夹内会生成所有依赖的 DLL 和 QML 插件，可直接压缩发给他人使用。

### 3. 移动端 (Android) 编译与打包
1. 确保手机与电脑在同一局域网，并开启 USB 调试。
2. 选择 `Android Qt 6.x.x Clang` 构建套件。
3. 在左侧面板的 **Projects (项目)** -> **Build** 选项中：
    * 找到 **Build Android APK** -> **Application Binary Interfaces (ABIs)**，勾选 `arm64-v8a`（适配现代手机）和 `armeabi-v7a`。
4. **配置正式签名 (Release 模式必须配置，否则安装提示损坏)**：
    * 在 Build Android APK 设置中，找到 **Keystore** 选项。
    * 点击 `Create...` 生成一个新的 `.keystore` 文件（务必牢记密码和别名）。
    * 勾选 **Sign package** 并选择刚创建的密钥。
5. 编译并生成 APK。直接安装到手机即可。

---

## 📖 使用说明

1. **设备发现**：
   * 确保两台或多台设备连接在**同一个 Wi-Fi** 或插入同一个路由器的网线。
   * 打开软件，左侧/发现页会自动列出其他设备。
   * *(Windows 如果搜索不到，请点击左下角的 `🛡️ 修复不可见` 按钮放行防火墙)*。
2. **修改名称**：
   * 点击带有“🙋‍♂️”或“💻”图标旁边的文本框，可以直接修改本机名称。失去焦点后即生效，其他设备上的列表会自动更新。
3. **发送文件**：
   * 点击选中目标设备。
   * **PC端**：将文件直接拖入主界面，或者点击“选择文件发送”。
   * **手机端**：点击右下角悬浮按钮（+）选择文件。
4. **接收文件**：
   * 默认自动接收并存入操作系统的“下载 (Downloads)”文件夹。可在界面下方更改保存路径。
   * 点击记录右侧的菜单或长按，可快速打开文件所在目录。

---

## ❓ 常见问题排查 (FAQ)


*   **Q: 为什么 PC 和手机连在同一个 Wi-Fi 却互相看不见？**
    *   **A**: 绝大多数是因为 Windows 防火墙拦截了 UDP 组播包或 TCP 传入连接。请在 PC 端点击“修复不可见”按钮（需管理员权限），如果仍无效，请检查路由器是否开启了“AP隔离”功能。



