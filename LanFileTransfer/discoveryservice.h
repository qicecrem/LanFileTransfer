/**
 * @file discoveryservice.h
 * @brief 局域网设备发现服务 - 基于 UDP 广播实现零配置设备发现
 *
 * 该服务通过 UDP 广播发送心跳包，并监听同一子网内其他设备的心跳，
 * 动态维护在线设备列表，供 QML 界面展示。
 */

#ifndef DISCOVERYSERVICE_H
#define DISCOVERYSERVICE_H

#include <QObject>
#include <QQmlEngine>
#include <QUdpSocket>
#include <QTimer>
#include <QVariantList>
#include <QMap>
#include <QUuid>
#include <QNetworkInterface>

/**
 * @class DiscoveryService
 * @brief 设备发现服务类，负责局域网内对等设备的自动发现与状态维护
 *
 * 工作流程：
 * 1. startScan() 启动后，定时广播本机信息（心跳）
 * 2. 同时监听 UDP 端口，接收其他设备的心跳
 * 3. 根据心跳更新设备列表，超时未响应的设备自动移除
 *
 * 该类被注册为 QML 类型，可在 QML 中直接使用：
 * @code {qml}
 * DiscoveryService {
 *     id: discoverer
 *     onDeviceListChanged: console.log(deviceList)
 * }
 * @endcode
 */
class DiscoveryService : public QObject
{
    Q_OBJECT
    QML_ELEMENT          ///< 将该类暴露给 QML 引擎，无需额外注册

    /**
     * @property QVariantList deviceList
     * @brief 当前在线设备列表，每个设备为一个 QVariantMap，包含 id, ip, port, name 等信息
     *
     * 该属性可读不可写，QML 中可绑定此属性实现 UI 自动刷新。
     * 当设备列表变化（新增、移除或更新）时，会发射 deviceListChanged() 信号。
     */
    Q_PROPERTY(QVariantList deviceList READ deviceList NOTIFY deviceListChanged)
    Q_PROPERTY(QString deviceName READ deviceName WRITE setDeviceName NOTIFY deviceNameChanged)

public:
    /**
     * @brief 构造函数
     * @param parent 父对象指针，用于 Qt 对象树管理
     *
     * 初始化 UDP socket 和定时器，但不开始扫描。
     * 需要调用 startScan() 后才会发送和接收广播。
     */
    explicit DiscoveryService(QObject *parent = nullptr);

    /**
     * @brief 析构函数，确保释放 Android 组播锁等资源
     */
    ~DiscoveryService();

    /**
     * @brief 获取当前设备列表（只读）
     * @return QVariantList 每个元素是一个 QVariantMap，包含设备详细信息
     */
    QVariantList deviceList() const { return m_deviceList; }
    QString deviceName() const { return m_deviceName; }
    void setDeviceName(const QString &name);

    /**
     * @brief 设置本机 TCP 服务端口号（例如文件传输服务端口）
     * @param port TCP 端口号
     *
     * 在广播心跳时，会将该端口号告知其他设备，
     * 以便它们建立 TCP 连接进行文件传输。
     */
    Q_INVOKABLE void setLocalTcpPort(quint16 port) { m_localTcpPort = port; }
    Q_INVOKABLE void setLocalMediaPort(quint16 port) { m_localMediaPort = port; }

    /**
     * @brief 开始扫描局域网设备
     *
     * 启动定时器，周期性发送 UDP 广播心跳；
     * 同时绑定 UDP socket 开始接收数据报。
     * 调用后 deviceList 会逐渐填充发现的设备。
     * 如果已经处于扫描状态，则重复调用无效。
     */
    Q_INVOKABLE void startScan();
    Q_INVOKABLE void refreshNetwork();

    /**
     * @brief 停止扫描并清空设备列表
     *
     * 停止定时器，关闭 UDP socket（如有必要），清除所有已发现的设备记录。
     * 在 Android 平台同时释放组播锁。
     */
    Q_INVOKABLE void stopScan();

    /**
     * @brief 自动向 Windows 防火墙添加白名单规则
     * @return 成功用户授权并添加返回 true，否则返回 false
     */
    Q_INVOKABLE bool fixWindowsFirewall();

signals:
    /**
     * @brief 设备列表发生变化时发射此信号
     *
     * 包括：新设备加入、设备离线移除、设备信息更新。
     * QML 中可以绑定该信号来刷新 UI。
     */
    void deviceListChanged();
       void deviceNameChanged();

private slots:
    /**
     * @brief 定时触发的槽函数，向局域网广播本机心跳
     *
     * 心跳数据包包含：本机 UUID、主机名、TCP 服务端口等信息。
     * 广播地址为 255.255.255.255，端口固定为 DISCOVERY_PORT。
     */
    void broadcastHeartbeat();

    /**
     * @brief 处理接收到的 UDP 数据报
     *
     * 解析其他设备的心跳包，更新设备列表和最后存活时间戳。
     * 如果收到本机自己的广播，则忽略。
     */
    void processPendingDatagrams();

private:
    QUdpSocket *m_udpSocket;   ///< UDP 套接字，用于发送广播和接收数据
    QTimer *m_timer;           ///< 定时器，控制心跳发送频率（3 秒一次）

    QVariantList m_deviceList;      ///< 当前在线设备列表（用于暴露给 QML）
    QMap<QString, qint64> m_lastSeen;
    QMap<QString, QVariantMap> m_peers;
    QList<QNetworkInterface> m_joinedInterfaces;

    QString m_instanceId;           ///< 本机唯一标识符（UUID），用于区分不同设备
    quint16 m_localTcpPort = 0;     ///< 本机文件传输服务的 TCP 端口号
    quint16 m_localMediaPort = 0;   ///< 视频与屏幕共享服务端口
     QString m_deviceName;

#ifdef Q_OS_ANDROID
    bool m_multicastAcquired = false; ///< Java 层共享组播锁的使用权
#endif

    /**
     * @brief 刷新设备列表，移除超时未发送心跳的设备
     *
     * 遍历 m_lastSeen，将超时（10 秒未更新）的设备从 m_deviceList 中删除，
     * 如果列表有变化则发射 deviceListChanged() 信号。
     */
    void refreshDeviceList();
    void joinMulticastInterfaces();
    void leaveMulticastInterfaces();
};

#endif // DISCOVERYSERVICE_H
