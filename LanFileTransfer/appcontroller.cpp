#include "appcontroller.h"
#include "diagnosticlog.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QHostInfo>
#include <QMenu>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QPointer>
#include <QDesktopServices>
#include <QFileInfo>
#include <QUrl>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace {
QPointer<AppController> androidController;

QVariantMap rowToMessage(const QSqlQuery &q)
{
    return {{"id", q.value(0)}, {"peerKey", q.value(1)}, {"peerName", q.value(2)},
            {"outgoing", q.value(3).toBool()}, {"kind", q.value(4)}, {"body", q.value(5)},
            {"fileName", q.value(6)}, {"fileSize", q.value(7)}, {"transferId", q.value(8)},
            {"progress", q.value(9)}, {"status", q.value(10)}, {"checksum", q.value(11)},
            {"createdAt", q.value(12)}};
}
}

AppController::AppController(QObject *parent)
    : QObject(parent), m_settings("Lantern Labs", "LanDrop")
{
    androidController = this;
    openDatabase();
    configureTray();
    applyAndroidBackgroundMode();
}

AppController::~AppController()
{
    if (androidController == this) androidController.clear();
    if (m_database.isOpen()) m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase("landrop-history");
}

QString AppController::language() const { return m_settings.value("ui/language", "zh_CN").toString(); }
QString AppController::deviceName() const
{
    const QString fallback = QHostInfo::localHostName().isEmpty() ? QStringLiteral("LanDrop") : QHostInfo::localHostName();
    return m_settings.value("identity/deviceName", fallback).toString();
}
QString AppController::downloadDirectory() const
{
#ifdef Q_OS_ANDROID
    const QString fallback = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                 .filePath(QStringLiteral("Received"));
#else
    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
#endif
    return m_settings.value("transfer/downloadDirectory",
                            fallback).toString();
}
bool AppController::minimizeToTray() const { return m_settings.value("desktop/minimizeToTray", true).toBool(); }
bool AppController::keepAwake() const { return m_settings.value("mobile/keepAwake", true).toBool(); }

void AppController::writeSetting(const QString &key, const QVariant &value)
{
    m_settings.setValue(key, value);
    m_settings.sync();
}
void AppController::setLanguage(const QString &v) { if (v == language()) return; writeSetting("ui/language", v); emit languageChanged(); }
void AppController::setDeviceName(const QString &v) { const auto s=v.trimmed(); if (s.isEmpty()||s==deviceName()) return; writeSetting("identity/deviceName",s); emit deviceNameChanged(); }
void AppController::setDownloadDirectory(const QString &v) { if (v.isEmpty()||v==downloadDirectory()) return; writeSetting("transfer/downloadDirectory",v); emit downloadDirectoryChanged(); }
void AppController::setMinimizeToTray(bool v) { if(v==minimizeToTray()) return; writeSetting("desktop/minimizeToTray",v); emit minimizeToTrayChanged(); }
void AppController::setKeepAwake(bool v) { if(v==keepAwake()) return; writeSetting("mobile/keepAwake",v); applyAndroidBackgroundMode(); emit keepAwakeChanged(); }

QString AppController::tr(const QString &key) const
{
    static const QHash<QString, QStringList> words = {
        {"appName", {"飞传", "LanDrop"}}, {"nearby", {"附近设备", "Nearby"}},
        {"online", {"在线", "online"}}, {"search", {"搜索设备", "Search devices"}},
        {"selectPeer", {"选择一台设备开始会话", "Choose a device to start"}},
        {"noPeers", {"暂未发现设备", "No devices found"}}, {"sameWifi", {"确认设备处于同一 Wi-Fi", "Make sure both devices use the same Wi-Fi"}},
        {"message", {"输入消息", "Write a message"}}, {"send", {"发送", "Send"}},
        {"file", {"文件", "File"}}, {"video", {"视频", "Video"}}, {"screen", {"共享屏幕", "Share screen"}},
        {"settings", {"设置", "Settings"}}, {"language", {"语言", "Language"}},
        {"deviceName", {"设备名称", "Device name"}}, {"downloads", {"接收目录", "Downloads"}},
        {"minimizeToTray", {"关闭窗口时最小化到托盘", "Minimize to tray when closing"}},
        {"keepAwake", {"尽量保持后台发现与传输", "Keep discovery and transfers active when possible"}},
        {"emptyChat", {"消息和文件会出现在这里", "Messages and files will appear here"}},
        {"incomingCall", {"收到视频请求", "Incoming video request"}}, {"accept", {"接听", "Accept"}},
        {"decline", {"拒绝", "Decline"}}, {"hangup", {"结束", "End"}},
        {"verified", {"校验通过", "Verified"}}, {"retrying", {"等待续传", "Waiting to resume"}},
        {"clear", {"清空会话", "Clear conversation"}}, {"chooseFolder", {"选择目录", "Choose folder"}}
    };
    const auto it = words.constFind(key);
    if (it == words.constEnd()) return key;
    return it.value().at(language().startsWith("zh") ? 0 : 1);
}

