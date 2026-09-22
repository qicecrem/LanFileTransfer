#include "transfermanager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QStringList>

#include <functional>
#include <cstdio>
#include <limits>

namespace {

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return condition();
}

bool waitUntilStable(const std::function<bool()> &condition, int stableMs,
                     int timeoutMs = 10000)
{
    QElapsedTimer total;
    QElapsedTimer stable;
    total.start();
    while (total.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (condition()) {
            if (!stable.isValid()) stable.start();
            if (stable.elapsed() >= stableMs) return true;
        } else {
            stable.invalidate();
        }
        QThread::msleep(1);
    }
    return false;
}

bool require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::fflush(stderr);
    }
    return condition;
}

bool createSourceFile(const QString &path, qint64 size)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QByteArray chunk(256 * 1024, Qt::Uninitialized);
    for (qsizetype i = 0; i < chunk.size(); ++i)
        chunk[i] = char((i * 31 + 17) & 0xff);
    qint64 written = 0;
    while (written < size) {
        const qsizetype count = qsizetype(qMin<qint64>(chunk.size(), size - written));
        if (file.write(chunk.constData(), count) != count) return false;
        written += count;
    }
    return file.flush();
}

QByteArray digest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result();
}

TransferManagerOptions options(const QString &root, const QString &id,
                               const QString &settingsName)
{
    TransferManagerOptions result;
    result.localId = id;
    result.settingsOrganization = QStringLiteral("LanDropIntegrationTest");
    result.settingsApplication = settingsName;
    result.transferDatabasePath = root + QLatin1Char('/') + settingsName + QStringLiteral(".sqlite3");
    result.saveDirectory = root + QLatin1Char('/') + settingsName + QStringLiteral("-received");
    result.reconnectBaseDelayMs = 25;
    return result;
}

}

class TransferManagerIntegrationTest {
public:
    static void sendAuthenticatedFileOffer(TransferManager &sender, QTcpSocket *socket,
                                           const QString &recipientId,
                                           const QString &transferId,
                                           const QString &fileName,
                                           qint64 totalBytes,
                                           const QByteArray &checksum)
    {
        const QByteArray key = sender.m_connections.value(recipientId).authKey;
        const QByteArray challenge = QCryptographicHash::hash(
            transferId.toUtf8(), QCryptographicHash::Sha256);
        const QByteArray proof = TransferManager::fileOfferProof(
            key, sender.m_localId, recipientId, challenge,
            transferId, fileName, totalBytes, checksum);
        sender.sendPacket(socket, TransferManager::MsgFileOffer,
                          [&](QDataStream &out) {
            out << quint16(2) << sender.m_localId << transferId << fileName
                << totalBytes << checksum << challenge << proof;
        });
    }

    static QString sessions(const TransferManager &manager)
    {
        QStringList result;
        for (auto it = manager.m_tasks.cbegin(); it != manager.m_tasks.cend(); ++it) {
            const auto *context = it.value();
            if (!context || !context->isControl) continue;
            const bool current = manager.m_sessions.value(context->peerId).data() == it.key();
            result.append(QStringLiteral("%1:%2:%3:%4->%5")
                              .arg(current ? QStringLiteral("current") : QStringLiteral("other"),
                                   context->isSender ? QStringLiteral("out") : QStringLiteral("in"),
                                   context->paired ? QStringLiteral("paired") : QStringLiteral("pending"))
                              .arg(it.key()->localPort())
                              .arg(it.key()->peerPort()));
        }
        return result.join(QLatin1Char(','));
    }

    static bool closeControlSession(TransferManager &manager, const QString &peerId)
    {
        auto *socket = manager.m_sessions.value(peerId).data();
        if (!socket) return false;
        manager.cleanupSocket(socket);
        return true;
    }

    static bool interruptIncomingTransfer(TransferManager &manager)
    {
        for (auto it = manager.m_tasks.cbegin(); it != manager.m_tasks.cend(); ++it) {
            const auto *context = it.value();
            if (context && !context->isControl && !context->isSender) {
                manager.cleanupSocket(it.key(), QStringLiteral("paused"), true);
                return true;
            }
        }
        return false;
    }

