#include "transfermanager.h"
#include "transferprotocol.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHostInfo>
#include <QMessageAuthenticationCode>
#include <QPointer>
#include <QRandomGenerator>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <utility>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace {
constexpr quint32 MaxPacketSize = 4 * 1024 * 1024;
constexpr qsizetype MaxDataChunkSize = 512 * 1024;
constexpr qsizetype MaxTextLength = 64 * 1024;
constexpr qint64 SessionTimeoutMs = 45000;
constexpr qint64 ConnectionTimeoutMs = 15000;
constexpr qint64 IncomingHandshakeTimeoutMs = 15000;
constexpr qint64 PairingDecisionTimeoutMs = 120000;
constexpr qint64 TransferIdleTimeoutMs = 90000;
constexpr qint64 VerificationTimeoutMs = 10 * 60 * 1000;
constexpr qint64 StorageReserveBytes = 16 * 1024 * 1024;
constexpr int MaxChecksumRetries = 1;
constexpr quint16 PairingProtocolVersion = 2;
constexpr qsizetype AuthenticationKeySize = 32;
constexpr qsizetype AuthenticationChallengeSize = 32;
constexpr qsizetype AuthenticationProofSize = 32;

QByteArray randomAuthenticationBytes(qsizetype size)
{
    QByteArray result(size, Qt::Uninitialized);
    for (qsizetype index = 0; index < size; ++index)
        result[index] = char(QRandomGenerator::system()->generate() & 0xff);
    return result;
}

bool constantTimeEqual(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size()) return false;
    uchar difference = 0;
    for (qsizetype index = 0; index < left.size(); ++index)
        difference |= uchar(left.at(index)) ^ uchar(right.at(index));
    return difference == 0;
}

bool isValidPeerId(const QString &peerId)
{
    return !peerId.isEmpty() && peerId.size() <= 128
        && !peerId.contains(QLatin1Char('/')) && !peerId.contains(QLatin1Char('\\'));
}

bool hasStorageCapacity(const QString &directory, qint64 totalBytes, qint64 existingBytes)
{
    if (totalBytes <= 0) return true;
    const qint64 reusable = existingBytes >= 0 && existingBytes <= totalBytes ? existingBytes : 0;
    const qint64 remaining = totalBytes - reusable;
    if (remaining <= 0) return true;
    const QStorageInfo storage(directory);
    if (!storage.isValid() || !storage.isReady() || storage.bytesAvailable() < 0)
        return true; // The provider cannot report capacity; let the write path decide.
    const qint64 available = storage.bytesAvailable();
    return available > StorageReserveBytes
        && remaining <= available - StorageReserveBytes;
}

struct DigestResult {
    QByteArray hash;
    qint64 size = -1;
    QString error;
};

DigestResult calculateDigest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {{}, -1, file.errorString()};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 bytesRead = 0;
    while (true) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
            return {{}, -1, file.errorString()};
        if (chunk.isEmpty()) break;
        hash.addData(chunk);
        bytesRead += chunk.size();
    }
    // Android content:// providers frequently report QFile::size() as zero.
    // Counting the bytes read works for both local files and document URIs.
    return {hash.result(), bytesRead, {}};
}

#ifdef Q_OS_ANDROID
bool persistAndroidDirectory(const QString &uri)
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject value = QJniObject::fromString(uri);
    return context.isValid() && QJniObject::callStaticMethod<jboolean>(
        "org/landrop/app/StorageBridge", "persistDirectory",
        "(Landroid/content/Context;Ljava/lang/String;)Z", context.object(), value.object());
}

bool copyToAndroidDirectory(const QString &treeUri, const QString &sourcePath, const QString &fileName)
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject directory = QJniObject::fromString(treeUri);
    const QJniObject source = QJniObject::fromString(sourcePath);
    const QJniObject name = QJniObject::fromString(fileName);
    return context.isValid() && QJniObject::callStaticMethod<jboolean>(
        "org/landrop/app/StorageBridge", "copyToDirectory",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z",
        context.object(), directory.object(), source.object(), name.object());
}
#endif
}

QByteArray TransferManager::controlRequestProof(const QByteArray &key,
                                                const QString &senderId,
                                                const QString &recipientId,
                                                bool reconnecting,
                                                const QByteArray &challenge)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    stream << QByteArrayLiteral("landrop-control-request-v2")
           << senderId << recipientId << reconnecting << challenge;
    return QMessageAuthenticationCode::hash(payload, key, QCryptographicHash::Sha256);
}

QByteArray TransferManager::controlAcceptProof(const QByteArray &key,
                                               const QString &senderId,
                                               const QString &recipientId,
                                               const QByteArray &requestChallenge,
                                               const QByteArray &responseChallenge)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    stream << QByteArrayLiteral("landrop-control-accept-v2")
           << senderId << recipientId << requestChallenge << responseChallenge;
    return QMessageAuthenticationCode::hash(payload, key, QCryptographicHash::Sha256);
}

QByteArray TransferManager::fileOfferProof(const QByteArray &key,
                                           const QString &senderId,
                                           const QString &recipientId,
                                           const QByteArray &challenge,
                                           const QString &transferId,
                                           const QString &fileName,
                                           qint64 totalBytes,
                                           const QByteArray &checksum)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    stream << QByteArrayLiteral("landrop-file-offer-v2")
           << senderId << recipientId << challenge << transferId
           << fileName << totalBytes << checksum;
    return QMessageAuthenticationCode::hash(payload, key, QCryptographicHash::Sha256);
}

QByteArray TransferManager::mediaAuthorizationKey(const QByteArray &key)
{
    return QMessageAuthenticationCode::hash(
        QByteArrayLiteral("landrop-media-authorization-v2"), key,
        QCryptographicHash::Sha256);
}

TransferManager::TransferManager(QObject *parent)
    : TransferManager(TransferManagerOptions{}, parent)
{
}

TransferManager::TransferManager(const TransferManagerOptions &options, QObject *parent)
    : QObject(parent),
      m_transferStore(options.transferDatabasePath),
      m_settingsOrganization(options.settingsOrganization),
      m_settingsApplication(options.settingsApplication),
      m_reconnectBaseDelayMs(qMax(1, options.reconnectBaseDelayMs)),
      m_maxReconnectAttempts(qMax(1, options.maxReconnectAttempts))
{
    connect(this, &TransferManager::taskUpdated, this,
            [this](const QString &id, qreal progress, const QString &status, const QString &checksum) {
        m_transferStore.updateState(id, progress, status, QByteArray::fromHex(checksum.toLatin1()));
    });
    connect(this, &TransferManager::taskSizeResolved, this,
            [this](const QString &id, qint64 totalBytes) {
        m_transferStore.updateSize(id, totalBytes);
    });
    QSettings settings(m_settingsOrganization, m_settingsApplication);
    m_localId = options.localId.isEmpty()
        ? settings.value(QStringLiteral("identity/instanceId")).toString()
        : options.localId;
    if (m_localId.isEmpty()) {
        m_localId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("identity/instanceId"), m_localId);
    }
    loadTrustedPeers();
    if (!options.saveDirectory.isEmpty()) {
        m_saveDirectory = options.saveDirectory;
    } else {
#ifdef Q_OS_ANDROID
        m_saveDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                              .filePath(QStringLiteral("Received"));
#else
        m_saveDirectory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
#endif
    }
    QDir().mkpath(m_saveDirectory);
    m_server = new QTcpServer(this);
    if (!m_server->listen(QHostAddress::AnyIPv4, options.listenPort))
        emit transferError(tr("无法启动文件接收服务：%1").arg(m_server->errorString()));
    connect(m_server, &QTcpServer::newConnection, this, &TransferManager::onNewConnection);
    auto *keepAlive = new QTimer(this);
    keepAlive->setInterval(15000);
    connect(keepAlive, &QTimer::timeout, this, &TransferManager::sweepSockets);
    keepAlive->start();
}

