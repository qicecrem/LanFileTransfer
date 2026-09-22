#include "transferstore.h"

#include <QDateTime>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QDebug>

namespace {
constexpr auto Columns =
    "id,peer_id,peer_name,peer_ip,peer_port,is_sender,source_path,file_name,"
    "total_bytes,checksum,partial_path,final_path,progress,status,created_at,updated_at";
}

TransferStore::TransferStore(const QString &databasePath)
    : m_connectionName(QStringLiteral("landrop-transfers-%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (databasePath.isEmpty()) QDir().mkpath(root);
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(databasePath.isEmpty()
        ? QDir(root).filePath(QStringLiteral("transfers.sqlite3")) : databasePath);
    if (!m_database.open()) {
        qCritical() << "Unable to open transfer task store" << m_database.lastError().text();
        return;
    }
    QSqlQuery query(m_database);
    query.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    query.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS transfer_tasks ("
            "id TEXT PRIMARY KEY, peer_id TEXT NOT NULL, peer_name TEXT, peer_ip TEXT,"
            "peer_port INTEGER NOT NULL DEFAULT 0, is_sender INTEGER NOT NULL,"
            "source_path TEXT, file_name TEXT NOT NULL, total_bytes INTEGER NOT NULL DEFAULT 0,"
            "checksum BLOB, partial_path TEXT, final_path TEXT, progress REAL NOT NULL DEFAULT 0,"
            "status TEXT NOT NULL, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL)")))
        qCritical() << "Unable to create transfer task table" << query.lastError().text();
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_transfer_tasks_peer_status "
        "ON transfer_tasks(peer_id,status,updated_at)"));
}

TransferStore::~TransferStore()
{
    if (m_database.isOpen()) m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool TransferStore::upsert(const PersistedTransfer &transfer)
{
    if (!m_database.isOpen() || transfer.id.isEmpty() || transfer.peerId.isEmpty()) return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO transfer_tasks(") + QString::fromLatin1(Columns)
        + QStringLiteral(") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
                         "ON CONFLICT(id) DO UPDATE SET peer_id=excluded.peer_id,peer_name=excluded.peer_name,"
                         "peer_ip=excluded.peer_ip,peer_port=excluded.peer_port,is_sender=excluded.is_sender,"
                         "source_path=excluded.source_path,file_name=excluded.file_name,total_bytes=excluded.total_bytes,"
                         "checksum=excluded.checksum,partial_path=excluded.partial_path,final_path=excluded.final_path,"
                         "progress=excluded.progress,status=excluded.status,updated_at=excluded.updated_at"));
    query.addBindValue(transfer.id);
    query.addBindValue(transfer.peerId);
    query.addBindValue(transfer.peerName);
    query.addBindValue(transfer.peerIp);
    query.addBindValue(transfer.peerPort);
    query.addBindValue(transfer.isSender);
    query.addBindValue(transfer.sourcePath);
    query.addBindValue(transfer.fileName);
    query.addBindValue(transfer.totalBytes);
    query.addBindValue(transfer.checksum);
    query.addBindValue(transfer.partialPath);
    query.addBindValue(transfer.finalPath);
    query.addBindValue(qBound<qreal>(0, transfer.progress, 1));
    query.addBindValue(transfer.status);
    query.addBindValue(transfer.createdAt > 0 ? transfer.createdAt : now);
    query.addBindValue(now);
    if (query.exec()) return true;
    qWarning() << "Unable to persist transfer task" << transfer.id << query.lastError().text();
    return false;
}

PersistedTransfer TransferStore::fromQuery(const QSqlQuery &query)
{
    PersistedTransfer transfer;
    transfer.id = query.value(0).toString();
    transfer.peerId = query.value(1).toString();
    transfer.peerName = query.value(2).toString();
    transfer.peerIp = query.value(3).toString();
    transfer.peerPort = quint16(query.value(4).toUInt());
    transfer.isSender = query.value(5).toBool();
    transfer.sourcePath = query.value(6).toString();
    transfer.fileName = query.value(7).toString();
    transfer.totalBytes = query.value(8).toLongLong();
    transfer.checksum = query.value(9).toByteArray();
    transfer.partialPath = query.value(10).toString();
    transfer.finalPath = query.value(11).toString();
    transfer.progress = query.value(12).toDouble();
    transfer.status = query.value(13).toString();
    transfer.createdAt = query.value(14).toLongLong();
    transfer.updatedAt = query.value(15).toLongLong();
    return transfer;
}

std::optional<PersistedTransfer> TransferStore::find(const QString &id) const
{
    if (!m_database.isOpen() || id.isEmpty()) return std::nullopt;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT ") + QString::fromLatin1(Columns)
                  + QStringLiteral(" FROM transfer_tasks WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec() || !query.next()) return std::nullopt;
    return fromQuery(query);
}

QList<PersistedTransfer> TransferStore::unfinished() const
{
    QList<PersistedTransfer> result;
    if (!m_database.isOpen()) return result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT ") + QString::fromLatin1(Columns)
        + QStringLiteral(" FROM transfer_tasks "
                         "WHERE status NOT IN ('verified','cancelled','read-error','write-error',"
                         "'checksum-error','protocol-error','no-space','rejected') "
                         "ORDER BY created_at"));
    if (!query.exec()) return result;
    while (query.next()) result.append(fromQuery(query));
    return result;
}

void TransferStore::markRunningTasksPaused()
{
    if (!m_database.isOpen()) return;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE transfer_tasks SET status='paused',updated_at=? "
        "WHERE status IN ('queued','verifying','connecting','negotiating','transferring',"
        "'receiving','resuming')"));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.exec();
}

void TransferStore::updateState(const QString &id, qreal progress, const QString &status,
                                const QByteArray &checksum)
{
    if (!m_database.isOpen() || id.isEmpty()) return;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE transfer_tasks SET progress=?,status=?,"
        "checksum=CASE WHEN length(?)=0 THEN checksum ELSE ? END,updated_at=? WHERE id=?"));
    query.addBindValue(qBound<qreal>(0, progress, 1));
    query.addBindValue(status);
    query.addBindValue(checksum);
    query.addBindValue(checksum);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(id);
    query.exec();
}

void TransferStore::updateSize(const QString &id, qint64 totalBytes)
{
    if (!m_database.isOpen() || id.isEmpty() || totalBytes < 0) return;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE transfer_tasks SET total_bytes=?,updated_at=? WHERE id=?"));
    query.addBindValue(totalBytes);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(id);
    query.exec();
}
