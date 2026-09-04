#pragma once
#include "transferstore.h"
#include <QFile>
#include <QMap>
#include <QObject>
#include <QQmlEngine>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QPointer>
#include <QHash>
#include <QSet>
#include <QVariantList>
#include <functional>

struct TransferContext {
    QString id, peerIp, fileName, finalPath, partialPath, sourcePath;
    QByteArray checksum;
    bool isSender=false, offerAccepted=false, completionSent=false, sourceEof=false;
    bool isControl=false, paired=false;
    QString peerId, peerName;
    quint16 peerPort=0;
    qint64 lastActivityMs=0;
    qint64 totalBytes=0, resumeOffset=0;
    QFile *file=nullptr;
    quint32 blockSize=0;
    int lastProgressPct=-1;
};

struct TransferManagerOptions {
    QString localId;
    QString settingsOrganization = QStringLiteral("Lantern Labs");
    QString settingsApplication = QStringLiteral("LanDrop");
    QString transferDatabasePath;
    QString saveDirectory;
    quint16 listenPort = 0;
    int reconnectBaseDelayMs = 1000;
    int maxReconnectAttempts = 5;
};

class TransferManager : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString saveDirectory READ saveDirectory WRITE setSaveDirectory NOTIFY saveDirectoryChanged)
    Q_PROPERTY(quint16 serverPort READ serverPort CONSTANT)
    Q_PROPERTY(QVariantList trustedPeers READ trustedPeers NOTIFY trustedPeersChanged)
public:
    enum class ConnectionState { Unpaired, Pairing, Online, Reconnecting, Offline };
    Q_ENUM(ConnectionState)

    explicit TransferManager(QObject *parent=nullptr);
    explicit TransferManager(const TransferManagerOptions &options, QObject *parent=nullptr);
    quint16 serverPort() const { return m_server->serverPort(); }
    QString saveDirectory() const { return m_saveDirectory; }
    QVariantList trustedPeers() const;
    Q_INVOKABLE void setSaveDirectory(const QString &dirUrl);
    Q_INVOKABLE void openFolder();
    Q_INVOKABLE void sendFiles(const QList<QUrl> &files,const QString &ip,quint16 port);
    Q_INVOKABLE void sendText(const QString &text,const QString &ip,quint16 port);
    Q_INVOKABLE void cancelTransfer(const QString &taskId);
    Q_INVOKABLE void restoreTransfers();
    Q_INVOKABLE void requestPairing(const QString &peerId,const QString &peerName,const QString &ip,quint16 port);
    Q_INVOKABLE void acceptPairing(const QString &peerId);
    Q_INVOKABLE void rejectPairing(const QString &peerId);
    Q_INVOKABLE void forgetPeer(const QString &peerId);
    Q_INVOKABLE void deviceAvailable(const QString &peerId,const QString &peerName,
                                     const QString &ip,quint16 port);
    Q_INVOKABLE bool isPaired(const QString &peerId) const;
    Q_INVOKABLE QString connectionState(const QString &peerId) const;
    enum MessageType:quint8 { MsgFileOffer=2,MsgFileData=3,MsgText=4,MsgResume=5,MsgComplete=6,
                              MsgPairRequest=7,MsgPairAccept=8,MsgPairReject=9,MsgPing=10,MsgPong=11 };
signals:
    void saveDirectoryChanged();
    void taskAdded(QString taskId,QString peerIp,QString fileName,bool isSender,qint64 totalBytes);
    void taskSizeResolved(QString taskId,qint64 totalBytes);
    void taskUpdated(QString taskId,qreal progress,QString status,QString checksum);
    void taskRestored(QString taskId,QString peerId,QString peerName,QString peerIp,
                      QString fileName,bool isSender,qint64 totalBytes,qreal progress,
                      QString status,QString checksum);
    void textReceived(QString ip,QString text);
    void transferError(QString message);
    void pairingRequested(QString peerId,QString peerName,QString ip);
    // Canonical values: unpaired, pairing, online, reconnecting, offline.
    void pairingStateChanged(QString peerId,QString state);
    void peerAuthorizationChanged(QString peerId,QString ip,bool allowed);
    void trustedPeersChanged();
private:
    friend class TransferManagerIntegrationTest;
    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void sendPacket(QTcpSocket *socket,MessageType type,const std::function<void(QDataStream&)> &writer={});
    void sendNextChunk(QTcpSocket *socket);
    void beginOutgoingTransfer(PersistedTransfer transfer,bool validateSource);
    void openOutgoingSocket(PersistedTransfer transfer);
    void enqueueOutgoing(const PersistedTransfer &transfer,bool validateSource);
    void resumePendingTransfers(const QString &peerId);
    QString peerIdForIp(const QString &ip) const;
    void finishReceive(QTcpSocket *socket);
    void cleanupSocket(QTcpSocket *socket,const QString &status={},bool preservePartial=true);
    QString availableFinalPath(const QString &name) const;
    QString storagePath() const;
    bool isPairedIp(const QString &ip) const;
    struct PeerConnection {
        QString name;
        QString ip;
        quint16 port=0;
        qint64 lastSeen=0;
        bool trusted=false;
        ConnectionState state=ConnectionState::Unpaired;
        int reconnectAttempt=0;
        quint64 reconnectGeneration=0;
    };
    static QString stateName(ConnectionState state);
    void setConnectionState(const QString &peerId,ConnectionState state);
    void loadTrustedPeers();
    bool isTrusted(const QString &peerId) const;
    void acceptPairingSocket(QTcpSocket *socket);
    void saveTrustedPeer(const QString &peerId,const QString &name,const QString &ip,quint16 port);
    void removeTrustedPeer(const QString &peerId);
    void openControlConnection(const QString &peerId,const QString &peerName,const QString &ip,
                               quint16 port,bool reconnecting);
    bool establishSession(QTcpSocket *socket,TransferContext *context);
    void scheduleReconnect(const QString &peerId);
    QString m_saveDirectory;
    QTcpServer *m_server=nullptr;
    QMap<QTcpSocket*,TransferContext*> m_tasks;
    QMap<QString,QPointer<QTcpSocket>> m_sessions;
    QHash<QString,PeerConnection> m_connections;
    struct PendingOutgoing {
        PersistedTransfer transfer;
        bool validateSource=false;
    };
    QHash<QString,PendingOutgoing> m_pendingOutgoing;
    QSet<QString> m_activeTransferIds;
    QSet<QString> m_cancelledTransferIds;
    QHash<QString,int> m_transferRetryAttempts;
    TransferStore m_transferStore;
    bool m_transfersRestored=false;
    QString m_localId;
    QString m_settingsOrganization;
    QString m_settingsApplication;
    int m_reconnectBaseDelayMs=1000;
    int m_maxReconnectAttempts=5;
};
