#include "transfermanager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
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

    static int run()
    {
        QTemporaryDir temporary;
        if (!require(temporary.isValid(), "temporary directory unavailable")) return 1;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           temporary.path() + QStringLiteral("/settings"));

        TransferManager left(options(temporary.path(), QStringLiteral("peer-a"),
                                     QStringLiteral("left")));
        TransferManager right(options(temporary.path(), QStringLiteral("peer-b"),
                                      QStringLiteral("right")));
        const QString localhost = QStringLiteral("127.0.0.1");
        QStringList leftStates;
        QStringList rightStates;
        QObject::connect(&left, &TransferManager::pairingStateChanged, &left,
                         [&](const QString &peerId, const QString &state) {
            if (peerId == QStringLiteral("peer-b")) leftStates.append(state);
        });
        QObject::connect(&right, &TransferManager::pairingStateChanged, &right,
                         [&](const QString &peerId, const QString &state) {
            if (peerId == QStringLiteral("peer-a")) rightStates.append(state);
        });

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

        QString receivedText;
        QObject::connect(&right, &TransferManager::textReceived, &right,
                         [&](const QString &, const QString &text) { receivedText = text; });
        left.sendText(QStringLiteral("after-reconnect"), localhost, right.serverPort());
        if (!require(waitUntil([&] { return receivedText == QStringLiteral("after-reconnect"); }),
                     "message was not delivered after reconnect")) return 7;

        const QString sourcePath = temporary.filePath(QStringLiteral("resume-source.bin"));
        constexpr qint64 SourceSize = 64 * 1024 * 1024;
        if (!require(createSourceFile(sourcePath, SourceSize), "source file creation failed")) return 8;

        QString transferId;
        bool interruptionQueued = false;
        bool interruptionPerformed = false;
        bool sawNonZeroResume = false;
        bool senderVerified = false;
        bool receiverVerified = false;
        QObject::connect(&left, &TransferManager::taskAdded, &left,
                         [&](const QString &id, const QString &, const QString &, bool sender, qint64) {
            if (sender) transferId = id;
        });
        QObject::connect(&left, &TransferManager::taskUpdated, &left,
                         [&](const QString &id, qreal progress, const QString &status, const QString &) {
            if (id != transferId) return;
            if (status == QStringLiteral("resuming") && progress > 0) sawNonZeroResume = true;
            if (status == QStringLiteral("verified")) senderVerified = true;
        });
        QObject::connect(&right, &TransferManager::taskUpdated, &right,
                         [&](const QString &id, qreal progress, const QString &status, const QString &) {
            if (transferId.isEmpty() || id != transferId) return;
            if (status == QStringLiteral("resuming") && progress > 0) sawNonZeroResume = true;
            if (status == QStringLiteral("verified")) receiverVerified = true;
            if (!interruptionQueued && status == QStringLiteral("receiving")
                && progress > 0 && progress < 0.9) {
                interruptionQueued = true;
                QTimer::singleShot(0, &right, [&] {
                    interruptionPerformed = interruptIncomingTransfer(right);
                });
            }
        });

        left.sendFiles({QUrl::fromLocalFile(sourcePath)}, localhost, right.serverPort());
        if (!require(waitUntil([&] { return senderVerified && receiverVerified; }, 30000),
                     "interrupted transfer did not complete")) return 9;
        if (!require(interruptionPerformed, "transfer completed before interruption")) return 10;
        if (!require(sawNonZeroResume, "retry restarted from zero instead of saved offset")) return 11;

        const QString receivedPath = right.saveDirectory() + QStringLiteral("/resume-source.bin");
        if (!require(QFileInfo(receivedPath).size() == SourceSize,
                     "resumed file size is incorrect")) return 12;
        if (!require(digest(receivedPath) == digest(sourcePath),
                     "resumed file content is incorrect")) return 13;

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
