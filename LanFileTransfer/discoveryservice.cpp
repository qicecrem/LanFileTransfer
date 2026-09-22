#include "discoveryservice.h"
#include <QHostInfo>
#include <QNetworkInterface>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QDebug>
#include <QRandomGenerator>
#include <QSettings>
#include <utility>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QCoreApplication>
#include <QtCore/qcoreapplication_platform.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <QDir>
#include <QCoreApplication>
#endif


const quint16 DISCOVERY_PORT = 45454;   ///< UDP 广播端口，需与所有设备保持一致

DiscoveryService::DiscoveryService(QObject *parent)
    : QObject{parent}
{
    QSettings settings("Lantern Labs", "LanDrop");
    m_instanceId = settings.value("identity/instanceId").toString();
    if (m_instanceId.isEmpty()) {
        m_instanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue("identity/instanceId", m_instanceId);
    }


    int randomId = QRandomGenerator::global()->bounded(1000, 10000);
#ifdef Q_OS_ANDROID
    m_deviceName = QString("手机_%1").arg(randomId);
#else
    m_deviceName = QString("电脑_%1").arg(randomId);
#endif


    m_udpSocket = new QUdpSocket(this);
    m_timer = new QTimer(this);

    // 1. 绑定端口，必须使用 IPv4
    if (!m_udpSocket->bind(QHostAddress::AnyIPv4, DISCOVERY_PORT,
                           QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint))
        qWarning() << "UDP discovery bind failed:" << m_udpSocket->errorString();

    // 允许组播数据回环（允许自己发出的组播也能被本机的其他程序收到，便于本机多开调试）
    m_udpSocket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
    // 强制设置组播生命周期
    m_udpSocket->setSocketOption(QAbstractSocket::MulticastTtlOption, 1);

    connect(m_udpSocket, &QUdpSocket::readyRead, this, &DiscoveryService::processPendingDatagrams);
    connect(m_timer, &QTimer::timeout, this, &DiscoveryService::broadcastHeartbeat);
}

void DiscoveryService::joinMulticastInterfaces()
{
    if (!m_joinedInterfaces.isEmpty()) return;
    const QHostAddress multicastAddress(QStringLiteral("239.255.43.21"));
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || (flags & QNetworkInterface::IsLoopBack)
            || !(flags & QNetworkInterface::CanMulticast)) continue;
        bool hasIPv4 = false;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) { hasIPv4 = true; break; }
        }
        if (!hasIPv4) continue;
        if (m_udpSocket->joinMulticastGroup(multicastAddress, iface))
            m_joinedInterfaces.append(iface);
        else
            qWarning() << "Multicast join failed on" << iface.humanReadableName()
                       << m_udpSocket->errorString();
    }
}

void DiscoveryService::leaveMulticastInterfaces()
{
    const QHostAddress multicastAddress(QStringLiteral("239.255.43.21"));
    for (const QNetworkInterface &iface : std::as_const(m_joinedInterfaces))
        m_udpSocket->leaveMulticastGroup(multicastAddress, iface);
    m_joinedInterfaces.clear();
    m_udpSocket->setMulticastInterface(QNetworkInterface());
}


DiscoveryService::~DiscoveryService()
{
    // 确保在对象销毁前释放 Android 组播锁
    stopScan();
}


void DiscoveryService::setDeviceName(const QString &name)
{
    // 如果名称未改变或是纯空白，则不作处理
    if (m_deviceName == name || name.trimmed().isEmpty())
        return;

    m_deviceName = name.trimmed();
    emit deviceNameChanged();

    qDebug() << "设备名称已更改为:" << m_deviceName;

    // 修改名字后立即广播一次，让局域网其他设备刷新列表
    if (m_timer->isActive()) {
        broadcastHeartbeat();
    }
}



