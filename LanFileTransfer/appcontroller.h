#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QSettings>
#include <QSqlDatabase>
#include <QVariantList>

class QSystemTrayIcon;
class QAction;

class AppController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(QString effectiveLanguage READ effectiveLanguage NOTIFY languageChanged)
    Q_PROPERTY(QString deviceName READ deviceName WRITE setDeviceName NOTIFY deviceNameChanged)
    Q_PROPERTY(QString downloadDirectory READ downloadDirectory WRITE setDownloadDirectory NOTIFY downloadDirectoryChanged)
    Q_PROPERTY(bool minimizeToTray READ minimizeToTray WRITE setMinimizeToTray NOTIFY minimizeToTrayChanged)
    Q_PROPERTY(bool keepAwake READ keepAwake WRITE setKeepAwake NOTIFY keepAwakeChanged)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    QString language() const;
    QString effectiveLanguage() const;
    QString deviceName() const;
    QString downloadDirectory() const;
    bool minimizeToTray() const;
    bool keepAwake() const;

    void setLanguage(const QString &value);
    void setDeviceName(const QString &value);
    void setDownloadDirectory(const QString &value);
    void setMinimizeToTray(bool value);
    void setKeepAwake(bool value);

    Q_INVOKABLE QString tr(const QString &key) const;
    Q_INVOKABLE QVariantList messages(const QString &peerKey, int limit = 500) const;
    Q_INVOKABLE QVariantList recentConversations() const;
    Q_INVOKABLE qint64 addMessage(const QString &peerKey, const QString &peerName,
                                  bool outgoing, const QString &kind, const QString &body,
                                  const QString &fileName = {}, qint64 fileSize = 0,
                                  const QString &transferId = {}, const QString &status = {});
    Q_INVOKABLE void updateTransfer(const QString &transferId, double progress, const QString &status,
                                    const QString &checksum = {});
    Q_INVOKABLE void updateTransferSize(const QString &transferId, qint64 fileSize);
    Q_INVOKABLE void clearConversation(const QString &peerKey);
    Q_INVOKABLE void showNotification(const QString &title, const QString &message);
    Q_INVOKABLE void copyToClipboard(const QString &text);
    Q_INVOKABLE void moveToBackground();
    Q_INVOKABLE void chooseFiles();
    Q_INVOKABLE void chooseReceiveDirectory();
    Q_INVOKABLE void hideToTray();
    Q_INVOKABLE void restoreWindow();
    Q_INVOKABLE QString diagnosticLogPath() const;
    Q_INVOKABLE QString exportDiagnosticBundle();

signals:
    void languageChanged();
    void deviceNameChanged();
    void downloadDirectoryChanged();
    void minimizeToTrayChanged();
    void keepAwakeChanged();
    void restoreRequested();
    void quitRequested();
    void storageError(const QString &message);
    void filesSelected(const QVariantList &files);
    void receiveDirectorySelected(const QString &uri);
    void networkEnvironmentChanged();
    void diagnosticBundleReady(const QString &path);
    void diagnosticExportFailed(const QString &message);

private:
    void openDatabase();
    void writeSetting(const QString &key, const QVariant &value);
    void configureTray();
    void updateTrayText();
    void applyAndroidBackgroundMode();

    mutable QSqlDatabase m_database;
    QSettings m_settings;
    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_restoreAction = nullptr;
    QAction *m_quitAction = nullptr;
};