void AppController::openDatabase()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(root);
    m_database = QSqlDatabase::addDatabase("QSQLITE", "landrop-history");
    m_database.setDatabaseName(QDir(root).filePath("history.sqlite3"));
    if (!m_database.open()) { emit storageError(m_database.lastError().text()); return; }
    QSqlQuery q(m_database);
    q.exec("PRAGMA journal_mode=WAL");
    q.exec("PRAGMA foreign_keys=ON");
    if (!q.exec("CREATE TABLE IF NOT EXISTS messages ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, peer_key TEXT NOT NULL, peer_name TEXT,"
                "outgoing INTEGER NOT NULL, kind TEXT NOT NULL, body TEXT, file_name TEXT,"
                "file_size INTEGER DEFAULT 0, transfer_id TEXT, progress REAL DEFAULT 0,"
                "status TEXT, checksum TEXT, created_at INTEGER NOT NULL)")) emit storageError(q.lastError().text());
    q.exec("CREATE INDEX IF NOT EXISTS idx_messages_peer_time ON messages(peer_key, created_at)");
    q.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_messages_transfer ON messages(transfer_id) WHERE transfer_id IS NOT NULL AND transfer_id <> ''");
}

QVariantList AppController::messages(const QString &peerKey, int limit) const
{
    QVariantList result; if (!m_database.isOpen()) return result;
    QSqlQuery q(m_database);
    q.prepare("SELECT id,peer_key,peer_name,outgoing,kind,body,file_name,file_size,transfer_id,progress,status,checksum,created_at "
              "FROM messages WHERE peer_key=? ORDER BY created_at DESC LIMIT ?");
    q.addBindValue(peerKey); q.addBindValue(qBound(1,limit,2000));
    if (!q.exec()) return result;
    while(q.next()) result.prepend(rowToMessage(q));
    return result;
}

qint64 AppController::addMessage(const QString &peerKey, const QString &peerName, bool outgoing,
                                 const QString &kind, const QString &body, const QString &fileName,
                                 qint64 fileSize, const QString &transferId, const QString &status)
{
    if (!m_database.isOpen()) return -1;
    QSqlQuery q(m_database);
    q.prepare("INSERT OR IGNORE INTO messages(peer_key,peer_name,outgoing,kind,body,file_name,file_size,transfer_id,progress,status,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    q.addBindValue(peerKey); q.addBindValue(peerName); q.addBindValue(outgoing); q.addBindValue(kind);
    q.addBindValue(body); q.addBindValue(fileName); q.addBindValue(fileSize); q.addBindValue(transferId);
    q.addBindValue(0.0); q.addBindValue(status); q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) { emit storageError(q.lastError().text()); return -1; }
    return q.lastInsertId().toLongLong();
}

void AppController::updateTransfer(const QString &id, double progress, const QString &status, const QString &checksum)
{
    if (!m_database.isOpen() || id.isEmpty()) return;
    QSqlQuery q(m_database);
    q.prepare("UPDATE messages SET progress=?,status=?,checksum=CASE WHEN ?='' THEN checksum ELSE ? END WHERE transfer_id=?");
    q.addBindValue(progress); q.addBindValue(status); q.addBindValue(checksum); q.addBindValue(checksum); q.addBindValue(id); q.exec();
}

void AppController::updateTransferSize(const QString &id, qint64 fileSize)
{
    if (!m_database.isOpen() || id.isEmpty() || fileSize < 0) return;
    QSqlQuery q(m_database);
    q.prepare("UPDATE messages SET file_size=? WHERE transfer_id=?");
    q.addBindValue(fileSize);
    q.addBindValue(id);
    if (!q.exec()) emit storageError(q.lastError().text());
}

void AppController::clearConversation(const QString &peerKey)
{
    QSqlQuery q(m_database); q.prepare("DELETE FROM messages WHERE peer_key=?"); q.addBindValue(peerKey); q.exec();
}