void TransferManager::sweepSockets()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    struct ExpiredSocket { QTcpSocket *socket; QString status; bool preservePartial; };
    QList<ExpiredSocket> expired;
    for (auto it = m_tasks.cbegin(); it != m_tasks.cend(); ++it) {
        QTcpSocket *socket = it.key();
        TransferContext *context = it.value();
        if (!socket || !context || context->lastActivityMs <= 0) continue;
        const qint64 idleMs = now - context->lastActivityMs;
        if (context->isControl) {
            const bool currentSession = context->paired
                && m_sessions.value(context->peerId).data() == socket;
            if (currentSession && socket->state() == QAbstractSocket::ConnectedState) {
                if (idleMs > SessionTimeoutMs) expired.append({socket, {}, true});
                else sendPacket(socket, MsgPing);
            } else if (idleMs > PairingDecisionTimeoutMs) {
                expired.append({socket, {}, true});
            }
            continue;
        }
        if (context->id.isEmpty()) {
            if (idleMs > IncomingHandshakeTimeoutMs)
                expired.append({socket, QStringLiteral("protocol-error"), false});
            continue;
        }
        const qint64 timeout = context->completionSent || context->verificationInProgress
            ? VerificationTimeoutMs : TransferIdleTimeoutMs;
        if (idleMs > timeout)
            expired.append({socket, QStringLiteral("paused"), true});
    }
    for (const ExpiredSocket &item : std::as_const(expired)) {
        if (!m_tasks.contains(item.socket)) continue;
        qWarning() << "Closing idle transfer socket" << item.socket
                   << "status" << item.status;
        cleanupSocket(item.socket, item.status, item.preservePartial);
    }
}

QString TransferManager::stateName(ConnectionState state)
{
    switch (state) {
    case ConnectionState::Unpaired: return QStringLiteral("unpaired");
    case ConnectionState::Pairing: return QStringLiteral("pairing");
    case ConnectionState::Online: return QStringLiteral("online");
    case ConnectionState::Reconnecting: return QStringLiteral("reconnecting");
    case ConnectionState::Offline: return QStringLiteral("offline");
    }
    return QStringLiteral("unpaired");
}

QString TransferManager::connectionState(const QString &peerId) const
{
    return stateName(m_connections.value(peerId).state);
}

QVariantList TransferManager::trustedPeers() const
{
    QStringList peerIds;
    for (auto it = m_connections.cbegin(); it != m_connections.cend(); ++it)
        if (it->trusted) peerIds.append(it.key());
    std::sort(peerIds.begin(), peerIds.end(), [this](const QString &left, const QString &right) {
        const int byName = QString::compare(m_connections.value(left).name,
                                            m_connections.value(right).name,
                                            Qt::CaseInsensitive);
        return byName == 0 ? left < right : byName < 0;
    });

    QVariantList result;
    result.reserve(peerIds.size());
    for (const QString &peerId : std::as_const(peerIds)) {
        const PeerConnection &peer = m_connections[peerId];
        result.append(QVariantMap{
            {QStringLiteral("id"), peerId},
            {QStringLiteral("name"), peer.name},
            {QStringLiteral("ip"), peer.ip},
            {QStringLiteral("port"), peer.port},
            {QStringLiteral("state"), stateName(peer.state)},
            {QStringLiteral("online"), peer.state == ConnectionState::Online},
            {QStringLiteral("lastSeen"), peer.lastSeen}
        });
    }
    return result;
}

void TransferManager::setConnectionState(const QString &peerId, ConnectionState state)
{
    if (peerId.isEmpty()) return;
    auto &peer = m_connections[peerId];
    if (peer.state == state) return;
    peer.state = state;
    qInfo() << "Peer connection state" << peerId << stateName(state);
    emit pairingStateChanged(peerId, stateName(state));
    const bool allowed = state == ConnectionState::Online
                         && peer.authKey.size() == AuthenticationKeySize;
    const QString mediaToken = allowed
        ? QString::fromLatin1(mediaAuthorizationKey(peer.authKey).toBase64())
        : QString{};
    emit peerAuthorizationChanged(peerId, peer.ip, mediaToken, allowed);
    if (peer.trusted) emit trustedPeersChanged();
    if (state == ConnectionState::Online) {
        for (auto it = m_pendingOutgoing.cbegin(); it != m_pendingOutgoing.cend(); ++it)
            if (it->transfer.peerId == peerId) m_transferRetryAttempts[it.key()] = 0;
        QTimer::singleShot(0, this, [this, peerId] { resumePendingTransfers(peerId); });
    }
}

void TransferManager::loadTrustedPeers()
{
    QSettings settings(m_settingsOrganization, m_settingsApplication);
    settings.beginGroup(QStringLiteral("trustedPeers"));
    for (const QString &peerId : settings.childGroups()) {
        settings.beginGroup(peerId);
        PeerConnection peer;
        peer.name = settings.value(QStringLiteral("name")).toString();
        peer.ip = settings.value(QStringLiteral("ip")).toString();
        peer.port = quint16(settings.value(QStringLiteral("port")).toUInt());
        peer.lastSeen = settings.value(QStringLiteral("lastSeen")).toLongLong();
        peer.authKey = QByteArray::fromBase64(
            settings.value(QStringLiteral("authKey")).toByteArray());
        if (peer.authKey.size() != AuthenticationKeySize) {
            qWarning() << "Ignoring legacy trusted peer without an authentication key" << peerId;
            settings.endGroup();
            continue;
        }
        peer.trusted = true;
        peer.state = ConnectionState::Offline;
        m_connections.insert(peerId, peer);
        settings.endGroup();
    }
    settings.endGroup();
}

bool TransferManager::isTrusted(const QString &peerId) const
{
    return m_connections.contains(peerId) && m_connections.value(peerId).trusted
        && m_connections.value(peerId).authKey.size() == AuthenticationKeySize;
}

void TransferManager::saveTrustedPeer(const QString &peerId, const QString &name,
                                      const QString &ip, quint16 port)
{
    auto &peer = m_connections[peerId];
    if (peer.authKey.size() != AuthenticationKeySize) {
        qWarning() << "Refusing to persist trusted peer without an authentication key" << peerId;
        return;
    }
    peer.trusted = true;
    peer.name = name;
    peer.ip = ip;
    if (port) peer.port = port;
    peer.lastSeen = QDateTime::currentMSecsSinceEpoch();
    QSettings settings(m_settingsOrganization, m_settingsApplication);
    settings.beginGroup(QStringLiteral("trustedPeers"));
    settings.beginGroup(peerId);
    settings.setValue(QStringLiteral("name"), peer.name);
    settings.setValue(QStringLiteral("ip"), peer.ip);
    settings.setValue(QStringLiteral("port"), peer.port);
    settings.setValue(QStringLiteral("lastSeen"), peer.lastSeen);
    settings.setValue(QStringLiteral("authKey"), peer.authKey.toBase64());
    settings.endGroup();
    settings.endGroup();
    settings.sync();
    emit trustedPeersChanged();
}

void TransferManager::removeTrustedPeer(const QString &peerId)
{
    if (m_connections.contains(peerId)) {
        m_connections[peerId].trusted = false;
        m_connections[peerId].authKey.fill('\0');
        m_connections[peerId].authKey.clear();
    }
    QSettings settings(m_settingsOrganization, m_settingsApplication);
    settings.beginGroup(QStringLiteral("trustedPeers"));
    settings.remove(peerId);
    settings.endGroup();
    settings.sync();
    emit trustedPeersChanged();
}

bool TransferManager::isPaired(const QString &peerId) const
{
    const auto socket = m_sessions.value(peerId);
    const auto *context = socket ? m_tasks.value(socket) : nullptr;
    return m_connections.value(peerId).state == ConnectionState::Online
        && socket && context && context->paired
        && socket->state() == QAbstractSocket::ConnectedState;
}

bool TransferManager::isPairedIp(const QString &ip) const
{
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        auto *socket = it.value().data();
        auto *context = socket ? m_tasks.value(socket) : nullptr;
        if (context && context->paired && context->peerIp == ip
            && socket->state() == QAbstractSocket::ConnectedState) return true;
    }
    return false;
}