void DiscoveryService::startScan()
{
    // 防止重复启动
    if (m_timer->isActive()) {
        qDebug() << "扫描已在运行中，忽略重复 startScan 调用";
        return;
    }

    qDebug() << "开始 UDP 局域网扫描，广播端口:" << DISCOVERY_PORT;

#ifdef Q_OS_ANDROID
    // Android 平台需要 MulticastLock 才能稳定接收组播。锁由 Java
    // 服务集中管理，避免扫描器和前台服务重复创建底层锁对象。
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (context.isValid()) {
        m_multicastAcquired = QJniObject::callStaticMethod<jboolean>(
            "org/landrop/app/LanTransferService", "acquireMulticast",
            "(Landroid/content/Context;)Z", context.object());
        if (m_multicastAcquired) qDebug() << "Android 共享组播锁已获取";
        else qWarning() << "获取 Android 共享组播锁失败";
    } else {
        qWarning() << "获取 Android Activity 失败";
    }
#endif

    // Android 必须先持有 MulticastLock，再加入组播组。
    joinMulticastInterfaces();

    // 立即发送一次广播，快速发现设备
    broadcastHeartbeat();
    // 启动定时器，每 3 秒发送一次心跳
    m_timer->start(3000);
}

void DiscoveryService::refreshNetwork()
{
    if (!m_timer->isActive()) {
        startScan();
        return;
    }
    qInfo() << "Network environment changed; rebinding discovery interfaces";
    leaveMulticastInterfaces();
    m_lastSeen.clear();
    m_peers.clear();
    if (!m_deviceList.isEmpty()) {
        m_deviceList.clear();
        emit deviceListChanged();
    }
    joinMulticastInterfaces();
    broadcastHeartbeat();
}

void DiscoveryService::stopScan()
{
    if (m_timer->isActive()) m_timer->stop();
    qDebug() << "停止 UDP 扫描，清空设备列表";

    leaveMulticastInterfaces();

    // 清空所有设备记录
    m_lastSeen.clear();
    m_peers.clear();
    m_deviceList.clear();
    emit deviceListChanged();

#ifdef Q_OS_ANDROID
    if (m_multicastAcquired) {
        QJniObject::callStaticMethod<void>("org/landrop/app/LanTransferService",
                                           "releaseMulticast", "()V");
        m_multicastAcquired = false;
        qDebug() << "Android 共享组播锁使用权已释放";
    }
#endif
}

void DiscoveryService::broadcastHeartbeat()
{
    QJsonObject json;
    json["name"] = m_deviceName;
    json["uid"] = m_instanceId;
    json["tcpPort"] = m_localTcpPort;
    json["mediaPort"] = m_localMediaPort;

    QJsonDocument doc(json);
    QByteArray datagram = doc.toJson(QJsonDocument::Compact);

    QHostAddress multicastAddress("239.255.43.21");

    bool sent = false;
    for (const QNetworkInterface &iface : std::as_const(m_joinedInterfaces)) {
        m_udpSocket->setMulticastInterface(iface);
        if (m_udpSocket->writeDatagram(datagram, multicastAddress, DISCOVERY_PORT) >= 0) sent = true;
    }
    if (m_joinedInterfaces.isEmpty())
        sent = m_udpSocket->writeDatagram(datagram, multicastAddress, DISCOVERY_PORT) >= 0;
    m_udpSocket->setMulticastInterface(QNetworkInterface());
    if (!sent) qWarning() << "发送 UDP 组播失败:" << m_udpSocket->errorString();

    refreshDeviceList();
}