    static bool corruptPartialTransfer(TransferManager &manager, const QString &transferId)
    {
        const auto saved = manager.m_transferStore.find(transferId);
        if (!saved || saved->partialPath.isEmpty()) return false;
        QFile partial(saved->partialPath);
        if (!partial.open(QIODevice::ReadWrite) || partial.size() < 1) return false;
        if (!partial.seek(0) || partial.write("X", 1) != 1) return false;
        return partial.flush();
    }

    static bool transferInactive(const TransferManager &manager, const QString &transferId)
    {
        return !manager.m_activeTransferIds.contains(transferId)
            && !manager.m_pendingOutgoing.contains(transferId);
    }

    static bool expireIncomingTransfer(TransferManager &manager, const QString &transferId)
    {
        QTcpSocket *expired = nullptr;
        for (auto it = manager.m_tasks.cbegin(); it != manager.m_tasks.cend(); ++it) {
            TransferContext *context = it.value();
            if (!context || context->isControl || context->isSender
                || context->id != transferId) continue;
            context->lastActivityMs = QDateTime::currentMSecsSinceEpoch() - 20 * 60 * 1000;
            expired = it.key();
            break;
        }
        if (!expired) return false;
        manager.sweepSockets();
        return !manager.m_tasks.contains(expired);
    }

    static int run()
    {
        QTemporaryDir temporary;
        if (!require(temporary.isValid(), "temporary directory unavailable")) return 1;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           temporary.path() + QStringLiteral("/settings"));

        {
            QSettings legacy(QStringLiteral("LanDropIntegrationTest"), QStringLiteral("legacy"));
            legacy.beginGroup(QStringLiteral("trustedPeers/peer-old"));
            legacy.setValue(QStringLiteral("name"), QStringLiteral("Old peer"));
            legacy.setValue(QStringLiteral("ip"), QStringLiteral("127.0.0.1"));
            legacy.setValue(QStringLiteral("port"), 45455);
            legacy.endGroup();
            legacy.sync();
            TransferManager migrated(options(temporary.path(), QStringLiteral("peer-migrated"),
                                               QStringLiteral("legacy")));
            if (!require(migrated.trustedPeers().isEmpty(),
                         "legacy unauthenticated trust record was not invalidated")) return 47;
        }

        TransferManager left(options(temporary.path(), QStringLiteral("peer-a"),
                                     QStringLiteral("left")));
        TransferManager right(options(temporary.path(), QStringLiteral("peer-b"),
                                      QStringLiteral("right")));
        const QString localhost = QStringLiteral("127.0.0.1");
        QStringList leftStates;
        QStringList rightStates;
        QString receivedText;
        QString leftMediaToken;
        QString rightMediaToken;
        QObject::connect(&left, &TransferManager::pairingStateChanged, &left,
                         [&](const QString &peerId, const QString &state) {
            if (peerId == QStringLiteral("peer-b")) leftStates.append(state);
        });
        QObject::connect(&right, &TransferManager::pairingStateChanged, &right,
                         [&](const QString &peerId, const QString &state) {
            if (peerId == QStringLiteral("peer-a")) rightStates.append(state);
        });
        QObject::connect(&right, &TransferManager::textReceived, &right,
                         [&](const QString &, const QString &text) { receivedText = text; });
        QObject::connect(&left, &TransferManager::peerAuthorizationChanged, &left,
                         [&](const QString &peerId, const QString &, const QString &token,
                             bool allowed) {
            if (peerId == QStringLiteral("peer-b") && allowed) leftMediaToken = token;
        });
        QObject::connect(&right, &TransferManager::peerAuthorizationChanged, &right,
                         [&](const QString &peerId, const QString &, const QString &token,
                             bool allowed) {
            if (peerId == QStringLiteral("peer-a") && allowed) rightMediaToken = token;
        });

        QTcpSocket unauthorized;
        unauthorized.connectToHost(localhost, right.serverPort());
        if (!require(waitUntil([&] {
                return unauthorized.state() == QAbstractSocket::ConnectedState;
            }), "unauthorized test socket did not connect")) return 19;
        right.sendPacket(&unauthorized, TransferManager::MsgText,
                         [](QDataStream &out) { out << QStringLiteral("unauthorized"); });
        unauthorized.flush();
        if (!require(waitUntil([&] {
                return unauthorized.state() == QAbstractSocket::UnconnectedState;
            }), "unauthorized message connection was not rejected")) return 20;
        if (!require(receivedText.isEmpty(),
                     "text from an unpaired connection was delivered")) return 21;

