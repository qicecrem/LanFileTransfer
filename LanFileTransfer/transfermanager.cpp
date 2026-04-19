#include "transfermanager.h"
#include <QFileInfo>
#include <QStandardPaths>
#include <QDir>
#include <QDataStream>
#include <QDesktopServices>
#include <QDateTime>


#ifdef Q_OS_ANDROID
#include <QJniObject>

#include <QCoreApplication>

// 通过 Android 底层数据库查询 content:// 的真实文件名（含后缀）
QString getAndroidContentName(const QString &uriString) {
    QJniObject uri = QJniObject::callStaticMethod<QJniObject>(
        "android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;",
        QJniObject::fromString(uriString).object());

    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) return "";

    QJniObject contentResolver = context.callMethod<QJniObject>(
        "getContentResolver", "()Landroid/content/ContentResolver;");
    if (!contentResolver.isValid()) return "";

    // 相当于 Java: Cursor cursor = contentResolver.query(uri, null, null, null, null);
    QJniObject cursor = contentResolver.callMethod<QJniObject>(
        "query", "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
        uri.object(), nullptr, nullptr, nullptr, nullptr);

    QString fileName;
    if (cursor.isValid()) {
        if (cursor.callMethod<jboolean>("moveToFirst")) {
            // 查询 OpenableColumns.DISPLAY_NAME 字段
            QJniObject columnName = QJniObject::fromString("_display_name");
            jint columnIndex = cursor.callMethod<jint>("getColumnIndex", "(Ljava/lang/String;)I", columnName.object());

            if (columnIndex != -1) {
                QJniObject nameObj = cursor.callMethod<QJniObject>("getString", "(I)Ljava/lang/String;", columnIndex);
                if (nameObj.isValid()) {
                    fileName = nameObj.toString();
                }
            }
        }
        cursor.callMethod<void>("close");
    }
    return fileName;
}
#endif

TransferManager::TransferManager(QObject *parent) : QObject{parent}
{
#ifdef Q_OS_ANDROID
    // 强制指定 Android 的公有下载目录
    m_saveDirectory = "/storage/emulated/0/Download";
#else
    m_saveDirectory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
#endif

    m_server = new QTcpServer(this);
    m_server->listen(QHostAddress::AnyIPv4, 0);
    connect(m_server, &QTcpServer::newConnection, this, &TransferManager::onNewConnection);

}

void TransferManager::setSaveDirectory(const QString &dirUrl)
{
    QString localPath = dirUrl;

    if (dirUrl.startsWith("file://")) {
        localPath = QUrl(dirUrl).toLocalFile();
    }

#ifdef Q_OS_ANDROID
    // 特殊处理：Android 的路径转换极其复杂
    // 如果路径包含 "primary:" 这种特征，通常需要手动映射
    if (localPath.contains("primary:")) {
        QString subPath = localPath.split("primary:").last();
        localPath = "/storage/emulated/0/" + subPath;
    }
#endif

    m_saveDirectory = localPath;
    emit saveDirectoryChanged();
    qDebug() << "最终写入路径:" << m_saveDirectory;
}

// ================= 发送端并发逻辑 =================
void TransferManager::sendFiles(const QList<QUrl>  &fileUrls, const QString &ip, quint16 port)
{
    for (const QUrl &url : fileUrls) {

        QString filePath;
        QString fileName;

        if (url.isLocalFile()) {
            // Windows/Mac/Linux 桌面端正常处理
            filePath = url.toLocalFile();
            fileName = QFileInfo(filePath).fileName();
        } else {
            // Android 端处理 (url 格式为 content://...)
            filePath = url.toString();

#ifdef Q_OS_ANDROID
            // 调用底层 Android API 查询真实的带后缀的文件名
            fileName = getAndroidContentName(filePath);
#endif
            // 兜底方案：如果查询失败，给一个基于时间戳的名字避免覆盖
            if (fileName.isEmpty()) {
                fileName = QString("AndroidFile_%1.bin").arg(QDateTime::currentMSecsSinceEpoch());
            }
        }

        if (filePath.isEmpty()) {
            qWarning() << "无效的文件路径，跳过:" << url;
            continue;
        }

        QTcpSocket *socket = new QTcpSocket(this);

        TransferContext *ctx = new TransferContext();
        ctx->id = QUuid::createUuid().toString();
        ctx->isSender = true;
        ctx->fileName = fileName;
        ctx->totalBytes = 0;

        ctx->file = new QFile(filePath, this);
        m_tasks[socket] = ctx;

        // 尝试打开文件
        if (!ctx->file->open(QIODevice::ReadOnly)) {
            emit taskAdded(ctx->id, ctx->fileName, true, 0);
            emit taskUpdated(ctx->id, 0.0, "无读取权限");
            m_tasks.remove(socket);
            delete ctx->file;
            ctx->file = nullptr;
            delete ctx;
            socket->deleteLater();
            continue;
        }

        // 打开成功后，获取真实大小并通知 UI
        ctx->totalBytes = ctx->file->size();
        emit taskAdded(ctx->id, ctx->fileName, true, ctx->totalBytes);
        emit taskUpdated(ctx->id, 0.0, "等待连接...");

        connect(socket, &QTcpSocket::connected, this, [this, socket, ctx]() {
            emit taskUpdated(ctx->id, 0.0, "正在发送...");
            QByteArray block;
            QDataStream out(&block, QIODevice::WriteOnly);
            out.setVersion(QDataStream::Qt_6_5);
            out << (quint32)0 << (quint8)MsgFileInfo << ctx->fileName << ctx->totalBytes;
            out.device()->seek(0);
            out << (quint32)(block.size() - sizeof(quint32));
            socket->write(block);
            sendNextChunk(socket);
        });

        connect(socket, &QTcpSocket::bytesWritten, this, [this, socket, ctx](qint64) {
            if (ctx->file && ctx->file->isOpen()) {
                sendNextChunk(socket);
            } else if (socket->bytesToWrite() == 0) {
                cleanupSocket(socket, "发送完成");
            }
        });

        connect(socket, &QTcpSocket::disconnected, this, &TransferManager::onSocketDisconnected);
        connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError error){
            QString errorMsg = "网络错误: " + socket->errorString();
            cleanupSocket(socket, errorMsg);
        });

        socket->connectToHost(ip, port);
    }
}