void DiscoveryService::processPendingDatagrams()
{
    while (m_udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        const qint64 pendingSize = m_udpSocket->pendingDatagramSize();
        if (pendingSize <= 0 || pendingSize > 4096) { m_udpSocket->readDatagram(nullptr, 0); continue; }
        datagram.resize(pendingSize);
        QHostAddress senderIp;
        quint16 senderPort;  // 虽然不需要，但 readDatagram 可以接收

        qint64 bytesRead = m_udpSocket->readDatagram(datagram.data(), datagram.size(),
                                                     &senderIp, &senderPort);
        if (bytesRead == -1) {
            qWarning() << "读取 UDP 数据报失败:" << m_udpSocket->errorString();
            continue;
        }

        // 解析 JSON
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(datagram, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            qWarning() << "无效的 JSON 数据:" << parseError.errorString();
            continue;
        }

        if (!doc.isObject()) {
            qWarning() << "收到非对象格式的 JSON";
            continue;
        }

        QJsonObject obj = doc.object();
        QString uid = obj["uid"].toString();
        // 忽略自己发出的广播
        if (uid.isEmpty() || uid == m_instanceId) {
            continue;
        }

        QString deviceName = obj["name"].toString();
        int tcpPort = obj["tcpPort"].toInt();
        const int mediaPort = obj["mediaPort"].toInt();
        if (deviceName.isEmpty() || deviceName.size() > 80 || tcpPort < 1 || tcpPort > 65535
            || mediaPort < 0 || mediaPort > 65535) {
            qDebug() << "收到不完整的设备信息（缺少 name 或 tcpPort）";
            continue;
        }

        // 处理 IPv6 映射地址，保留纯 IPv4 格式
        if (senderIp.protocol() == QAbstractSocket::IPv4Protocol) {
            senderIp = QHostAddress(senderIp.toIPv4Address());
        }
        QString ipStr = senderIp.toString();

        QVariantMap device;
        device["id"] = uid;
        device["ip"] = ipStr;
        device["name"] = deviceName;
        device["port"] = tcpPort;
        device["mediaPort"] = mediaPort;
        m_peers[uid] = device;
        m_lastSeen[uid] = QDateTime::currentMSecsSinceEpoch();

        // 实时刷新设备列表（可选，也可依赖定时刷新，但此处立即刷新可更快响应）
        refreshDeviceList();
    }
}

void DiscoveryService::refreshDeviceList()
{
    QVariantList newList;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 TIMEOUT_MS = 10000;  // 10 秒未收到心跳即视为离线

    QMutableMapIterator<QString, qint64> it(m_lastSeen);
    while (it.hasNext()) {
        it.next();
        if (now - it.value() < TIMEOUT_MS) {
            if (m_peers.contains(it.key())) newList.append(m_peers.value(it.key()));
        } else {
            m_peers.remove(it.key());
            it.remove();
        }
    }

    if (m_deviceList != newList) {
        m_deviceList = newList;
        emit deviceListChanged();
        qDebug() << "设备列表已更新，当前在线设备数:" << m_deviceList.size();
    }
}

bool DiscoveryService::fixWindowsFirewall()
{
#ifdef Q_OS_WIN
    // 获取当前 .exe 的绝对路径，并将 '/' 替换为 Windows 支持的 '\'
    QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());

    // 构建 netsh 命令参数，允许所有入站连接
    QString params = QString("advfirewall firewall add rule name=\"LanFileTransfer\" dir=in action=allow program=\"%1\" enable=yes profile=any")
                         .arg(exePath);

    // 转换编码为 Windows API 支持的格式
    std::wstring wParams = params.toStdWString();

    // 呼出 UAC 盾牌管理员提权窗口，静默执行
    HINSTANCE result = ShellExecuteW(nullptr,
                                     L"runas",
                                     L"netsh.exe",
                                     wParams.c_str(),
                                     nullptr,
                                     SW_HIDE);

    // ShellExecute 返回值大于 32 表示执行成功
    if (reinterpret_cast<INT_PTR>(result) > 32) {
        qDebug() << "防火墙规则添加成功！";
        return true;
    } else {
        qWarning() << "防火墙规则添加失败或用户取消了授权。";
        return false;
    }
#else
    qDebug() << "当前不是 Windows 系统，无需修复防火墙。";
    return true; // 非 Windows 系统默认放行
#endif
}