        bool rejectFirstRequest = true;
        QObject::connect(&right, &TransferManager::pairingRequested, &right,
                         [&](const QString &peerId, const QString &, const QString &) {
            if (rejectFirstRequest) {
                rejectFirstRequest = false;
                right.rejectPairing(peerId);
            } else {
                right.acceptPairing(peerId);
            }
        });

        left.requestPairing(QStringLiteral("peer-b"), QStringLiteral("Right"),
                            localhost, right.serverPort());
        if (!require(waitUntil([&] {
                return left.connectionState(QStringLiteral("peer-b")) == QStringLiteral("unpaired");
            }), "pairing rejection did not return to unpaired")) return 2;

        left.requestPairing(QStringLiteral("peer-b"), QStringLiteral("Right"),
                            localhost, right.serverPort());
        if (!require(waitUntil([&] {
                return left.isPaired(QStringLiteral("peer-b"))
                    && right.isPaired(QStringLiteral("peer-a"));
            }), "pairing acceptance did not establish both sessions")) return 3;
        if (!require(left.m_connections.value(QStringLiteral("peer-b")).authKey.size() == 32
                     && left.m_connections.value(QStringLiteral("peer-b")).authKey
                         == right.m_connections.value(QStringLiteral("peer-a")).authKey,
                      "pairing did not establish a shared authentication key")) return 40;
        const QByteArray expectedMediaToken=TransferManager::mediaAuthorizationKey(
            left.m_connections.value(QStringLiteral("peer-b")).authKey).toBase64();
        if (!require(!leftMediaToken.isEmpty()&&leftMediaToken==rightMediaToken
                     &&leftMediaToken.toLatin1()==expectedMediaToken,
                     "pairing did not emit a matching derived media token")) return 49;
        {
            QSettings persisted(QStringLiteral("LanDropIntegrationTest"), QStringLiteral("left"));
            persisted.beginGroup(QStringLiteral("trustedPeers/peer-b"));
            const QByteArray storedKey = QByteArray::fromBase64(
                persisted.value(QStringLiteral("authKey")).toByteArray());
            persisted.endGroup();
            if (!require(storedKey == left.m_connections.value(QStringLiteral("peer-b")).authKey,
                         "paired authentication key was not persisted")) return 48;
        }

        QTcpSocket forgedControl;
        forgedControl.connectToHost(localhost, right.serverPort());
        if (!require(waitUntil([&] {
                return forgedControl.state() == QAbstractSocket::ConnectedState;
            }), "forged control socket did not connect")) return 41;
        left.sendPacket(&forgedControl, TransferManager::MsgPairRequest,
                        [&](QDataStream &out) {
            out << quint16(2) << QStringLiteral("peer-a") << QStringLiteral("Impostor")
                << left.serverPort() << true << QByteArray(32, 'c') << QByteArray{}
                << QByteArray(32, 'x');
        });
        forgedControl.flush();
        if (!require(waitUntil([&] {
                return forgedControl.state() == QAbstractSocket::UnconnectedState;
            }), "forged trusted-device reconnect was not rejected")) return 42;
        if (!require(left.isPaired(QStringLiteral("peer-b"))
                     && right.isPaired(QStringLiteral("peer-a")),
                     "forged reconnect displaced the authenticated session")) return 43;

        if (!require(closeControlSession(left, QStringLiteral("peer-b")),
                     "control session was not found")) return 4;
        if (!require(waitUntil([&] {
                return left.connectionState(QStringLiteral("peer-b")) == QStringLiteral("reconnecting")
                    || right.connectionState(QStringLiteral("peer-a")) == QStringLiteral("reconnecting");
            }), "disconnect did not enter reconnecting")) return 5;
        const bool reconnected = waitUntilStable([&] {
                return left.isPaired(QStringLiteral("peer-b"))
                    && right.isPaired(QStringLiteral("peer-a"));
            }, 150);
        if (!reconnected) {
            std::fprintf(stderr, "left=%s paired=%d states=%s sessions=%s\n"
                                 "right=%s paired=%d states=%s sessions=%s\n",
                         qPrintable(left.connectionState(QStringLiteral("peer-b"))),
                         left.isPaired(QStringLiteral("peer-b")),
                         qPrintable(leftStates.join(QLatin1Char(','))),
                         qPrintable(sessions(left)),
                         qPrintable(right.connectionState(QStringLiteral("peer-a"))),
                         right.isPaired(QStringLiteral("peer-a")),
                         qPrintable(rightStates.join(QLatin1Char(','))),
                         qPrintable(sessions(right)));
        }
        if (!require(reconnected, "automatic reconnect did not restore both sessions")) return 6;