QString TransferManager::peerIdForIp(const QString &ip) const
{
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        auto *socket = it.value().data();
        auto *context = socket ? m_tasks.value(socket) : nullptr;
        if (context && context->paired && context->peerIp == ip) return it.key();
    }
    return {};
}

void TransferManager::requestPairing(const QString &peerId, const QString &peerName,
                                     const QString &ip, quint16 port)
{
    if (!isValidPeerId(peerId) || ip.isEmpty() || port == 0) {
        emit transferError(tr("无法向该设备发起配对"));
        return;
    }
    if (isPaired(peerId)) {
        emit pairingStateChanged(peerId, stateName(ConnectionState::Online));
        return;
    }
    for (auto *item : std::as_const(m_tasks)) {
        if (item->isControl && item->peerId == peerId) {
            emit pairingStateChanged(peerId, connectionState(peerId));
            return;
        }
    }
    const bool reconnecting = isTrusted(peerId);
    auto &peer = m_connections[peerId];
    peer.name = peerName;
    peer.ip = ip;
    peer.port = port;
    openControlConnection(peerId, peerName, ip, port, reconnecting);
}

void TransferManager::openControlConnection(const QString &peerId, const QString &peerName,
                                            const QString &ip, quint16 port, bool reconnecting)
{
    QByteArray authKey = reconnecting ? m_connections.value(peerId).authKey
                                      : randomAuthenticationBytes(AuthenticationKeySize);
    if (authKey.size() != AuthenticationKeySize) {
        qWarning() << "Cannot authenticate control connection for" << peerId;
        setConnectionState(peerId, ConnectionState::Unpaired);
        emit transferError(tr("设备身份凭据无效，请解除配对后重新配对"));
        return;
    }
    auto *socket = new QTcpSocket(this);
    auto *context = new TransferContext;
    context->isControl = true;
    context->isSender = true;
    context->peerId = peerId;
    context->peerName = peerName;
    context->peerIp = ip;
    context->peerPort = port;
    context->authKey = authKey;
    context->authChallenge = randomAuthenticationBytes(AuthenticationChallengeSize);
    context->reconnecting = reconnecting;
    context->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    m_tasks[socket] = context;
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    connect(socket, &QTcpSocket::connected, this, [this, socket] {
        auto *item = m_tasks.value(socket);
        if (!item) return;
        QSettings settings(m_settingsOrganization, m_settingsApplication);
        const QString localName = settings.value(QStringLiteral("identity/deviceName"),
                                                  QHostInfo::localHostName()).toString();
        const QByteArray proof = controlRequestProof(item->authKey, m_localId,
                                                     item->peerId, item->reconnecting,
                                                     item->authChallenge);
        sendPacket(socket, MsgPairRequest, [this, item, localName, proof](QDataStream &out) {
            out << PairingProtocolVersion << m_localId << localName << serverPort()
                << item->reconnecting << item->authChallenge
                << (item->reconnecting ? QByteArray{} : item->authKey) << proof;
        });
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        if (m_tasks.contains(socket)) cleanupSocket(socket);
    });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, socket, peerId](QAbstractSocket::SocketError) {
        if (!m_tasks.contains(socket)) return;
        cleanupSocket(socket);
    });
    setConnectionState(peerId, reconnecting ? ConnectionState::Reconnecting
                                             : ConnectionState::Pairing);
    const QPointer<QTcpSocket> guardedSocket(socket);
    QTimer::singleShot(ConnectionTimeoutMs, socket, [this, guardedSocket] {
        if (!guardedSocket || !m_tasks.contains(guardedSocket)
            || guardedSocket->state() != QAbstractSocket::ConnectingState) return;
        qWarning() << "Control connection timed out" << guardedSocket->peerName();
        cleanupSocket(guardedSocket);
    });
    socket->connectToHost(ip, port);
}

void TransferManager::acceptPairing(const QString &peerId)
{
    for (auto it = m_tasks.cbegin(); it != m_tasks.cend(); ++it) {
        auto *item = it.value();
        if (!item->isControl || item->isSender || item->peerId != peerId || item->paired) continue;
        acceptPairingSocket(it.key());
        return;
    }
}

void TransferManager::acceptPairingSocket(QTcpSocket *socket)
{
    auto *item = m_tasks.value(socket);
    if (!item || !item->isControl || item->isSender || item->peerId.isEmpty() || item->paired)
        return;
    if (item->authKey.size() != AuthenticationKeySize
        || item->authChallenge.size() != AuthenticationChallengeSize) {
        cleanupSocket(socket, QStringLiteral("protocol-error"), false);
        return;
    }
    m_connections[item->peerId].authKey = item->authKey;
    const QByteArray responseChallenge = randomAuthenticationBytes(AuthenticationChallengeSize);
    const QByteArray proof = controlAcceptProof(item->authKey, m_localId, item->peerId,
                                                item->authChallenge, responseChallenge);
    item->paired = true;
    const bool adopted = establishSession(socket, item);
    QSettings settings(m_settingsOrganization, m_settingsApplication);
    const QString localName = settings.value(QStringLiteral("identity/deviceName"),
                                              QHostInfo::localHostName()).toString();
    sendPacket(socket, MsgPairAccept, [this, localName, responseChallenge, proof](QDataStream &out) {
        out << PairingProtocolVersion << m_localId << localName << serverPort()
            << responseChallenge << proof;
    });
    if (!adopted) {
        socket->flush();
        socket->disconnectFromHost();
    }
}

void TransferManager::rejectPairing(const QString &peerId)
{
    for (auto it = m_tasks.cbegin(); it != m_tasks.cend(); ++it) {
        auto *item = it.value();
        if (!item->isControl || item->isSender || item->peerId != peerId || item->paired) continue;
        sendPacket(it.key(), MsgPairReject);
        removeTrustedPeer(peerId);
        setConnectionState(peerId, ConnectionState::Unpaired);
        it.key()->flush();
        it.key()->disconnectFromHost();
        return;
    }
}

void TransferManager::forgetPeer(const QString &peerId)
{
    if (!isValidPeerId(peerId)) return;
    ++m_connections[peerId].reconnectGeneration;
    removeTrustedPeer(peerId);
    setConnectionState(peerId, ConnectionState::Unpaired);

    QList<QTcpSocket *> sockets;
    for (auto it = m_tasks.cbegin(); it != m_tasks.cend(); ++it)
        if (it.value() && it.value()->peerId == peerId) sockets.append(it.key());
    for (QTcpSocket *socket : std::as_const(sockets)) {
        const auto *context = m_tasks.value(socket);
        cleanupSocket(socket, context && !context->isControl
                                  ? QStringLiteral("cancelled") : QString{}, false);
    }

    const auto unfinished = m_transferStore.unfinished();
    for (const PersistedTransfer &transfer : unfinished) {
        if (transfer.peerId != peerId) continue;
        m_cancelledTransferIds.insert(transfer.id);
        m_pendingOutgoing.remove(transfer.id);
        m_activeTransferIds.remove(transfer.id);
        m_transferRetryAttempts.remove(transfer.id);
        m_checksumRetryAttempts.remove(transfer.id);
        if (!transfer.isSender && !transfer.partialPath.isEmpty())
            QFile::remove(transfer.partialPath);
        emit taskUpdated(transfer.id, transfer.progress, QStringLiteral("cancelled"),
                         transfer.checksum.toHex());
    }
    m_sessions.remove(peerId);
    m_connections.remove(peerId);
    emit trustedPeersChanged();
}

void TransferManager::deviceAvailable(const QString &peerId, const QString &peerName,
                                      const QString &ip, quint16 port)
{
    if (!isTrusted(peerId) || ip.isEmpty() || port == 0) return;
    auto &peer = m_connections[peerId];
    peer.name = peerName;
    peer.ip = ip;
    peer.port = port;
    saveTrustedPeer(peerId, peerName, ip, port);
    if (peer.state != ConnectionState::Offline) return;
    for (auto *context : std::as_const(m_tasks))
        if (context->isControl && context->peerId == peerId) return;
    peer.reconnectAttempt = 0;
    openControlConnection(peerId, peerName, ip, port, true);
}

