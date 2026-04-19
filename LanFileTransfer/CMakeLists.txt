# ============================================================================
# CMake 最低版本要求
# ============================================================================
# Qt6 需要 CMake 3.16 或更高版本以支持其现代目标模型和 qml 模块功能
cmake_minimum_required(VERSION 3.16)

# ============================================================================
# 项目基本信息
# ============================================================================
# 项目名称：LanFileTransfer
# 版本：0.1（可通过 project(… VERSION) 自动生成版本宏）
# 使用语言：仅 C++（LANGUAGES CXX 可减少不必要的语言检测，加快配置）
project(LanFileTransfer VERSION 0.1 LANGUAGES CXX)

# ============================================================================
# C++ 标准配置
# ============================================================================
# Qt6 核心库要求 C++17 或更高（例如 QByteArray::toStdString() 等特性）
set(CMAKE_CXX_STANDARD 17)
# 强制使用 C++17，如果编译器不支持则报错
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Qt 自动代码生成工具开关
# ============================================================================
# AUTOMOC: 自动对含有 Q_OBJECT 宏的头文件运行 moc（元对象编译器）
set(CMAKE_AUTOMOC ON)
# AUTORCC: 自动将 .qrc 资源文件编译为 C++ 源文件并链接
set(CMAKE_AUTORCC ON)
# AUTOUIC: 自动将 Qt Designer 的 .ui 文件转换为 ui_*.h 头文件
set(CMAKE_AUTOUIC ON)

# ============================================================================
# 查找 Qt6 组件
# ============================================================================
# REQUIRED 表示缺失任一组件立即停止配置并报错
# Core     - 非图形核心功能（信号槽、容器、字符串等）
# Gui      - 图形界面基础（窗口、事件、图像等）
# Qml      - QML 引擎与运行时
# Quick    - Qt Quick 模块（标准 QML 类型和控件）
# Network  - 网络功能（TCP/UDP、HTTP、本地发现等）
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Network)

# ============================================================================
# 创建可执行文件并嵌入资源
# ============================================================================
# qt_add_executable 是 add_executable 的 Qt 封装，自动处理：
#   1. 编译选项（如 -fPIC）
#   2. 链接 Qt 默认库（若未显式 link 也会补全）
#   3. 资源编译（配合 AUTORCC）
qt_add_executable(appLanFileTransfer
    main.cpp           # 程序入口，通常包含 QGuiApplication 和 QQmlApplicationEngine
    img/res.qrc        # 资源文件，其中声明的图片、字体等会嵌入可执行文件
)

# ============================================================================
# 注册 QML 模块（将 C++ 类型和 QML 文件暴露给 QML 引擎）
# ============================================================================
# qt_add_qml_module 为目标 appLanFileTransfer 关联一个 QML 模块
# 它会自动：
#   1. 生成 qmldir 文件并嵌入资源
#   2. 调用 qmltyperegistrar 注册 C++ 类型
#   3. 将 QML_FILES 和 SOURCES 中的文件加入构建
#   4. 确保模块可在 QML 中通过 import 语句使用
qt_add_qml_module(appLanFileTransfer
    # URI（统一资源标识符）：QML 中通过 "import LanTransfer.Core 1.0" 引入该模块
    URI LanTransfer.Core
    # 模块版本号，影响 QML 导入语句中的版本
    VERSION 1.0
    # 资源前缀：模块内的所有 QML 文件在资源系统中的根路径
    # 最终 Main.qml 可通过 "qrc:/qt/qml/LanTransfer.Core/Main.qml" 访问
    RESOURCE_PREFIX "/qt/qml"
    # 属于该模块的 QML 文件列表（自动加入资源）
    QML_FILES
        Main.qml
        DesktopLayout.qml
        MobileLayout.qml
    # 属于该模块的 C++ 源文件和头文件
    # 这些文件中定义的 QObject 派生类会自动注册到 QML 类型系统
    SOURCES
        transfermanager.h transfermanager.cpp
        discoveryservice.h discoveryservice.cpp

)

# ============================================================================
# 显式链接 Qt 库
# ============================================================================
# 虽然 qt_add_executable 和 qt_add_qml_module 可能隐式添加部分链接依赖，
# 但显式列出所需模块更清晰、可靠，并避免漏掉 Network 等非默认模块。
# PRIVATE 表示这些链接依赖仅用于本目标，不传递给其他依赖本目标的目标（无）。
target_link_libraries(appLanFileTransfer
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::Network
)

# ============================================================================
# Windows 平台配置：隐藏控制台窗口
# ============================================================================
# 对于 GUI 应用程序，不希望在启动时同时弹出黑色的命令行窗口。
# WIN32_EXECUTABLE 属性会使链接器选择 /SUBSYSTEM:WINDOWS 而不是 /SUBSYSTEM:CONSOLE。
if(WIN32)
    set_target_properties(appLanFileTransfer PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# ============================================================================
# Android 平台配置（打包部署相关）
# ============================================================================
if(ANDROID)
    # 指定 Android 部署包源目录，该目录下应包含 AndroidManifest.xml、build.gradle 等文件
    # Qt 的 androiddeployqt 工具会使用此目录生成最终 APK
    set_property(TARGET appLanFileTransfer PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")

    # 强制指定目标 ABI 为 arm64-v8a（适用于现代 64 位 ARM 设备，如骁龙 8 Gen 1）
    # 注意：该变量可能在 Android 工具链中被重新设置，更可靠的方式是使用 CMAKE_ANDROID_ARCH_ABI
    # 但此处保留以明确意图
endif()
