#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QNetworkProxy>
#include <QIcon>
#include <QDebug>



#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#include <QOperatingSystemVersion>

#endif

int main(int argc, char *argv[])
{

    qputenv("QT_QUICK_CONTROLS_STYLE", "Material");
    QGuiApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/logo.png"));
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);




    QQmlApplicationEngine engine;

    // 【核心诊断逻辑】
    // 如果 QML 加载失败，它会把具体的“哪一行报错”、“什么错误”打印到 Logcat 里
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [](const QUrl &url) {
                         qCritical() << "致命错误：QML 加载失败！";
                         qCritical() << "尝试加载的路径是：" << url;
                         QCoreApplication::exit(-1);
                     }, Qt::QueuedConnection);

    // 添加导入路径，确保模块搜索范围涵盖 QRC
    engine.addImportPath("qrc:/qt/qml");

    // 使用绝对路径加载（配合 CMake 的 RESOURCE_PREFIX）
    const QUrl url("qrc:/qt/qml/LanTransfer/Core/Main.qml");
    engine.load(url);


#ifdef Q_OS_ANDROID
    if (QOperatingSystemVersion::current().majorVersion() >= 11) {
        bool isManager = QJniObject::callStaticMethod<jboolean>("android/os/Environment", "isExternalStorageManager");
        if (!isManager) {
            // 注意：第三个参数必须是 "Ljava/lang/String;"
            QJniObject action = QJniObject::getStaticObjectField("android/provider/Settings",
                                                                 "ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION",
                                                                 "Ljava/lang/String;");
            QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V", action.object());

            // 【修改部分】：使用 Qt 6 官方推荐的 QNativeInterface 获取 Activity 上下文
            QJniObject activity = QNativeInterface::QAndroidApplication::context();

            // 增加 isValid() 判断是一个好习惯，防止 JNI 调用引起应用崩溃
            if (activity.isValid()) {
                activity.callMethod<void>("startActivity", "(Landroid/content/Intent;)V", intent.object());
            } else {
                qWarning() << "无法获取 Android Activity 上下文，权限请求失败";
            }
        }
    }
#endif


    return app.exec();
}