        left.sendText(QStringLiteral("after-reconnect"), localhost, right.serverPort());
        if (!require(waitUntil([&] { return receivedText == QStringLiteral("after-reconnect"); }),
                     "message was not delivered after reconnect")) return 7;

        QTcpSocket forgedTransfer;
        forgedTransfer.connectToHost(localhost, right.serverPort());
        if (!require(waitUntil([&] {
                return forgedTransfer.state() == QAbstractSocket::ConnectedState;
            }), "forged transfer socket did not connect")) return 44;
        const QString forgedTransferId = QStringLiteral("forged-transfer");
        left.sendPacket(&forgedTransfer, TransferManager::MsgFileOffer,
                        [&](QDataStream &out) {
            out << quint16(2) << QStringLiteral("peer-a") << forgedTransferId
                << QStringLiteral("forged.bin") << qint64(16) << QByteArray(32, 'f')
                << QByteArray(32, 'n') << QByteArray(32, 'x');
        });
        forgedTransfer.flush();
        if (!require(waitUntil([&] {
                return forgedTransfer.state() == QAbstractSocket::UnconnectedState;
            }), "forged file offer was not rejected")) return 45;
        if (!require(!right.m_transferStore.find(forgedTransferId).has_value(),
                     "forged file offer reached persistent transfer state")) return 46;

        QTcpSocket stalledTransfer;
        stalledTransfer.connectToHost(localhost, right.serverPort());
        if (!require(waitUntil([&] {
                return stalledTransfer.state() == QAbstractSocket::ConnectedState;
            }), "stalled transfer socket did not connect")) return 31;
        const QString stalledId = QStringLiteral("stalled-transfer");
        QString stalledStatus;
        QObject::connect(&right, &TransferManager::taskUpdated, &right,
                         [&](const QString &id, qreal, const QString &status, const QString &) {
            if (id == stalledId) stalledStatus = status;
        });
        sendAuthenticatedFileOffer(left, &stalledTransfer, QStringLiteral("peer-b"),
                                   stalledId, QStringLiteral("stalled.bin"), qint64(4096),
                                   QByteArray(32, 's'));
        stalledTransfer.flush();
        if (!require(waitUntil([&] { return stalledStatus == QStringLiteral("receiving"); }),
                     "stalled transfer offer was not accepted")) return 32;
        if (!require(expireIncomingTransfer(right, stalledId),
                     "idle incoming transfer was not cleaned up")) return 33;
        if (!require(stalledStatus == QStringLiteral("paused"),
                     "idle incoming transfer did not enter paused state")) return 34;
        if (!require(waitUntil([&] {
                return stalledTransfer.state() == QAbstractSocket::UnconnectedState;
            }), "expired transfer socket remained connected")) return 35;

        QTcpSocket oversizedTransfer;
        oversizedTransfer.connectToHost(localhost, right.serverPort());
        if (!require(waitUntil([&] {
                return oversizedTransfer.state() == QAbstractSocket::ConnectedState;
            }), "oversized transfer socket did not connect")) return 36;
        const QString oversizedId = QStringLiteral("oversized-transfer");
        QString oversizedStatus;
        QObject::connect(&right, &TransferManager::taskUpdated, &right,
                         [&](const QString &id, qreal, const QString &status, const QString &) {
            if (id == oversizedId) oversizedStatus = status;
        });
        sendAuthenticatedFileOffer(left, &oversizedTransfer, QStringLiteral("peer-b"),
                                   oversizedId, QStringLiteral("oversized.bin"),
                                   qint64(std::numeric_limits<qint64>::max() / 4),
                                   QByteArray(32, 'o'));
        oversizedTransfer.flush();
        if (!require(waitUntil([&] { return oversizedStatus == QStringLiteral("no-space"); }),
                     "oversized transfer was not rejected before writing")) return 37;
        if (!require(waitUntil([&] {
                return oversizedTransfer.state() == QAbstractSocket::UnconnectedState;
            }), "no-space transfer socket remained connected")) return 38;
        if (!require(!right.m_transferStore.find(oversizedId).has_value(),
                     "rejected oversized transfer was persisted as resumable")) return 39;