void TransferManager::setSaveDirectory(const QString &value)
{
    const QUrl url(value);
#ifdef Q_OS_ANDROID
    if (url.scheme() == QStringLiteral("content")) {
        const QString uri = url.toString(QUrl::FullyEncoded);
        if (!persistAndroidDirectory(uri)) {
            emit transferError(tr("无法获得所选目录的长期读写权限"));
            return;
        }
        if (uri == m_saveDirectory) return;
        m_saveDirectory = uri;
        emit saveDirectoryChanged();
        return;
    }
#endif
    const QString path = url.isLocalFile() ? url.toLocalFile()
                                           : (url.scheme().isEmpty() ? value : QString{});
    if (path.isEmpty()) {
        emit transferError(tr("当前版本仅支持可直接写入的本地目录"));
        return;
    }
    if (QDir::cleanPath(path) == QDir::cleanPath(m_saveDirectory))
        return;
    if (!QDir().mkpath(path) || !QFileInfo(path).isDir() || !QFileInfo(path).isWritable()) {
        emit transferError(tr("接收目录不可写：%1").arg(path));
        return;
    }
    m_saveDirectory = QDir::cleanPath(path);
    emit saveDirectoryChanged();
}

void TransferManager::sendPacket(QTcpSocket *socket, MessageType type,
                                 const std::function<void(QDataStream &)> &write)
{
    if (!socket || socket->state() == QAbstractSocket::UnconnectedState)
        return;
    QByteArray packet;
    QDataStream out(&packet, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_5);
    out << quint32(0) << quint8(type);
    if (write)
        write(out);
    out.device()->seek(0);
    out << quint32(packet.size() - sizeof(quint32));
    socket->write(packet);
    if (auto *context = m_tasks.value(socket); context && !context->isControl)
        context->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
}

void TransferManager::sendFiles(const QList<QUrl> &urls, const QString &ip, quint16 port)
{
    if (ip.isEmpty() || port == 0) {
        emit transferError(tr("目标设备地址无效"));
        return;
    }
    if (!isPairedIp(ip)) {
        emit transferError(tr("请先与该设备配对"));
        return;
    }
    const QString peerId = peerIdForIp(ip);
    if (peerId.isEmpty()) {
        emit transferError(tr("无法识别目标设备"));
        return;
    }
    const PeerConnection peer = m_connections.value(peerId);
    for (const QUrl &url : urls) {
        const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
        QString name = url.fileName().isEmpty() ? QFileInfo(path).fileName() : url.fileName();
        if (name.isEmpty())
            name = QStringLiteral("file-%1.bin").arg(QDateTime::currentMSecsSinceEpoch());
        const qint64 knownSize = url.isLocalFile() ? QFileInfo(path).size() : 0;
        PersistedTransfer transfer;
        transfer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        transfer.peerId = peerId;
        transfer.peerName = peer.name;
        transfer.peerIp = ip;
        transfer.peerPort = port;
        transfer.isSender = true;
        transfer.sourcePath = path;
        transfer.fileName = name;
        transfer.totalBytes = qMax<qint64>(0, knownSize);
        transfer.status = QStringLiteral("verifying");
        transfer.createdAt = QDateTime::currentMSecsSinceEpoch();
        m_transferStore.upsert(transfer);
        emit taskAdded(transfer.id, ip, name, true, transfer.totalBytes);
        emit taskUpdated(transfer.id, 0, transfer.status, {});
        beginOutgoingTransfer(transfer, true);
    }
}

void TransferManager::restoreTransfers()
{
    if (m_transfersRestored) return;
    m_transfersRestored = true;
    m_transferStore.markRunningTasksPaused();
    const auto transfers = m_transferStore.unfinished();
    for (PersistedTransfer transfer : transfers) {
        transfer.status = QStringLiteral("paused");
        if (!transfer.isSender && !transfer.partialPath.isEmpty() && transfer.totalBytes > 0)
            transfer.progress = qBound<qreal>(0, qreal(QFileInfo(transfer.partialPath).size())
                                                 / transfer.totalBytes, 1);
        m_transferStore.upsert(transfer);
        emit taskRestored(transfer.id, transfer.peerId, transfer.peerName, transfer.peerIp,
                          transfer.fileName, transfer.isSender, transfer.totalBytes,
                          transfer.progress, transfer.status, transfer.checksum.toHex());
        if (transfer.isSender) enqueueOutgoing(transfer, true);
    }
    for (auto it = m_connections.cbegin(); it != m_connections.cend(); ++it)
        if (it->state == ConnectionState::Online) resumePendingTransfers(it.key());
}

void TransferManager::enqueueOutgoing(const PersistedTransfer &transfer, bool validateSource)
{
    m_activeTransferIds.remove(transfer.id);
    m_pendingOutgoing.insert(transfer.id, PendingOutgoing{transfer, validateSource});
}

void TransferManager::resumePendingTransfers(const QString &peerId)
{
    if (!m_transfersRestored && m_pendingOutgoing.isEmpty()) return;
    QStringList ids;
    for (auto it = m_pendingOutgoing.cbegin(); it != m_pendingOutgoing.cend(); ++it)
        if (it->transfer.peerId == peerId) ids.append(it.key());
    for (const QString &id : std::as_const(ids)) {
        const PendingOutgoing pending = m_pendingOutgoing.take(id);
        beginOutgoingTransfer(pending.transfer, pending.validateSource);
    }
}

void TransferManager::beginOutgoingTransfer(PersistedTransfer transfer, bool validateSource)
{
    if (m_activeTransferIds.contains(transfer.id)) return;
    if (!isPaired(transfer.peerId)) {
        transfer.status = QStringLiteral("paused");
        m_transferStore.upsert(transfer);
        enqueueOutgoing(transfer, validateSource);
        emit taskUpdated(transfer.id, transfer.progress, transfer.status, transfer.checksum.toHex());
        return;
    }
    m_activeTransferIds.insert(transfer.id);
    if (!validateSource && transfer.checksum.size() == 32 && transfer.totalBytes >= 0) {
        openOutgoingSocket(transfer);
        return;
    }
    emit taskUpdated(transfer.id, transfer.progress, QStringLiteral("verifying"), transfer.checksum.toHex());
    auto *watcher = new QFutureWatcher<DigestResult>(this);
    connect(watcher, &QFutureWatcher<DigestResult>::finished, this,
            [this, watcher, transfer, validateSource]() mutable {
        const DigestResult digest = watcher->result();
        watcher->deleteLater();
        if (m_cancelledTransferIds.remove(transfer.id)) return;
        if (!digest.error.isEmpty() || digest.size < 0
            || (validateSource && !transfer.checksum.isEmpty()
                && (transfer.checksum != digest.hash || transfer.totalBytes != digest.size))) {
            m_activeTransferIds.remove(transfer.id);
            emit taskUpdated(transfer.id, transfer.progress, QStringLiteral("read-error"), {});
            emit transferError(tr("无法恢复发送 %1：源文件不可用或已发生变化").arg(transfer.fileName));
            return;
        }
        transfer.totalBytes = digest.size;
        transfer.checksum = digest.hash;
        transfer.status = QStringLiteral("connecting");
        const auto peer = m_connections.value(transfer.peerId);
        transfer.peerName = peer.name;
        transfer.peerIp = peer.ip;
        transfer.peerPort = peer.port;
        m_transferStore.upsert(transfer);
        emit taskSizeResolved(transfer.id, digest.size);
        openOutgoingSocket(transfer);
    });
    watcher->setFuture(QtConcurrent::run(calculateDigest, transfer.sourcePath));
}

