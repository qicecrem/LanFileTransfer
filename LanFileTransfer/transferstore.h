#pragma once

#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <optional>

struct PersistedTransfer {
    QString id;
    QString peerId;
    QString peerName;
    QString peerIp;
    quint16 peerPort = 0;
    bool isSender = false;
    QString sourcePath;
    QString fileName;
    qint64 totalBytes = 0;
    QByteArray checksum;
    QString partialPath;
    QString finalPath;
    qreal progress = 0;
    QString status;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;
};

class TransferStore {
public:
    explicit TransferStore(const QString &databasePath = {});
    ~TransferStore();

    bool isOpen() const { return m_database.isOpen(); }
    bool upsert(const PersistedTransfer &transfer);
    std::optional<PersistedTransfer> find(const QString &id) const;
    QList<PersistedTransfer> unfinished() const;
    void markRunningTasksPaused();
    void updateState(const QString &id, qreal progress, const QString &status,
                     const QByteArray &checksum = {});
    void updateSize(const QString &id, qint64 totalBytes);

private:
    static PersistedTransfer fromQuery(const class QSqlQuery &query);
    QString m_connectionName;
    QSqlDatabase m_database;
};