        const QString sourcePath = temporary.filePath(QStringLiteral("resume-source.bin"));
        constexpr qint64 SourceSize = 64 * 1024 * 1024;
        if (!require(createSourceFile(sourcePath, SourceSize), "source file creation failed")) return 8;

        QString transferId;
        bool interruptionQueued = false;
        bool interruptionPerformed = false;
        bool sawNonZeroResume = false;
        bool senderVerified = false;
        bool receiverVerified = false;
        bool partialCorrupted = false;
        bool checksumFailureObserved = false;
        bool checksumRetryObserved = false;
        QObject::connect(&left, &TransferManager::taskAdded, &left,
                         [&](const QString &id, const QString &, const QString &, bool sender, qint64) {
            if (sender) transferId = id;
        });
        QObject::connect(&left, &TransferManager::taskUpdated, &left,
                         [&](const QString &id, qreal progress, const QString &status, const QString &) {
            if (id != transferId) return;
            if (status == QStringLiteral("resuming") && progress > 0) sawNonZeroResume = true;
            if (status == QStringLiteral("retrying")) checksumRetryObserved = true;
            if (status == QStringLiteral("verified")) senderVerified = true;
        });
        QObject::connect(&right, &TransferManager::taskUpdated, &right,
                         [&](const QString &id, qreal progress, const QString &status, const QString &) {
            if (transferId.isEmpty() || id != transferId) return;
            if (status == QStringLiteral("resuming") && progress > 0) sawNonZeroResume = true;
            if (status == QStringLiteral("checksum-error")) checksumFailureObserved = true;
            if (status == QStringLiteral("verified")) receiverVerified = true;
            if (!interruptionQueued && status == QStringLiteral("receiving")
                && progress > 0 && progress < 0.9) {
                interruptionQueued = true;
                QTimer::singleShot(0, &right, [&] {
                    interruptionPerformed = interruptIncomingTransfer(right);
                    if (interruptionPerformed)
                        partialCorrupted = corruptPartialTransfer(right, transferId);
                });
            }
        });

        left.sendFiles({QUrl::fromLocalFile(sourcePath)}, localhost, right.serverPort());
        if (!require(waitUntil([&] { return senderVerified && receiverVerified; }, 30000),
                     "interrupted transfer did not complete")) return 9;
        if (!require(interruptionPerformed, "transfer completed before interruption")) return 10;
        if (!require(sawNonZeroResume, "retry restarted from zero instead of saved offset")) return 11;
        if (!require(partialCorrupted, "saved partial transfer could not be corrupted")) return 22;
        if (!require(checksumFailureObserved, "corrupted partial file was not rejected")) return 23;
        if (!require(checksumRetryObserved, "checksum failure did not schedule a clean retry")) return 24;

        const QString receivedPath = right.saveDirectory() + QStringLiteral("/resume-source.bin");
        if (!require(QFileInfo(receivedPath).size() == SourceSize,
                     "resumed file size is incorrect")) return 12;
        if (!require(digest(receivedPath) == digest(sourcePath),
                     "resumed file content is incorrect")) return 13;