void TransferManager::sendNextChunk(QTcpSocket *socket)
{
    TransferContext *ctx = m_tasks.value(socket, nullptr);
    if (!ctx || !ctx->file || !ctx->file->isOpen()) return;

    // 如果底层的发送缓冲区还有超过 2MB 数据没发出去，就暂时不读本地文件
    // 直接返回。当底层把缓冲区数据发出去后，会自动再次触发 bytesWritten 信号并进入这里
    if (socket->bytesToWrite() > 2 * 1024 * 1024) return;

    //加个循环一次性多写入几块，以填满底层缓冲区，提升局域网千兆传输速度
    while (socket->bytesToWrite() <= 2 * 1024 * 1024 && !ctx->file->atEnd()) {
        QByteArray payload = ctx->file->read(256 * 1024); // 256KB 块

        QByteArray block;
        QDataStream out(&block, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_5);
        out << (quint32)0 << (quint8)MsgFileData << payload;
        out.device()->seek(0);
        out << (quint32)(block.size() - sizeof(quint32));
        socket->write(block);

        qreal p = (qreal)ctx->file->pos() / (qreal)ctx->totalBytes;
        int currentPct = static_cast<int>(p * 100);
        // 只有当百分比增加（即跨越 1%），或者到达 100% 时，才通知 UI
        if (currentPct > ctx->lastProgressPct || p >= 1.0) {
            ctx->lastProgressPct = currentPct;
            emit taskUpdated(ctx->id, p, ctx->isSender ? "传输中..." : "接收中...");
        }
    }

    if (ctx->file->atEnd()) {
        ctx->file->close();
        ctx->file->deleteLater();
        ctx->file = nullptr;
        emit taskUpdated(ctx->id, 1.0, "等待网络同步...");
    }
}

// ================= 接收端并发逻辑 =================
void TransferManager::onNewConnection()
{
    QTcpSocket *socket = m_server->nextPendingConnection();

    // 新连接到来，创建接收上下文
    TransferContext *ctx = new TransferContext();
    ctx->id = QUuid::createUuid().toString();
    ctx->isSender = false;
    m_tasks[socket] = ctx;

    connect(socket, &QTcpSocket::readyRead, this, &TransferManager::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &TransferManager::onSocketDisconnected);
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError error){
        QString errorMsg = "接收中断: " + socket->errorString();
        cleanupSocket(socket, errorMsg);
    });
}