void TransferManager::openOutgoingSocket(PersistedTransfer transfer)
{
    if (!isPaired(transfer.peerId)) {
        transfer.status = QStringLiteral("paused");
        m_transferStore.upsert(transfer);
        enqueueOutgoing(transfer, false);
        emit taskUpdated(transfer.id, transfer.progress, transfer.status, transfer.checksum.toHex());
        return;
    }
    const PeerConnection peer = m_connections.value(transfer.peerId);
    auto *socket = new QTcpSocket(this);
    auto *context = new TransferContext;
    context->id = transfer.id;
    context->isSender = true;
    context->peerId = transfer.peerId;
    context->peerName = peer.name;
    context->peerIp = peer.ip;
    context->peerPort = peer.port;
    context->authKey = peer.authKey;
    context->authChallenge = randomAuthenticationBytes(AuthenticationChallengeSize);
    context->sourcePath = transfer.sourcePath;
    context->fileName = transfer.fileName;
    context->totalBytes = transfer.totalBytes;
    context->checksum = transfer.checksum;
    context->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    context->file = new QFile(transfer.sourcePath, this);
    m_tasks[socket] = context;
    if (!context->file->open(QIODevice::ReadOnly)) {
        m_activeTransferIds.remove(transfer.id);
        emit taskUpdated(transfer.id, transfer.progress, QStringLiteral("read-error"), {});
        cleanupSocket(socket);
        return;
    }
    emit taskUpdated(transfer.id, transfer.progress, QStringLiteral("connecting"), transfer.checksum.toHex());
    connect(socket, &QTcpSocket::connected, this, [this, socket, context] {
        const QByteArray proof = fileOfferProof(
            context->authKey, m_localId, context->peerId, context->authChallenge,
            context->id, context->fileName, context->totalBytes, context->checksum);
        sendPacket(socket, MsgFileOffer, [this, context, proof](QDataStream &out) {
            out << PairingProtocolVersion << m_localId << context->id << context->fileName
                << context->totalBytes << context->checksum
                << context->authChallenge << proof;
        });
        emit taskUpdated(context->id, 0, QStringLiteral("negotiating"), context->checksum.toHex());
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
    connect(socket, &QTcpSocket::bytesWritten, this, [this, socket](qint64) {
        if (auto *item = m_tasks.value(socket))
            item->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        sendNextChunk(socket);
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        if (auto *item = m_tasks.value(socket))
            cleanupSocket(socket, item->completionSent ? QString{} : QStringLiteral("paused"), true);
    });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, socket](QAbstractSocket::SocketError) {
        if (m_tasks.contains(socket)) cleanupSocket(socket, QStringLiteral("paused"), true);
    });
    const QPointer<QTcpSocket> guardedSocket(socket);
    QTimer::singleShot(ConnectionTimeoutMs, socket, [this, guardedSocket] {
        if (!guardedSocket || !m_tasks.contains(guardedSocket)
            || guardedSocket->state() != QAbstractSocket::ConnectingState) return;
        qWarning() << "File transfer connection timed out" << guardedSocket->peerName();
        cleanupSocket(guardedSocket, QStringLiteral("paused"), true);
    });
    socket->connectToHost(peer.ip, peer.port);
}

void TransferManager::sendNextChunk(QTcpSocket *socket)
{
    auto *context = m_tasks.value(socket);
    if (!context || !context->isSender || !context->offerAccepted || !context->file
        || !context->file->isOpen())
        return;
    while (socket->bytesToWrite() < 2 * 1024 * 1024 && !context->sourceEof) {
        const QByteArray data = context->file->read(256 * 1024);
        if (data.isEmpty() && context->file->error() != QFileDevice::NoError) {
            cleanupSocket(socket, QStringLiteral("read-error"), true);
            return;
        }
        if (data.isEmpty()) {
            context->sourceEof = true;
            break;
        }
        sendPacket(socket, MsgFileData, [context, data](QDataStream &out) { out << context->id << data; });
        const qreal progress = context->totalBytes ? qreal(context->file->pos()) / context->totalBytes : 1;
        const int percent = int(progress * 100);
        if (percent > context->lastProgressPct) {
            context->lastProgressPct = percent;
            emit taskUpdated(context->id, progress, QStringLiteral("transferring"), context->checksum.toHex());
        }
    }
    if (context->sourceEof && !context->completionSent && socket->bytesToWrite() == 0) {
        context->completionSent = true;
        sendPacket(socket, MsgComplete, [context](QDataStream &out) {
            out << context->id << true << context->checksum;
        });
        emit taskUpdated(context->id, 1, QStringLiteral("verifying"), context->checksum.toHex());
    }
}

void TransferManager::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        auto *socket = m_server->nextPendingConnection();
        auto *context = new TransferContext;
        context->peerIp = socket->peerAddress().toString().remove(QStringLiteral("::ffff:"));
        context->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        m_tasks[socket] = context;
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            if (auto *item = m_tasks.value(socket))
                cleanupSocket(socket, item->isControl || item->completionSent ? QString{} : QStringLiteral("paused"), true);
        });
        connect(socket, &QTcpSocket::errorOccurred, this,
                [this, socket](QAbstractSocket::SocketError) {
            if (m_tasks.contains(socket))
                cleanupSocket(socket, m_tasks.value(socket)->isControl ? QString{} : QStringLiteral("paused"), true);
        });
    }
}

