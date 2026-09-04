#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QNetworkProxy>
#include <QIcon>
#include <QDebug>
#include <QDir>
#include <QTimer>
#include <QQuickWindow>
#include "callmanager.h"
#include "diagnosticlog.h"



#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

int main(int argc, char *argv[])
{

    qputenv("QT_QUICK_CONTROLS_STYLE", "Material");
    QApplication app(argc, argv);
    app.setApplicationName("LanDrop");
    app.setOrganizationName("Lantern Labs");
    app.setApplicationVersion("0.1");
    DiagnosticLog::install();
#ifdef Q_OS_ANDROID
    const QJniObject diagnosticContext = QNativeInterface::QAndroidApplication::context();
    if (diagnosticContext.isValid()) {
        const QJniObject crashDirectory = QJniObject::fromString(
            QDir(DiagnosticLog::directory()).filePath(QStringLiteral("crashes")));
        QJniObject::callStaticMethod<void>("org/landrop/app/CrashReporter", "install",
                                           "(Landroid/content/Context;Ljava/lang/String;)V",
                                           diagnosticContext.object(), crashDirectory.object());
    }
    // Qt loads multimedia plugins through dlopen(), which does not run the
    // plugin's JNI_OnLoad. Explicit Java loading registers QtCamera2 natives.
    QJniObject::callStaticMethod<void>("org/landrop/app/QtMultimediaLoader",
                                       "ensureLoaded", "()V");
#endif
    app.setQuitOnLastWindowClosed(true);
    app.setWindowIcon(QIcon(":/logo.png"));
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);




    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlEngine::warnings, &app,
                     [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            qWarning().noquote() << "QML" << warning.toString();
    });
    auto *callFrames = new CallFrameProvider;
    CallManager::setFrameProvider(callFrames);
    engine.addImageProvider("call", callFrames);

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

    if (!engine.rootObjects().isEmpty()) {
        QObject *rootObject = engine.rootObjects().constFirst();
        if (qEnvironmentVariableIsSet("LANDROP_DESIGN_PREVIEW"))
            QMetaObject::invokeMethod(rootObject, "loadDesignPreview");
        if (qEnvironmentVariableIsSet("LANDROP_CALL_PREVIEW"))
            QMetaObject::invokeMethod(rootObject, "loadCallPreview");
    }

    const QString screenshotPath = qEnvironmentVariable("LANDROP_SCREENSHOT_PATH");
    if (!screenshotPath.isEmpty()) {
        QTimer::singleShot(1400, &app, [&engine, screenshotPath]() {
            if (engine.rootObjects().isEmpty()) {
                QCoreApplication::exit(2);
                return;
            }
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
            if (!window || !window->grabWindow().save(screenshotPath)) {
                QCoreApplication::exit(3);
                return;
            }
            QCoreApplication::quit();
        });
    }

    bool smokeTestOk = false;
    const int smokeTestMs = qEnvironmentVariableIntValue("LANDROP_SMOKE_TEST_MS", &smokeTestOk);
    if (smokeTestOk && smokeTestMs > 0)
        QTimer::singleShot(smokeTestMs, &app, &QCoreApplication::quit);


#ifdef Q_OS_ANDROID
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (activity.isValid())
        QJniObject::callStaticMethod<void>("org/landrop/app/LanTransferService", "requestRuntimePermissions",
                                          "(Landroid/app/Activity;)V", activity.object());
#endif


    const int result = app.exec();
    DiagnosticLog::markCleanShutdown();
    return result;
}