void TransferManager::onReadyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    TransferContext *ctx = m_tasks.value(socket, nullptr);
    if (!ctx) return;

    QDataStream in(socket);
    in.setVersion(QDataStream::Qt_6_5);

    while (true) {
        if (ctx->blockSize == 0) {
            if (socket->bytesAvailable() < sizeof(quint32)) return;
            in >> ctx->blockSize;
        }
        if (socket->bytesAvailable() < ctx->blockSize) return;

        QByteArray packetData = socket->read(ctx->blockSize);
        QDataStream packetStream(&packetData, QIODevice::ReadOnly);
        packetStream.setVersion(QDataStream::Qt_6_5);

        quint8 msgType;
        packetStream >> msgType;

        if (msgType == MsgText) {
            QString text;
               packetStream >> text;
            // 获取发送方 IP 并清理 IPv6 映射前缀
            QString senderIp = socket->peerAddress().toString();
            senderIp.remove("::ffff:");

            emit textReceived(senderIp, text);
            cleanupSocket(socket); // 接收完毕，直接清理该连接
            return;
        }
        else if (msgType == MsgFileInfo) {
            packetStream  >> ctx->fileName >> ctx->totalBytes;
            emit taskAdded(ctx->id, ctx->fileName, false, ctx->totalBytes);
            emit taskUpdated(ctx->id, 0.0, "准备接收...");

            QDir dir(m_saveDirectory);
            if (!dir.exists()) {
                dir.mkpath(".");
            }

            QString savePath = QDir(m_saveDirectory).filePath(ctx->fileName);
            QFileInfo fileInfo(savePath);
            int counter = 1;
            while (fileInfo.exists()) {
                QString newName = QString("%1(%2).%3")
                .arg(fileInfo.completeBaseName())
                    .arg(counter++)
                    .arg(fileInfo.suffix());
                savePath = QDir(m_saveDirectory).filePath(newName);
                fileInfo.setFile(savePath);
            }
            ctx->file = new QFile(savePath, this);
            if (!ctx->file->open(QIODevice::WriteOnly)) {
                QString errorMsg = "写入失败: " + ctx->file->errorString();
                qWarning() << "无法创建文件:" << savePath << " 原因:" << ctx->file->errorString();
                cleanupSocket(socket, errorMsg);
                return;
            }
        }
        else if (msgType == MsgFileData) {
            QByteArray payload;
            packetStream  >> payload;
            if (ctx->file && ctx->file->isOpen()) {
                ctx->file->write(payload);
                qreal p = (qreal)ctx->file->pos() / (qreal)ctx->totalBytes;
                int currentPct = static_cast<int>(p * 100);
                if (currentPct > ctx->lastProgressPct || p >= 1.0) {
                    ctx->lastProgressPct = currentPct;
                    emit taskUpdated(ctx->id, p, "接收中...");
                }

                if (ctx->file->pos() >= ctx->totalBytes) {
                    cleanupSocket(socket, "接收完成");
                    return;
                }
            }
        }
        else {
            qWarning() << "收到未知类型的消息:" << msgType;
        }

        ctx->blockSize = 0;
        if (socket->bytesAvailable() == 0) break;
    }
}

// ================= 资源清理 =================
void TransferManager::onSocketDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    cleanupSocket(socket, "连接断开");
}

void TransferManager::cleanupSocket(QTcpSocket *socket, const QString &finalStatus)
{
    if (!socket) return;
    TransferContext *ctx = m_tasks.take(socket);
    if (ctx) {
        bool incomplete = false;
        if (ctx->file) {
            // 如果文件还没写完/读完就断开，说明失败了
            if (ctx->file->pos() < ctx->totalBytes) {
                incomplete = true;
            }
            ctx->file->close();
            ctx->file->deleteLater();
        }

        QString status = finalStatus;
        if (incomplete && status == "连接断开") {
            status = "传输中断";
        }
        if (!ctx->fileName.isEmpty()) {
            emit taskUpdated(ctx->id, 1.0, status);
        }



        delete ctx;
    }
    // 断开所有关联的 Qt 信号槽，防止后续对象销毁期间触发野指针
    socket->disconnect();
    // 先强制断开底层 TCP 连接（如果还在连接的话）
    socket->abort();

    socket->deleteLater();
}


void TransferManager::openFolder()
{
#ifdef Q_OS_ANDROID
    // Android 端：调用原生 Intent 打开系统文件管理器 / 下载目录
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (context.isValid()) {
        // 使用 ACTION_VIEW_DOWNLOADS，这是 Android 官方支持的跳转到“下载内容”或文件管理器的常量
        QJniObject action = QJniObject::fromString("android.intent.action.VIEW_DOWNLOADS");
        QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V", action.object());

        // 添加 FLAG_ACTIVITY_NEW_TASK 标志 (0x10000000)，否则在某些系统上无法从后台拉起独立应用
        intent.callMethod<QJniObject>("addFlags", "(I)Landroid/content/Intent;", 0x10000000);

        // 启动 Activity
        context.callMethod<void>("startActivity", "(Landroid/content/Intent;)V", intent.object());
    }
#else
    // Windows / Mac / Linux 桌面端：直接调用资源管理器打开指定目录
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_saveDirectory));
#endif
}

void TransferManager::sendText(const QString &text, const QString &ip, quint16 port)
{
    QTcpSocket *socket = new QTcpSocket(this);
    TransferContext *ctx = new TransferContext();
    ctx->id = QUuid::createUuid().toString();
    ctx->isSender = true;
    // 注意：文本传输不需要绑定 file，也不触发 taskAdded (文件列表里不显示它)
    m_tasks[socket] = ctx;

    connect(socket, &QTcpSocket::connected, this, [socket, text]() {
        QByteArray block;
        QDataStream out(&block, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_5);
        // 打包：大小(占位) + 类型(MsgText) + 文本内容
        out << (quint32)0 << (quint8)MsgText << text;
        out.device()->seek(0);
        out << (quint32)(block.size() - sizeof(quint32));
        socket->write(block);
    });

    // 写入完毕后主动断开
    connect(socket, &QTcpSocket::bytesWritten, this, [socket](qint64) {
        if (socket->bytesToWrite() == 0) {
            socket->disconnectFromHost();
        }
    });

    connect(socket, &QTcpSocket::disconnected, this, &TransferManager::onSocketDisconnected);
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
        cleanupSocket(socket);
    });

    socket->connectToHost(ip, port);
}