void TransferManager::onReadyRead(QTcpSocket *socket)
{
    auto *context = m_tasks.value(socket);
    if (!context)
        return;
    QDataStream in(socket);
    in.setVersion(QDataStream::Qt_6_5);
    const auto fail = [this, socket] { cleanupSocket(socket, QStringLiteral("protocol-error"), false); };
    while (true) {
        if (!context->blockSize) {
            if (socket->bytesAvailable() < qint64(sizeof(quint32)))
                return;
            in >> context->blockSize;
            if (context->blockSize < 1 || context->blockSize > MaxPacketSize) {
                fail();
                return;
            }
        }
        if (socket->bytesAvailable() < context->blockSize)
            return;
        const QByteArray raw = socket->read(context->blockSize);
        context->blockSize = 0;
        context->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        QDataStream packet(raw);
        packet.setVersion(QDataStream::Qt_6_5);
        quint8 type = 0;
        packet >> type;
        if (packet.status() != QDataStream::Ok) {
            fail();
            return;
        }
        if (type == MsgPairRequest) {
            quint16 protocolVersion = 0;
            QString peerId, peerName;
            quint16 peerPort = 0;
            bool reconnecting = false;
            QByteArray challenge, proposedKey, proof;
            packet >> protocolVersion >> peerId >> peerName >> peerPort >> reconnecting
                   >> challenge >> proposedKey >> proof;
            if (packet.status() != QDataStream::Ok || context->isControl || context->file
                || !context->id.isEmpty() || !isValidPeerId(peerId) || peerId == m_localId
                || peerName.trimmed().isEmpty() || peerName.size() > 80
                || protocolVersion != PairingProtocolVersion || !packet.atEnd()
                || challenge.size() != AuthenticationChallengeSize
                || proof.size() != AuthenticationProofSize) {
                fail(); return;
            }
            const bool trusted = isTrusted(peerId);
            if (reconnecting != trusted
                || (!reconnecting && proposedKey.size() != AuthenticationKeySize)
                || (reconnecting && !proposedKey.isEmpty())) {
                qWarning() << "Rejected pairing mode mismatch for" << peerId
                           << "reconnecting" << reconnecting << "trusted" << trusted;
                fail(); return;
            }
            const QByteArray authKey = reconnecting
                ? m_connections.value(peerId).authKey : proposedKey;
            const QByteArray expectedProof = controlRequestProof(
                authKey, peerId, m_localId, reconnecting, challenge);
            if (!constantTimeEqual(proof, expectedProof)) {
                qWarning() << "Rejected invalid control authentication proof for" << peerId;
                fail(); return;
            }
            context->isControl = true;
            context->peerId = peerId;
            context->peerName = peerName.trimmed();
            context->peerPort = peerPort;
            context->authKey = authKey;
            context->authChallenge = challenge;
            context->reconnecting = reconnecting;
            socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
            qInfo() << "Incoming pairing request from" << context->peerName << context->peerIp
                    << "peerId" << peerId;
            if (reconnecting) {
                auto &peer = m_connections[peerId];
                peer.name = context->peerName;
                peer.ip = context->peerIp;
                if (peerPort) peer.port = peerPort;
                if (!isPaired(peerId))
                    setConnectionState(peerId, ConnectionState::Reconnecting);
                acceptPairingSocket(socket);
            } else {
                setConnectionState(peerId, ConnectionState::Pairing);
                emit pairingRequested(peerId, context->peerName, context->peerIp);
            }
        } else if (type == MsgPairAccept) {
            quint16 protocolVersion = 0;
            QString peerId, peerName;
            quint16 peerPort = 0;
            QByteArray responseChallenge, proof;
            packet >> protocolVersion >> peerId >> peerName >> peerPort
                   >> responseChallenge >> proof;
            if (packet.status() != QDataStream::Ok || !context->isControl || !context->isSender
                || peerId != context->peerId || peerName.trimmed().isEmpty()
                || peerName.size() > 80 || protocolVersion != PairingProtocolVersion
                || responseChallenge.size() != AuthenticationChallengeSize
                || proof.size() != AuthenticationProofSize || !packet.atEnd()) {
                fail(); return;
            }
            const QByteArray expectedProof = controlAcceptProof(
                context->authKey, peerId, m_localId,
                context->authChallenge, responseChallenge);
            if (!constantTimeEqual(proof, expectedProof)) {
                qWarning() << "Rejected invalid pairing acceptance proof for" << peerId;
                fail(); return;
            }
            context->peerName = peerName.trimmed();
            if (peerPort) context->peerPort = peerPort;
            m_connections[peerId].authKey = context->authKey;
            context->paired = true;
            if (!establishSession(socket, context)) {
                cleanupSocket(socket);
                return;
            }
        } else if (type == MsgPairReject) {
            if (!context->isControl || !context->isSender) { fail(); return; }
            removeTrustedPeer(context->peerId);
            setConnectionState(context->peerId, ConnectionState::Unpaired);
            cleanupSocket(socket);
            return;
        } else if (type == MsgPing) {
            if (!context->isControl || !context->paired) { fail(); return; }
            sendPacket(socket, MsgPong);
        } else if (type == MsgPong) {
            if (!context->isControl || !context->paired) { fail(); return; }
        } else if (type == MsgText) {
            QString text;
            packet >> text;
            if (packet.status() != QDataStream::Ok || text.size() > MaxTextLength
                || !context->isControl || !context->paired) {
                fail(); return;
            }
            emit textReceived(context->peerIp, text);
        } else if (type == MsgFileOffer) {
            if (context->isControl || context->isSender || context->file
                || !context->id.isEmpty()) {
                fail(); return;
            }
            quint16 protocolVersion = 0;
            QString claimedPeerId;
            QByteArray challenge, proof;
            packet >> protocolVersion >> claimedPeerId >> context->id >> context->fileName
                   >> context->totalBytes >> context->checksum >> challenge >> proof;
            const auto controlSocket = m_sessions.value(claimedPeerId);
            const auto *controlContext = controlSocket ? m_tasks.value(controlSocket) : nullptr;
            const PeerConnection peer = m_connections.value(claimedPeerId);
            const QByteArray expectedProof = fileOfferProof(
                peer.authKey, claimedPeerId, m_localId, challenge, context->id,
                context->fileName, context->totalBytes, context->checksum);
            if (packet.status() != QDataStream::Ok || protocolVersion != PairingProtocolVersion
                || !packet.atEnd() || !isValidPeerId(claimedPeerId) || !isPaired(claimedPeerId)
                || !controlContext || controlContext->peerIp != context->peerIp
                || peer.authKey.size() != AuthenticationKeySize
                || challenge.size() != AuthenticationChallengeSize
                || proof.size() != AuthenticationProofSize
                || !constantTimeEqual(proof, expectedProof)
                || context->id.isEmpty() || context->fileName.isEmpty()
                || context->fileName.size() > 255 || context->totalBytes < 0
                || context->checksum.size() != 32
                || QFileInfo(context->fileName).fileName() != context->fileName) {
                qWarning() << "Rejected unauthenticated or invalid file offer from"
                           << context->peerIp << "claimed peer" << claimedPeerId;
                fail(); return;
            }
            context->peerId = claimedPeerId;
            context->authKey = peer.authKey;
            context->authChallenge = challenge;
            context->peerName = peer.name;
            context->peerPort = peer.port;
            const auto saved = m_transferStore.find(context->id);
            if (saved) {
                if (saved->isSender || saved->peerId != context->peerId
                    || saved->fileName != context->fileName || saved->totalBytes != context->totalBytes
                    || saved->checksum != context->checksum) {
                    fail(); return;
                }
                if (saved->status == QStringLiteral("verified")) {
                    emit taskAdded(context->id, context->peerIp, context->fileName, false,
                                   context->totalBytes);
                    emit taskUpdated(context->id, 1, QStringLiteral("verified"),
                                     context->checksum.toHex());
                    context->completionSent = true;
                    sendPacket(socket, MsgComplete, [context](QDataStream &out) {
                        out << context->id << true << context->checksum;
                    });
                    socket->flush();
                    socket->disconnectFromHost();
                    return;
                }
                context->partialPath = saved->partialPath;
                context->finalPath = saved->finalPath;
                if (context->partialPath.isEmpty() || (!QFileInfo::exists(context->partialPath)
                    && QFileInfo::exists(context->finalPath))) {
                    const QString receivePath = storagePath();
                    context->partialPath = QDir(receivePath).filePath(
                        QStringLiteral(".%1.%2.part").arg(context->fileName,
                                                           QString(context->checksum.toHex().left(12))));
                    context->finalPath = availableFinalPath(context->fileName);
                }
            } else {
                const QString receivePath = storagePath();
                QDir().mkpath(receivePath);
                context->partialPath = QDir(receivePath).filePath(
                    QStringLiteral(".%1.%2.part").arg(context->fileName,
                                                       QString(context->checksum.toHex().left(12))));
                context->finalPath = availableFinalPath(context->fileName);
            }
            const QString receivePath = storagePath();
            QDir().mkpath(receivePath);
            const qint64 existingBytes = QFileInfo(context->partialPath).size();
            if (!hasStorageCapacity(receivePath, context->totalBytes, existingBytes)) {
                qWarning() << "Rejecting transfer because storage is insufficient"
                           << context->id << "bytes" << context->totalBytes;
                emit taskAdded(context->id, context->peerIp, context->fileName, false,
                               context->totalBytes);
                emit taskUpdated(context->id, 0, QStringLiteral("no-space"),
                                 context->checksum.toHex());
                context->completionSent = true;
                sendPacket(socket, MsgComplete, [context](QDataStream &out) {
                    out << context->id << false << context->checksum;
                });
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            context->file = new QFile(context->partialPath, this);
            if (!context->file->open(QIODevice::ReadWrite | QIODevice::Append)) {
                emit taskAdded(context->id, context->peerIp, context->fileName, false, context->totalBytes);
                emit taskUpdated(context->id, 0, QStringLiteral("write-error"), {});
                cleanupSocket(socket, QStringLiteral("write-error"), false);
                return;
            }
            if (context->file->size() > context->totalBytes)
                context->file->resize(0);
            context->resumeOffset = context->file->size();
            PersistedTransfer received;
            received.id = context->id;
            received.peerId = context->peerId;
            received.peerName = context->peerName;
            received.peerIp = context->peerIp;
            received.peerPort = context->peerPort;
            received.isSender = false;
            received.fileName = context->fileName;
            received.totalBytes = context->totalBytes;
            received.checksum = context->checksum;
            received.partialPath = context->partialPath;
            received.finalPath = context->finalPath;
            received.progress = context->totalBytes
                ? qreal(context->resumeOffset) / context->totalBytes : 0;
            received.status = context->resumeOffset ? QStringLiteral("resuming")
                                                    : QStringLiteral("receiving");
            received.createdAt = saved ? saved->createdAt : QDateTime::currentMSecsSinceEpoch();
            m_transferStore.upsert(received);
            emit taskAdded(context->id, context->peerIp, context->fileName, false, context->totalBytes);
            emit taskUpdated(context->id,
                             context->totalBytes ? qreal(context->resumeOffset) / context->totalBytes : 0,
                             context->resumeOffset ? QStringLiteral("resuming") : QStringLiteral("receiving"),
                             context->checksum.toHex());
            sendPacket(socket, MsgResume, [context](QDataStream &out) {
                out << context->id << context->resumeOffset;
            });
        } else if (type == MsgResume) {
            QString id;
            qint64 offset = -1;
            packet >> id >> offset;
            if (packet.status() != QDataStream::Ok || context->isControl || !context->isSender
                || context->offerAccepted || id != context->id
                || offset < 0 || offset > context->totalBytes || !context->file
                || !context->file->seek(offset)) {
                fail(); return;
            }
            context->resumeOffset = offset;
            context->offerAccepted = true;
            m_transferRetryAttempts[context->id] = 0;
            emit taskUpdated(context->id, context->totalBytes ? qreal(offset) / context->totalBytes : 0,
                             offset ? QStringLiteral("resuming") : QStringLiteral("transferring"),
                             context->checksum.toHex());
            sendNextChunk(socket);
        } else if (type == MsgFileData) {
            QString id;
            QByteArray data;
            packet >> id >> data;
            if (packet.status() != QDataStream::Ok || context->isSender || id != context->id
                || !context->file || data.size() > MaxDataChunkSize
                || context->file->pos() + data.size() > context->totalBytes) {
                fail(); return;
            }
            if (context->file->write(data) != data.size()) {
                cleanupSocket(socket, QStringLiteral("write-error"), true);
                return;
            }
            const qreal progress = context->totalBytes ? qreal(context->file->pos()) / context->totalBytes : 1;
            const int percent = int(progress * 100);
            if (percent > context->lastProgressPct) {
                context->lastProgressPct = percent;
                emit taskUpdated(context->id, progress, QStringLiteral("receiving"), context->checksum.toHex());
            }
        } else if (type == MsgComplete) {
            QString id;
            bool ok = false;
            QByteArray hash;
            packet >> id >> ok >> hash;
            if (packet.status() != QDataStream::Ok || id != context->id || hash.size() != 32) {
                fail(); return;
            }
            if (context->isSender) {
                const bool verified = TransferProtocol::isCompletionVerified(
                    ok, hash, context->checksum);
                const bool checksumMismatch = !ok && hash != context->checksum;
                const bool peerRejected = !ok && hash == context->checksum;
                const auto persisted = checksumMismatch ? m_transferStore.find(context->id)
                                                        : std::nullopt;
                if (!verified) {
                    qWarning() << "Transfer completion rejected or checksum mismatch"
                               << context->id << "peerAccepted" << ok;
                }
                emit taskUpdated(context->id, verified ? 1 : 0,
                                  verified ? QStringLiteral("verified")
                                           : peerRejected ? QStringLiteral("rejected")
                                           : QStringLiteral("checksum-error"),
                                 context->checksum.toHex());
                const QString completedId = context->id;
                cleanupSocket(socket);
                if (verified) {
                    m_checksumRetryAttempts.remove(completedId);
                } else if (checksumMismatch && persisted && persisted->isSender
                           && m_checksumRetryAttempts.value(completedId) < MaxChecksumRetries) {
                    m_checksumRetryAttempts[completedId] += 1;
                    PersistedTransfer retry = *persisted;
                    retry.progress = 0;
                    retry.status = QStringLiteral("retrying");
                    m_transferStore.upsert(retry);
                    enqueueOutgoing(retry, true);
                    emit taskUpdated(completedId, 0, retry.status, retry.checksum.toHex());
                    QTimer::singleShot(m_reconnectBaseDelayMs, this, [this, completedId] {
                        auto it = m_pendingOutgoing.find(completedId);
                        if (it == m_pendingOutgoing.end() || !isPaired(it->transfer.peerId)) return;
                        const PendingOutgoing pending = *it;
                        m_pendingOutgoing.erase(it);
                        beginOutgoingTransfer(pending.transfer, pending.validateSource);
                    });
                }
                return;
            }
            finishReceive(socket);
            return;
        } else {
            fail(); return;
        }
        if (!socket->bytesAvailable())
            return;
    }
}

void TransferManager::finishReceive(QTcpSocket *socket)
{
    auto *context = m_tasks.value(socket);
    if (!context || !context->file)
        return;
    context->file->flush();
    context->file->close();
    context->verificationInProgress = true;
    context->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    emit taskUpdated(context->id,
                     context->totalBytes ? qreal(QFileInfo(context->partialPath).size()) / context->totalBytes : 1,
                     QStringLiteral("verifying"), context->checksum.toHex());

    const QString id = context->id;
    const QString partial = context->partialPath;
    const QString final = context->finalPath;
    const QByteArray expectedHash = context->checksum;
    const qint64 expectedSize = context->totalBytes;
    const QPointer<QTcpSocket> guarded(socket);
    auto *watcher = new QFutureWatcher<DigestResult>(this);
    connect(watcher, &QFutureWatcher<DigestResult>::finished, this,
            [this, watcher, guarded, id, partial, final, expectedHash, expectedSize] {
        const DigestResult digest = watcher->result();
        watcher->deleteLater();
        if (!guarded)
            return;
        auto *item = m_tasks.value(guarded);
        if (!item || item->id != id)
            return;
        item->verificationInProgress = false;
        item->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        bool ok = digest.error.isEmpty() && digest.size == expectedSize && digest.hash == expectedHash;
        QString status = ok ? QStringLiteral("verified") : QStringLiteral("checksum-error");
        if (!ok && status == QStringLiteral("checksum-error"))
            QFile::remove(partial);
        if (ok && !QFile::rename(partial, final)) {
            ok = false;
            status = QStringLiteral("write-error");
            emit transferError(tr("文件校验成功，但无法保存到：%1").arg(final));
        }
#ifdef Q_OS_ANDROID
        const QString destination = m_saveDirectory;
        if (ok && QUrl(destination).scheme() == QStringLiteral("content")) {
            const QString fileName = QFileInfo(final).fileName();
            auto *copyWatcher = new QFutureWatcher<bool>(this);
            connect(copyWatcher, &QFutureWatcher<bool>::finished, this,
                    [this, copyWatcher, guarded, id, final, digest] {
                const bool copied = copyWatcher->result();
                copyWatcher->deleteLater();
                if (!guarded) return;
                auto *active = m_tasks.value(guarded);
                if (!active || active->id != id) return;
                if (copied) QFile::remove(final);
                else emit transferError(tr("文件校验成功，但无法写入所选接收目录"));
                emit taskUpdated(id, copied ? 1 : 0,
                                 copied ? QStringLiteral("verified") : QStringLiteral("write-error"),
                                 digest.hash.toHex());
                active->completionSent = true;
                sendPacket(guarded, MsgComplete, [id, copied, digest](QDataStream &out) {
                    out << id << copied << digest.hash;
                });
                guarded->flush();
                guarded->disconnectFromHost();
            });
            copyWatcher->setFuture(QtConcurrent::run(
                [destination, final, fileName] {
                    return copyToAndroidDirectory(destination, final, fileName);
                }));
            return;
        }
#endif
        const qreal progress = ok ? 1 : qreal(qMax<qint64>(0, digest.size)) / qMax<qint64>(1, expectedSize);
        emit taskUpdated(id, progress, status, digest.hash.toHex());
        item->completionSent = true;
        sendPacket(guarded, MsgComplete, [id, ok, digest](QDataStream &out) {
            out << id << ok << digest.hash;
        });
        guarded->flush();
        guarded->disconnectFromHost();
    });
    watcher->setFuture(QtConcurrent::run(calculateDigest, partial));
}

void TransferManager::sendText(const QString &text, const QString &ip, quint16 port)
{
    if (text.trimmed().isEmpty() || text.size() > MaxTextLength || ip.isEmpty() || port == 0)
        return;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        auto *socket = it.value().data();
        auto *context = socket ? m_tasks.value(socket) : nullptr;
        if (context && context->paired && context->peerIp == ip) {
            sendPacket(socket, MsgText, [text](QDataStream &out) { out << text; });
            return;
        }
    }
    emit transferError(tr("请先与该设备配对"));
}

