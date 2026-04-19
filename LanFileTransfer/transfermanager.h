#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMap>
#include <QFile>
#include <QUrl>
#include <QUuid>

/**
 * @brief 传输上下文：管理单个文件传输任务的完整状态
 *
 * 每个并发的文件传输（发送或接收）拥有独立的上下文实例。
 */
struct TransferContext {
    QString id;              ///< 任务唯一标识符（UUID）
    bool isSender;           ///< 是否为发送端（true=发送，false=接收）
    QString fileName;        ///< 文件名（不含路径）
    qint64 totalBytes = 0;   ///< 文件总大小（字节）
    QFile *file = nullptr;   ///< 文件句柄（读写用）
    quint32 blockSize = 0;   ///< 粘包处理用的当前数据块大小
    int lastProgressPct = -1;  ///< 记录上一次通知的百分比(0-100)，用于节流
};

/**
 * @brief 文件传输管理器
 *
 * 提供局域网内点对点文件传输功能：
 * - TCP 服务端监听端口，接收传入的文件
 * - TCP 客户端主动连接，发送选中的文件
 * - 支持多任务并发，通过 taskAdded/taskUpdated 信号通知 QML 界面
 */
class TransferManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    /// QML 可读写属性：文件保存目录（支持 file:// URL）
    Q_PROPERTY(QString saveDirectory READ saveDirectory WRITE setSaveDirectory NOTIFY saveDirectoryChanged)
    /// QML 只读属性：当前 TCP 服务端口号
    Q_PROPERTY(quint16 serverPort READ serverPort CONSTANT)

public:
    explicit TransferManager(QObject *parent = nullptr);

    /// 获取 TCP 服务端监听的端口号
    quint16 serverPort() const { return m_server->serverPort(); }
    /// 获取当前文件保存目录（本地路径）
    QString saveDirectory() const { return m_saveDirectory; }

    /// 设置文件保存目录（自动处理 file:// 前缀及 Android 特殊路径）
    Q_INVOKABLE void setSaveDirectory(const QString &dirUrl);

     Q_INVOKABLE void openFolder();

    /// 消息类型枚举（用于 TCP 数据流标记）
    enum MessageType {
        MsgHandshake = 1,   ///< 握手消息（当前未使用，预留）
        MsgFileInfo = 2,    ///< 文件信息（文件名、总大小）
        MsgFileData = 3,     ///< 文件数据块（二进制负载）
        MsgText = 4
    };

    /**
     * @brief 发送文件列表到指定对端
     * @param fileUrls 文件 URL 列表（支持 file:// 或本地路径）
     * @param ip        目标 IP 地址（IPv4 字符串）
     * @param port      目标 TCP 端口号（由对端服务端提供）
     *
     * 为每个文件创建独立的 TCP 连接和传输上下文，实现并发发送。
     */
    Q_INVOKABLE void sendFiles(const QList<QUrl> &fileUrls, const QString &ip, quint16 port);
    Q_INVOKABLE void sendText(const QString &text, const QString &ip, quint16 port);


signals:
    /// 保存目录变化时发射
    void saveDirectoryChanged();

    /**
     * @brief 新传输任务添加时发射（QML 界面据此创建传输条目）
     * @param id         任务唯一标识
     * @param fileName   文件名
     * @param isSender   是否为发送端
     * @param totalBytes 文件总大小
     */
    void taskAdded(QString taskId, QString fileName, bool isSender, qint64 totalBytes);

    /**
     * @brief 传输进度或状态更新时发射
     * @param id       任务唯一标识
     * @param progress 进度（0.0 ~ 1.0）
     * @param status   状态描述文本（如“传输中...”、“完成”、“错误”）
     */
    void taskUpdated(QString taskId, qreal progress, QString status);
    void textReceived(QString ip, QString text);

private slots:
    void onNewConnection();       ///< 新 TCP 连接到达（服务端）
    void onReadyRead();           ///< socket 数据可读（接收端处理）
    void onSocketDisconnected();  ///< socket 断开连接

private:
    QString m_saveDirectory;      ///< 文件保存目录（本地绝对路径）
    QTcpServer *m_server;         ///< TCP 服务端，监听传入连接

    /// 任务映射表：socket -> 传输上下文（支持多任务并发）
    QMap<QTcpSocket*, TransferContext*> m_tasks;

    void sendNextChunk(QTcpSocket *socket);                 ///< 发送下一个数据块
    void cleanupSocket(QTcpSocket *socket, const QString &finalStatus = ""); ///< 清理 socket 及关联资源
};