void AppController::configureTray()
{
#ifndef Q_OS_ANDROID
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    m_tray = new QSystemTrayIcon(QIcon(":/logo.png"), this);
    auto *menu = new QMenu;
    menu->addAction(tr("appName"), this, &AppController::restoreWindow);
    menu->addSeparator();
    menu->addAction(language().startsWith("zh") ? "退出" : "Quit", this, &AppController::quitRequested);
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r){ if(r==QSystemTrayIcon::Trigger||r==QSystemTrayIcon::DoubleClick) restoreWindow(); });
    m_tray->show();
#endif
}
void AppController::showNotification(const QString &title,const QString &message) {
#ifdef Q_OS_ANDROID
    const auto context = QNativeInterface::QAndroidApplication::context();
    if (context.isValid()) {
        const QJniObject jTitle = QJniObject::fromString(title);
        const QJniObject jMessage = QJniObject::fromString(message);
        QJniObject::callStaticMethod<void>("org/landrop/app/LanTransferService", "showNotification",
                                           "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)V",
                                           context.object(), jTitle.object(), jMessage.object());
    }
#else
    if(m_tray) m_tray->showMessage(title,message,QSystemTrayIcon::Information,3500);
#endif
}
void AppController::copyToClipboard(const QString &text)
{
    if (auto *clipboard = QApplication::clipboard())
        clipboard->setText(text);
}
void AppController::moveToBackground()
{
#ifdef Q_OS_ANDROID
    const QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (activity.isValid())
        activity.callMethod<jboolean>("moveTaskToBack", "(Z)Z", true);
#endif
}
void AppController::chooseReceiveDirectory()
{
#ifdef Q_OS_ANDROID
    const QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (activity.isValid()) activity.callMethod<void>("requestReceiveDirectory", "()V");
#endif
}
void AppController::hideToTray() { if(m_tray) m_tray->showMessage(tr("appName"), language().startsWith("zh")?"仍在后台接收文件":"Still receiving in the background",QSystemTrayIcon::Information,2500); }
void AppController::restoreWindow() { emit restoreRequested(); }
QString AppController::diagnosticLogPath() const { return DiagnosticLog::path(); }

QString AppController::exportDiagnosticBundle()
{
    QString error;
#ifdef Q_OS_ANDROID
    const QString exportDirectory = QDir(DiagnosticLog::directory()).filePath(QStringLiteral("exports"));
    const QString result = DiagnosticLog::createBundle(exportDirectory, &error);
#else
    const QString result = DiagnosticLog::createBundle({}, &error);
#endif
    if (result.isEmpty()) {
        emit diagnosticExportFailed(error.isEmpty() ? tr("无法导出诊断包") : error);
        return {};
    }
#ifdef Q_OS_ANDROID
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (context.isValid()) {
        const QJniObject filePath = QJniObject::fromString(result);
        QJniObject::callStaticMethod<void>("org/landrop/app/LanTransferService", "shareFile",
                                           "(Landroid/content/Context;Ljava/lang/String;)V",
                                           context.object(), filePath.object());
    }
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(result).absolutePath()));
#endif
    emit diagnosticBundleReady(result);
    return result;
}

void AppController::applyAndroidBackgroundMode()
{
#ifdef Q_OS_ANDROID
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) return;
    QJniObject::callStaticMethod<void>("org/landrop/app/LanTransferService", "setEnabled",
                                      "(Landroid/content/Context;Z)V", context.object(), jboolean(keepAwake()));
#endif
}

#ifdef Q_OS_ANDROID
extern "C" Q_DECL_EXPORT void JNICALL
Java_org_landrop_app_LanTransferActivity_nativeReceiveDirectorySelected(
    JNIEnv *, jobject, jstring value)
{
    const QString uri = QJniObject::fromLocalRef(value).toString();
    if (!androidController || uri.isEmpty()) return;
    QMetaObject::invokeMethod(androidController, [uri] {
        if (androidController) emit androidController->receiveDirectorySelected(uri);
    }, Qt::QueuedConnection);
}

extern "C" Q_DECL_EXPORT void JNICALL
Java_org_landrop_app_LanTransferActivity_nativeNetworkChanged(JNIEnv *, jobject)
{
    if (!androidController) return;
    QMetaObject::invokeMethod(androidController, [] {
        if (androidController) emit androidController->networkEnvironmentChanged();
    }, Qt::QueuedConnection);
}
#endif