QString TransferManager::availableFinalPath(const QString &name) const
{
    const QString receivePath = storagePath();
    QString path = QDir(receivePath).filePath(name);
    const QFileInfo info(path);
    int number = 1;
    while (QFileInfo::exists(path)) {
        path = QDir(receivePath).filePath(info.suffix().isEmpty()
            ? QStringLiteral("%1 (%2)").arg(info.completeBaseName()).arg(number++)
            : QStringLiteral("%1 (%2).%3").arg(info.completeBaseName()).arg(number++).arg(info.suffix()));
    }
    return path;
}

QString TransferManager::storagePath() const
{
#ifdef Q_OS_ANDROID
    if (QUrl(m_saveDirectory).scheme() == QStringLiteral("content")) {
        const QString staging = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                    .filePath(QStringLiteral("Received"));
        QDir().mkpath(staging);
        return staging;
    }
#endif
    return m_saveDirectory;
}

bool TransferManager::establishSession(QTcpSocket *socket, TransferContext *context)
{
    if (!socket || !context || context->peerId.isEmpty()) return false;
    auto *previous = m_sessions.value(context->peerId).data();
    auto *previousContext = previous ? m_tasks.value(previous) : nullptr;
    const bool preferOutgoing = m_localId < context->peerId;
    const bool newPreferred = context->isSender == preferOutgoing;
    const bool previousPreferred = previousContext
        && previousContext->isSender == preferOutgoing;
    if (previous && previous != socket && previousContext
        && previous->state() == QAbstractSocket::ConnectedState
        && previousPreferred && !newPreferred) {
        context->paired = true;
        context->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        qInfo() << "Ignoring duplicate control session for" << context->peerId
                << "direction" << (context->isSender ? "outgoing" : "incoming")
                << "preferred direction" << (preferOutgoing ? "outgoing" : "incoming");
        return false;
    }
    m_sessions[context->peerId] = socket;
    context->paired = true;
    context->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    saveTrustedPeer(context->peerId, context->peerName, context->peerIp, context->peerPort);
    auto &peer = m_connections[context->peerId];
    peer.reconnectAttempt = 0;
    ++peer.reconnectGeneration;
    setConnectionState(context->peerId, ConnectionState::Online);
    qInfo() << "Adopted control session for" << context->peerId
            << "direction" << (context->isSender ? "outgoing" : "incoming")
            << "preferred" << newPreferred;
    if (previous && previous != socket) cleanupSocket(previous);
    return true;
}