        const QString retryLimitSource = temporary.filePath(QStringLiteral("retry-limit.bin"));
        if (!require(createSourceFile(retryLimitSource, 4 * 1024 * 1024),
                     "retry limit source creation failed")) return 25;
        QString retryLimitId;
        int injectedChecksumFailures = 0;
        int receiverChecksumFailures = 0;
        int senderChecksumFailures = 0;
        int scheduledChecksumRetries = 0;
        bool retryLimitVerified = false;
        bool corruptionInjectionFailed = false;
        QObject::connect(&left, &TransferManager::taskAdded, &left,
                         [&](const QString &id, const QString &, const QString &name,
                             bool sender, qint64) {
            if (sender && name == QStringLiteral("retry-limit.bin")) retryLimitId = id;
        });
        QObject::connect(&left, &TransferManager::taskUpdated, &left,
                         [&](const QString &id, qreal, const QString &status, const QString &) {
            if (id != retryLimitId) return;
            if (status == QStringLiteral("checksum-error")) ++senderChecksumFailures;
            if (status == QStringLiteral("retrying")) ++scheduledChecksumRetries;
            if (status == QStringLiteral("verified")) retryLimitVerified = true;
        });
        QObject::connect(&right, &TransferManager::taskUpdated, &right,
                         [&](const QString &id, qreal, const QString &status, const QString &) {
            if (id != retryLimitId) return;
            if (status == QStringLiteral("verifying") && injectedChecksumFailures < 2) {
                if (corruptPartialTransfer(right, id)) ++injectedChecksumFailures;
                else corruptionInjectionFailed = true;
            }
            if (status == QStringLiteral("checksum-error")) ++receiverChecksumFailures;
        });

        left.sendFiles({QUrl::fromLocalFile(retryLimitSource)}, localhost, right.serverPort());
        if (!require(waitUntil([&] {
                return senderChecksumFailures == 2 && receiverChecksumFailures == 2
                    && transferInactive(left, retryLimitId);
            }, 15000), "checksum retry limit did not reach a terminal failure")) return 26;
        if (!require(!corruptionInjectionFailed && injectedChecksumFailures == 2,
                     "could not inject both checksum failures")) return 27;
        if (!require(scheduledChecksumRetries == 1,
                     "checksum failure retried more than once")) return 28;
        if (!require(!retryLimitVerified,
                     "repeatedly corrupted transfer was incorrectly verified")) return 29;
        if (!require(waitUntilStable([&] {
                return transferInactive(left, retryLimitId)
                    && senderChecksumFailures == 2 && scheduledChecksumRetries == 1;
            }, 300, 2000), "checksum retry continued after reaching its limit")) return 30;

        const QVariantList trusted = left.trustedPeers();
        const QVariantMap trustedPeer = trusted.isEmpty() ? QVariantMap{} : trusted.first().toMap();
        if (!require(trusted.size() == 1
                     && trustedPeer.value(QStringLiteral("id")).toString() == QStringLiteral("peer-b")
                     && trustedPeer.value(QStringLiteral("online")).toBool()
                     && trustedPeer.value(QStringLiteral("lastSeen")).toLongLong() > 0,
                     "trusted peer list does not reflect the live session")) return 14;

        PersistedTransfer pending;
        pending.id = QStringLiteral("pending-before-forget");
        pending.peerId = QStringLiteral("peer-b");
        pending.peerName = QStringLiteral("Right");
        pending.peerIp = localhost;
        pending.peerPort = right.serverPort();
        pending.isSender = true;
        pending.sourcePath = sourcePath;
        pending.fileName = QStringLiteral("pending.bin");
        pending.totalBytes = SourceSize;
        pending.status = QStringLiteral("paused");
        if (!require(left.m_transferStore.upsert(pending),
                     "pending transfer setup failed")) return 15;

        left.forgetPeer(QStringLiteral("peer-b"));
        const auto cancelled = left.m_transferStore.find(pending.id);
        right.forgetPeer(QStringLiteral("peer-a"));
        if (!require(left.connectionState(QStringLiteral("peer-b")) == QStringLiteral("unpaired")
                     && right.connectionState(QStringLiteral("peer-a")) == QStringLiteral("unpaired"),
                     "forget peer did not clear trust state")) return 16;
        if (!require(left.trustedPeers().isEmpty() && right.trustedPeers().isEmpty(),
                     "forgotten peer remains in trusted device list")) return 17;
        if (!require(cancelled && cancelled->status == QStringLiteral("cancelled"),
                     "forget peer did not cancel unfinished transfer")) return 18;
        return 0;
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("LanDropIntegrationTest"));
    QCoreApplication::setApplicationName(QStringLiteral("TransferManagerIntegrationTest"));
    return TransferManagerIntegrationTest::run();
}
