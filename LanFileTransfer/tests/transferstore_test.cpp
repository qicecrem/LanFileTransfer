#include "transferstore.h"

#include <QCoreApplication>
#include <QDebug>

namespace {
bool require(bool condition, const char *message)
{
    if (!condition) qCritical() << message;
    return condition;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TransferStore store(QStringLiteral(":memory:"));
    if (!require(store.isOpen(), "store did not open")) return 1;

    PersistedTransfer task;
    task.id = QStringLiteral("task-1");
    task.peerId = QStringLiteral("peer-1");
    task.peerName = QStringLiteral("Peer");
    task.peerIp = QStringLiteral("192.168.1.8");
    task.peerPort = 45455;
    task.isSender = true;
    task.sourcePath = QStringLiteral("source.bin");
    task.fileName = QStringLiteral("source.bin");
    task.totalBytes = 4096;
    task.checksum = QByteArray(32, 'a');
    task.progress = .5;
    task.status = QStringLiteral("transferring");
    if (!require(store.upsert(task), "upsert failed")) return 2;

    const auto saved = store.find(task.id);
    if (!require(saved.has_value(), "saved task not found")
        || !require(saved->peerId == task.peerId, "peer id changed")
        || !require(saved->checksum == task.checksum, "checksum changed")) return 3;

    store.markRunningTasksPaused();
    const auto paused = store.find(task.id);
    if (!require(paused && paused->status == QStringLiteral("paused"),
                 "running task was not paused")) return 4;
    if (!require(store.unfinished().size() == 1, "paused task was not restorable")) return 5;

    store.updateState(task.id, 1, QStringLiteral("verified"), task.checksum);
    if (!require(store.unfinished().isEmpty(), "verified task was restored")) return 6;
    return 0;
}