void TransferManager::scheduleReconnect(const QString &peerId)
{
    if (!isTrusted(peerId) || peerId.isEmpty()) return;
    auto &peer = m_connections[peerId];
    if (peer.ip.isEmpty() || peer.port == 0 || peer.reconnectAttempt >= m_maxReconnectAttempts) {
        setConnectionState(peerId, ConnectionState::Offline);
        return;
    }
    setConnectionState(peerId, ConnectionState::Reconnecting);
    const int attempt = ++peer.reconnectAttempt;
    const quint64 generation = ++peer.reconnectGeneration;
    const int delayMs = qMin(30000, m_reconnectBaseDelayMs * (1 << qMin(attempt - 1, 5)));
    QTimer::singleShot(delayMs, this, [this, peerId, generation] {
        auto it = m_connections.find(peerId);
        if (it == m_connections.end() || it->reconnectGeneration != generation
            || it->state != ConnectionState::Reconnecting || isPaired(peerId)) return;
        const PeerConnection peer = *it;
        openControlConnection(peerId, peer.name, peer.ip, peer.port, true);
    });
}

void TransferManager::cleanupSocket(QTcpSocket *socket, const QString &status, bool preservePartial)
{
    if (!socket) return;
    auto *context = m_tasks.take(socket);
    if (context) {
        const QString peerId = context->peerId;
        const bool wasPaired = context->paired;
        qreal progress = 0;
        if (context->file && context->totalBytes)
            progress = qreal(context->file->pos()) / context->totalBytes;
        if (context->file) {
            context->file->close();
            if (!preservePartial && !context->partialPath.isEmpty())
                QFile::remove(context->partialPath);
            context->file->deleteLater();
        }
        if (!status.isEmpty() && !context->id.isEmpty())
            emit taskUpdated(context->id, progress, status, context->checksum.toHex());
        if (!context->isControl && context->isSender && !context->id.isEmpty()) {
            m_activeTransferIds.remove(context->id);
            if (status == QStringLiteral("paused")) {
                const auto transfer = m_transferStore.find(context->id);
                if (transfer) {
                    enqueueOutgoing(*transfer, false);
                    const int attempt = ++m_transferRetryAttempts[context->id];
                    if (attempt <= m_maxReconnectAttempts && isPaired(transfer->peerId)) {
                        const QString transferId = context->id;
                        const int delayMs = qMin(30000, m_reconnectBaseDelayMs * (1 << qMin(attempt - 1, 5)));
                        QTimer::singleShot(delayMs, this, [this, transferId] {
                            auto it = m_pendingOutgoing.find(transferId);
                            if (it == m_pendingOutgoing.end() || !isPaired(it->transfer.peerId)) return;
                            const PendingOutgoing pending = *it;
                            m_pendingOutgoing.erase(it);
                            beginOutgoingTransfer(pending.transfer, pending.validateSource);
                        });
                    }
                }
            }
            else m_transferRetryAttempts.remove(context->id);
        }
        if (!peerId.isEmpty()) {
            const bool currentSession = m_sessions.value(peerId) == socket;
            if (currentSession) m_sessions.remove(peerId);
            if (currentSession && wasPaired)
                scheduleReconnect(peerId);
            else if (!wasPaired && m_connections.value(peerId).state == ConnectionState::Pairing)
                setConnectionState(peerId, ConnectionState::Unpaired);
            else if (!wasPaired && m_connections.value(peerId).state == ConnectionState::Reconnecting)
                scheduleReconnect(peerId);
        }
        context->authKey.fill('\0');
        context->authKey.clear();
        delete context;
    }
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
}

void TransferManager::cancelTransfer(const QString &id)
{
    for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it) {
        if (it.value()->id == id) {
            cleanupSocket(it.key(), QStringLiteral("cancelled"), false);
            return;
        }
    }
    if (m_pendingOutgoing.remove(id) > 0) {
        m_transferRetryAttempts.remove(id);
        m_checksumRetryAttempts.remove(id);
        emit taskUpdated(id, 0, QStringLiteral("cancelled"), {});
        return;
    }
    const auto transfer = m_transferStore.find(id);
    if (transfer) {
        if (transfer->isSender) {
            m_cancelledTransferIds.insert(id);
            m_activeTransferIds.remove(id);
            m_checksumRetryAttempts.remove(id);
        } else if (!transfer->partialPath.isEmpty()) {
            QFile::remove(transfer->partialPath);
        }
        emit taskUpdated(id, transfer->progress, QStringLiteral("cancelled"),
                         transfer->checksum.toHex());
    }
}

void TransferManager::openFolder()
{
#ifdef Q_OS_ANDROID
    const auto context = QNativeInterface::QAndroidApplication::context();
    if (context.isValid() && QUrl(m_saveDirectory).scheme() == QStringLiteral("content")) {
        const QJniObject directory = QJniObject::fromString(m_saveDirectory);
        QJniObject::callStaticMethod<void>("org/landrop/app/StorageBridge", "openDirectory",
                                           "(Landroid/content/Context;Ljava/lang/String;)V",
                                           context.object(), directory.object());
    } else if (context.isValid()) {
        QJniObject::callStaticMethod<void>("org/landrop/app/LanTransferService", "openDownloads",
                                           "(Landroid/content/Context;)V", context.object());
    }
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_saveDirectory));
#endif
}
