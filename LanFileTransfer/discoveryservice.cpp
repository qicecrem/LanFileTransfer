#include "discoveryservice.h"
#include <QHostInfo>
#include <QNetworkInterface>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QDebug>
#include <QRandomGenerator>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QCoreApplication>
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
    m_instanceId = QUuid::createUuid().toString();


    int randomId = QRandomGenerator::global()->bounded(1000, 10000);
#ifdef Q_OS_ANDROID
    m_deviceName = QString("手机_%1").arg(randomId);
#else
    m_deviceName = QString("电脑_%1").arg(randomId);
#endif


    m_udpSocket = new QUdpSocket(this);
    m_timer = new QTimer(this);

    // 1. 绑定端口，必须使用 IPv4
    m_udpSocket->bind(QHostAddress::AnyIPv4, DISCOVERY_PORT,
                      QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);

    QHostAddress multicastAddress("239.255.43.21");

    // 2. 【核心修复】：遍历所有网卡，让所有处于活跃状态的网卡都加入组播！
    QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        // 只筛选出：正在运行的 (IsUp)、非本机环回的 (!IsLoopBack)、且支持组播的 (CanMulticast) 网卡
        if ((iface.flags() & QNetworkInterface::IsUp) &&
            !(iface.flags() & QNetworkInterface::IsLoopBack) &&
            (iface.flags() & QNetworkInterface::CanMulticast)) {

            // 确保网卡有 IPv4 地址
            bool hasIPv4 = false;
            for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    hasIPv4 = true;
                    break;
                }
            }

            if (hasIPv4) {
                // 针对该特定网卡加入组播组
                if (!m_udpSocket->joinMulticastGroup(multicastAddress, iface)) {
                    qWarning() << "网卡 [" << iface.humanReadableName() << "] 加入组播失败:" << m_udpSocket->errorString();
                } else {
                    qDebug() << "网卡 [" << iface.humanReadableName() << "] 成功加入组播";
                }
            }
        }
    }

    // 允许组播数据回环（允许自己发出的组播也能被本机的其他程序收到，便于本机多开调试）
    m_udpSocket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
    // 强制设置组播生命周期
    m_udpSocket->setSocketOption(QAbstractSocket::MulticastTtlOption, 1);

    connect(m_udpSocket, &QUdpSocket::readyRead, this, &DiscoveryService::processPendingDatagrams);
    connect(m_timer, &QTimer::timeout, this, &DiscoveryService::broadcastHeartbeat);
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
    // Android 平台需要获取 MulticastLock 才能接收广播包
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (context.isValid()) {
        QJniObject serviceName = QJniObject::fromString("wifi");
        QJniObject wifiManager = context.callMethod<QJniObject>(
            "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", serviceName.object());

        if (wifiManager.isValid()) {
            QJniObject lock = wifiManager.callMethod<QJniObject>(
                "createMulticastLock", "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;",
                QJniObject::fromString("LanFileTransferLock").object());
            if (lock.isValid()) {
                m_multicastLock = lock;          // 保存锁对象供后续释放
                m_multicastLock.callMethod<void>("acquire");
                qDebug() << "Android 组播锁已获取";
            } else {
                qWarning() << "创建 Android 组播锁失败";
            }
        } else {
            qWarning() << "获取 Android WifiManager 失败";
        }
    } else {
        qWarning() << "获取 Android Activity 失败";
    }
#endif

    // 立即发送一次广播，快速发现设备
    broadcastHeartbeat();
    // 启动定时器，每 3 秒发送一次心跳
    m_timer->start(3000);
}

void DiscoveryService::stopScan()
{
    if (!m_timer->isActive()) {
        qDebug() << "扫描未运行，stopScan 无操作";
        return;
    }

    m_timer->stop();
    qDebug() << "停止 UDP 扫描，清空设备列表";

    // 清空所有设备记录
    m_lastSeen.clear();
    m_deviceList.clear();
    emit deviceListChanged();

#ifdef Q_OS_ANDROID
    if (m_multicastLock.isValid()) {
        // 使用 QNativeInterface 判断 JNI 环境是否还活着
        if (QNativeInterface::QAndroidApplication::context().isValid()) {
            m_multicastLock.callMethod<void>("release");
            qDebug() << "Android 组播锁已释放";
        }
        m_multicastLock = nullptr; // 置空
    }
#endif
}

void DiscoveryService::broadcastHeartbeat()
{
    QJsonObject json;
    json["name"] = m_deviceName;
    json["uid"] = m_instanceId;
    json["tcpPort"] = m_localTcpPort;

    QJsonDocument doc(json);
    QByteArray datagram = doc.toJson(QJsonDocument::Compact);

    QHostAddress multicastAddress("239.255.43.21");

    // 【核心修复】：将组播包通过系统的默认路由发出去
    // 为了防止部分系统路由错误，这里直接发出去，通常上一步的多网卡 join 已经解决了 90% 的问题
    qint64 bytesSent = m_udpSocket->writeDatagram(datagram, multicastAddress, DISCOVERY_PORT);

    if (bytesSent == -1) {
        qWarning() << "发送 UDP 组播失败:" << m_udpSocket->errorString();
    }

    refreshDeviceList();
}

void DiscoveryService::processPendingDatagrams()
{
    while (m_udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_udpSocket->pendingDatagramSize());
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
        if (uid == m_instanceId) {
            continue;
        }

        QString deviceName = obj["name"].toString();
        int tcpPort = obj["tcpPort"].toInt();
        if (deviceName.isEmpty() || tcpPort == 0) {
            qDebug() << "收到不完整的设备信息（缺少 name 或 tcpPort）";
            continue;
        }

        // 处理 IPv6 映射地址，保留纯 IPv4 格式
        if (senderIp.protocol() == QAbstractSocket::IPv4Protocol) {
            senderIp = QHostAddress(senderIp.toIPv4Address());
        }
        QString ipStr = senderIp.toString();

        // 使用 UUID + IP + Name + Port 作为唯一键，防止同 IP 多设备互相覆盖
        // 格式: UUID|IP|Name|Port
        QString key = QString("%1|%2|%3|%4").arg(uid, ipStr, deviceName).arg(tcpPort);
        m_lastSeen[key] = QDateTime::currentMSecsSinceEpoch();

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
            QStringList parts = it.key().split('|');
            if (parts.size() == 4) {
                QVariantMap device;
                device["ip"]   = parts[1];
                device["name"] = parts[2];
                device["port"] = parts[3].toInt();
                newList.append(device);
            }
        } else {
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
