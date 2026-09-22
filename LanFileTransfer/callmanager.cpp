#include "callmanager.h"

#include <QBuffer>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QCamera>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QScreenCapture>
#include <QGuiApplication>
#include <QImageReader>
#include <QScreen>
#include <QPointer>
#include <QLoggingCategory>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVideoFrame>
#include <QVideoSink>
#include <QtEndian>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

Q_LOGGING_CATEGORY(lcCall, "landrop.call")

namespace {
constexpr int MediaHandshakeTimeoutMs = 10000;
constexpr int CallResponseTimeoutMs = 45000;
constexpr int MediaHeartbeatIntervalMs = 5000;
constexpr int MediaIdleTimeoutMs = 20000;
constexpr int MaxRemoteFrameDimension = 4096;
constexpr qint64 MaxRemoteFramePixels = 16 * 1024 * 1024;
constexpr quint8 MediaProtocolVersion = 2;
constexpr int MediaAuthorizationKeySize = 32;
constexpr int MediaChallengeSize = 32;
constexpr int MediaProofSize = 32;

QByteArray randomAuthenticationBytes(int size)
{
    QByteArray result(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        result[i] = char(QRandomGenerator::system()->generate() & 0xff);
    return result;
}

bool constantTimeEqual(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left.at(i) ^ right.at(i));
    return difference == 0;
}

QString normalizedIp(QString ip)
{
    if (ip.startsWith(QStringLiteral("::ffff:"))) ip.remove(0, 7);
    return ip;
}

#ifdef Q_OS_ANDROID
QPointer<CallManager> androidCallManager;

bool hasAndroidPermission(const char *permission)
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject name = QJniObject::fromString(QString::fromLatin1(permission));
    return context.isValid() && context.callMethod<jint>(
        "checkSelfPermission", "(Ljava/lang/String;)I", name.object()) == 0;
}
#endif
}

CallFrameProvider *CallManager::s_provider = nullptr;

CallFrameProvider::CallFrameProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
QImage CallFrameProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    QMutexLocker locker(&m_mutex);
    QImage image = id.startsWith("local") ? m_local : m_remote;
    if (image.isNull()) { image=QImage(16,9,QImage::Format_RGB32); image.fill(QColor("#0b1720")); }
    if (size) *size=image.size();
    return requestedSize.isValid() ? image.scaled(requestedSize,Qt::KeepAspectRatio,Qt::SmoothTransformation) : image;
}
void CallFrameProvider::setFrame(const QString &channel,const QImage &image)
{
    QMutexLocker locker(&m_mutex);
    if(channel=="local") m_local=image; else m_remote=image;
}

void CallManager::setFrameProvider(CallFrameProvider *provider) { s_provider=provider; }

CallManager::CallManager(QObject *parent) : QObject(parent)
{
#ifdef Q_OS_ANDROID
    androidCallManager = this;
#endif
    qCInfo(lcCall) << "CallManager constructing";
    m_server=new QTcpServer(this);
    if(!m_server->listen(QHostAddress::AnyIPv4,0)){qCCritical(lcCall)<<"Media server listen failed"<<m_server->errorString();emit callError(m_server->errorString());}
    else qCInfo(lcCall)<<"Media server listening on"<<m_server->serverPort();
    connect(m_server,&QTcpServer::newConnection,this,[this]{
        auto *next=m_server->nextPendingConnection();
        const QString peerIp=normalizedIp(next->peerAddress().toString());
        qCInfo(lcCall)<<"Incoming media socket from"<<peerIp;
        bool addressAllowed=false;
        for(const AuthorizedPeer &peer : std::as_const(m_allowedPeers)){
            if(normalizedIp(peer.ip)==peerIp){addressAllowed=true;break;}
        }
        if(!addressAllowed){qCWarning(lcCall)<<"Rejected unpaired media peer"<<peerIp;next->disconnectFromHost();next->deleteLater();return;}
        if(m_socket) { next->disconnectFromHost(); next->deleteLater(); return; }
        attachSocket(next,false);
    });
    m_capture=new QMediaCaptureSession(this);
    m_sink=new QVideoSink(this);
    m_capture->setVideoSink(m_sink);
    connect(m_sink,&QVideoSink::videoFrameChanged,this,[this](const QVideoFrame &frame){
        const qint64 now=QDateTime::currentMSecsSinceEpoch();
        if(now-m_lastFrameAt<100) return; // cap LAN preview at 10 fps
        QImage image=frame.toImage();
        if(image.isNull()){qCWarning(lcCall)<<"Video frame conversion returned a null image";return;}
        image=image.scaled(960,540,Qt::KeepAspectRatio,Qt::SmoothTransformation);
        if(!m_loggedFirstLocalFrame){m_loggedFirstLocalFrame=true;qCInfo(lcCall)<<"First local frame"<<image.size()<<"format"<<image.format();}
        ++m_localFrameCount;
        if (m_localFrameCount % 100 == 0)
            qCInfo(lcCall) << "Local frame heartbeat" << m_localFrameCount << image.size();
        if(s_provider) s_provider->setFrame("local",image);
        ++m_localRevision; emit localFrameChanged();
        if(!m_videoMuted && m_socket && m_state=="connected" && m_socket->bytesToWrite()<2*1024*1024) {
            QByteArray jpg; QBuffer buffer(&jpg); buffer.open(QIODevice::WriteOnly); image.save(&buffer,"JPG",60);
            sendPacket(VideoFrame,jpg);
        }
        m_lastFrameAt=now;
    });
    auto *healthTimer = new QTimer(this);
    healthTimer->setInterval(MediaHeartbeatIntervalMs);
    connect(healthTimer, &QTimer::timeout, this, &CallManager::checkConnectionHealth);
    healthTimer->start();
}

CallManager::~CallManager()
{
    qCInfo(lcCall)<<"CallManager destroying";
#ifdef Q_OS_ANDROID
    if(androidCallManager==this)androidCallManager.clear();
#endif
    stopCapture();
    clearActiveAuthentication();
    for(AuthorizedPeer &peer : m_allowedPeers)peer.key.fill('\0');
    m_allowedPeers.clear();
}
quint16 CallManager::serverPort() const { return m_server->serverPort(); }
bool CallManager::cameraPermissionGranted() const
{
#ifdef Q_OS_ANDROID
    return hasAndroidPermission("android.permission.CAMERA");
#else
    return true;
#endif
}
bool CallManager::microphonePermissionGranted() const
{
#ifdef Q_OS_ANDROID
    return hasAndroidPermission("android.permission.RECORD_AUDIO");
#else
    return true;
#endif
}
void CallManager::setState(const QString &s) { if(s==m_state)return;qCInfo(lcCall)<<"Call state"<<m_state<<"->"<<s;m_state=s; emit stateChanged(); }
void CallManager::setAudioMuted(bool muted) { if(m_audioMuted==muted)return; m_audioMuted=muted; emit controlsChanged(); if(m_state=="connected")startCapture(); }
void CallManager::setVideoMuted(bool muted) { if(m_videoMuted==muted)return; m_videoMuted=muted; emit controlsChanged(); if(m_state=="connected")startCapture(); }
void CallManager::allowPeer(const QString &peerId,const QString &ip,const QString &mediaToken)
{
    const QByteArray key=QByteArray::fromBase64(mediaToken.toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
    if(peerId.isEmpty()||ip.isEmpty()||key.size()!=MediaAuthorizationKeySize){
        qCWarning(lcCall)<<"Ignored invalid media authorization for peer"<<peerId;
        return;
    }
    AuthorizedPeer peer;
    peer.ip=normalizedIp(ip);
    peer.key=key;
    auto existing=m_allowedPeers.find(peerId);
    if(existing!=m_allowedPeers.end())existing->key.fill('\0');
    m_allowedPeers.insert(peerId,peer);
    qCInfo(lcCall)<<"Allowed authenticated media peer"<<peerId<<peer.ip;
}

void CallManager::revokePeer(const QString &peerId,const QString &ip)
{
    auto it=m_allowedPeers.find(peerId);
    if(it!=m_allowedPeers.end()){
        it->key.fill('\0');
        m_allowedPeers.erase(it);
    }
    qCInfo(lcCall)<<"Revoked media peer"<<peerId<<ip;
    if((!peerId.isEmpty()&&m_peerId==peerId)
        ||(peerId.isEmpty()&&!ip.isEmpty()&&m_peerIp==normalizedIp(ip)))hangup();
}
void CallManager::requestMediaPermissions()
{
#ifdef Q_OS_ANDROID
    const QJniObject activity=QNativeInterface::QAndroidApplication::context();
    if(activity.isValid())activity.callMethod<void>("requestMediaPermissions","()V");
#endif
}
void CallManager::openApplicationSettings()
{
#ifdef Q_OS_ANDROID
    const QJniObject activity=QNativeInterface::QAndroidApplication::context();
    if(activity.isValid())activity.callMethod<void>("openApplicationSettings","()V");
#endif
}
void CallManager::refreshPermissions() { emit permissionsChanged(); }

bool CallManager::ensurePermissionsForMode(const QString &mode)
{
#ifdef Q_OS_ANDROID
    const bool needsCamera=mode!="screen"&&!m_videoMuted&&!cameraPermissionGranted();
    const bool needsMicrophone=!m_audioMuted&&!microphonePermissionGranted();
    if(needsCamera||needsMicrophone){
        qCWarning(lcCall)<<"Media permissions missing camera"<<needsCamera
                         <<"microphone"<<needsMicrophone;
        requestMediaPermissions();
        emit callError(tr("请授权摄像头和麦克风后重试"));
        return false;
    }
#else
    Q_UNUSED(mode);
#endif
    return true;
}

QByteArray CallManager::authenticationProof(const QByteArray &key,const QByteArray &label,
                                            const QByteArray &challenge,const QString &mode)
{
    QByteArray signedData;
    QDataStream stream(&signedData,QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    stream<<label<<challenge<<mode;
    return QMessageAuthenticationCode::hash(signedData,key,QCryptographicHash::Sha256);
}

QByteArray CallManager::replyPayload(PacketType type) const
{
    const QByteArray label=type==Accept ? QByteArrayLiteral("landrop-media-accept-v2")
                                        : QByteArrayLiteral("landrop-media-reject-v2");
    QByteArray payload;
    QDataStream stream(&payload,QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    stream<<MediaProtocolVersion
          <<authenticationProof(m_activeAuthKey,label,m_authChallenge,m_mode);
    return payload;
}

bool CallManager::verifyReply(PacketType type,const QByteArray &payload) const
{
    quint8 version=0;
    QByteArray proof;
    QDataStream stream(payload);
    stream.setVersion(QDataStream::Qt_6_5);
    stream>>version>>proof;
    if(stream.status()!=QDataStream::Ok||!stream.atEnd()||version!=MediaProtocolVersion
        ||proof.size()!=MediaProofSize||m_activeAuthKey.size()!=MediaAuthorizationKeySize
        ||m_authChallenge.size()!=MediaChallengeSize)return false;
    const QByteArray label=type==Accept ? QByteArrayLiteral("landrop-media-accept-v2")
                                        : QByteArrayLiteral("landrop-media-reject-v2");
    return constantTimeEqual(proof,authenticationProof(
        m_activeAuthKey,label,m_authChallenge,m_mode));
}

void CallManager::clearActiveAuthentication()
{
    m_activeAuthKey.fill('\0');
    m_activeAuthKey.clear();
    m_authChallenge.fill('\0');
    m_authChallenge.clear();
    m_peerId.clear();
    m_handshakeStage=HandshakeStage::None;
}

void CallManager::attachSocket(QTcpSocket *socket,bool outgoing)
{
    qCInfo(lcCall)<<"Attaching media socket"<<socket<<"peer"<<socket->peerAddress().toString();
    m_socket=socket; m_buffer.clear();
    m_lastInboundAt=QDateTime::currentMSecsSinceEpoch();
    if(outgoing)m_handshakeStage=HandshakeStage::AwaitingChallenge;
    else clearActiveAuthentication();
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    const QString connectedPeer=normalizedIp(socket->peerAddress().toString());
    if(!connectedPeer.isEmpty())m_peerIp=connectedPeer;
    const QPointer<QTcpSocket> guardedSocket(socket);
    connect(socket,&QTcpSocket::readyRead,this,[this,guardedSocket]{
        if(!guardedSocket||m_socket!=guardedSocket)return;
        m_buffer+=guardedSocket->readAll(); processPackets();
    });
    connect(socket,&QTcpSocket::disconnected,this,[this,socket]{
        qCInfo(lcCall)<<"Media socket disconnected"<<socket->errorString();
        if(m_socket==socket){m_socket=nullptr;m_buffer.clear();m_peerIp.clear();m_lastInboundAt=0;clearActiveAuthentication();stopCapture();resetFrames();setState("idle");} socket->deleteLater();
    });
    connect(socket,&QTcpSocket::errorOccurred,this,[this,guardedSocket](QAbstractSocket::SocketError){
        if(guardedSocket&&m_socket==guardedSocket){
            const QString message=guardedSocket->errorString();
            qCWarning(lcCall)<<"Media socket error"<<message;
            emit callError(message);
            endCall(false);
        }
    });
    QTimer::singleShot(MediaHandshakeTimeoutMs,socket,[this,guardedSocket]{
        if(guardedSocket&&m_socket==guardedSocket&&m_state=="idle"){
            qCWarning(lcCall)<<"Incoming media socket timed out before invite";
            emit callError("Media handshake timed out");
            endCall(false);
        }
    });
}

void CallManager::startCall(const QString &peerId,const QString &ip,quint16 port,const QString &mode)
{
    qCInfo(lcCall)<<"startCall"<<peerId<<ip<<port<<mode;
    if(m_socket || peerId.isEmpty() || ip.isEmpty() || port==0) return;
    const auto authorized=m_allowedPeers.constFind(peerId);
    if(authorized==m_allowedPeers.cend()||normalizedIp(authorized->ip)!=normalizedIp(ip)
        ||authorized->key.size()!=MediaAuthorizationKeySize){
        qCWarning(lcCall)<<"Call rejected because peer is not authenticated"<<peerId<<ip;
        emit callError("Pair with this device before starting a call");
        return;
    }
    if(!ensurePermissionsForMode(mode))return;
    m_peerId=peerId;
    m_peerIp=normalizedIp(ip);
    m_activeAuthKey=authorized->key;
    m_mode=mode=="screen"?"screen":"camera"; emit stateChanged();
    auto *socket=new QTcpSocket(this); attachSocket(socket,true); setState("calling");
    const QPointer<QTcpSocket> connectedSocket=socket;
    connect(socket,&QTcpSocket::connected,this,[this,connectedSocket]{
        if(!connectedSocket||m_socket!=connectedSocket)return;
        QByteArray payload;
        QDataStream stream(&payload,QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_5);
        stream<<MediaProtocolVersion;
        sendPacket(AuthHello,payload);
    });
    const QPointer<QTcpSocket> guardedSocket=socket;
    QTimer::singleShot(CallResponseTimeoutMs,socket,[this,guardedSocket]{
        if(guardedSocket&&m_socket==guardedSocket&&m_state=="calling"){
            qCWarning(lcCall)<<"Outgoing call timed out while waiting for an answer";
            emit callError("Call invitation timed out");
            endCall(false);
        }
    });
    socket->connectToHost(ip,port);
}

void CallManager::acceptCall() { qCInfo(lcCall)<<"acceptCall state"<<m_state<<"mode"<<m_mode;if(!m_socket||m_state!="incoming"||m_handshakeStage!=HandshakeStage::AwaitingDecision)return;if(!ensurePermissionsForMode(m_mode)){sendPacket(Reject,replyPayload(Reject));endCall(false);return;}sendPacket(Accept,replyPayload(Accept));m_handshakeStage=HandshakeStage::Authenticated;setState("connected");startCapture(); }
void CallManager::rejectCall() { qCInfo(lcCall)<<"rejectCall";if(m_socket&&m_handshakeStage==HandshakeStage::AwaitingDecision)sendPacket(Reject,replyPayload(Reject));endCall(false); }
void CallManager::hangup() { qCInfo(lcCall)<<"hangup";endCall(m_state=="connected"&&m_handshakeStage==HandshakeStage::Authenticated); }
void CallManager::switchMode(const QString &mode) { if(m_state!="connected")return;qCInfo(lcCall)<<"switchMode"<<mode;m_mode=mode=="screen"?"screen":"camera";sendPacket(SwitchMode,m_mode.toUtf8());startCapture();emit stateChanged(); }
void CallManager::startLocalDiagnostic(const QString &mode)
{
    qCInfo(lcCall) << "startLocalDiagnostic" << mode;
    if (m_socket)
        hangup();
    if(!ensurePermissionsForMode(mode))return;
    m_mode = mode == "screen" ? "screen" : "camera";
    emit stateChanged();
    setState("connected");
    startCapture();
}

void CallManager::sendPacket(PacketType type,const QByteArray &payload)
{
    if(!m_socket||payload.size()>16*1024*1024)return;
    QByteArray packet; packet.append(char(type));packet.append(payload);
    quint32 n=qToBigEndian<quint32>(packet.size());
    m_socket->write(reinterpret_cast<const char*>(&n),4);m_socket->write(packet);
}

void CallManager::endCall(bool notifyPeer)
{
    QPointer<QTcpSocket> socket=m_socket;
    if(notifyPeer&&socket&&socket->state()==QAbstractSocket::ConnectedState)
        sendPacket(Hangup);
    m_socket=nullptr;
    m_buffer.clear();
    m_peerIp.clear();
    m_lastInboundAt=0;
    clearActiveAuthentication();
    stopCapture();
    resetFrames();
    setState("idle");
    if(socket){
        socket->disconnectFromHost();
        if(socket->state()==QAbstractSocket::UnconnectedState)socket->deleteLater();
    }
}

void CallManager::checkConnectionHealth()
{
    if (!m_socket || m_state != QStringLiteral("connected")
        || m_socket->state() != QAbstractSocket::ConnectedState) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastInboundAt > 0 && now - m_lastInboundAt > MediaIdleTimeoutMs) {
        qCWarning(lcCall) << "Media connection timed out after"
                          << now - m_lastInboundAt << "ms without inbound data";
        emit callError(QStringLiteral("Media connection timed out"));
        endCall(false);
        return;
    }
    // An empty audio frame is understood by existing peers, so it doubles as
    // a backwards-compatible heartbeat even when camera and microphone are muted.
    if (m_socket->bytesToWrite() < 2 * 1024 * 1024)
        sendPacket(AudioFrame, {});
}

void CallManager::resetFrames()
{
    if(s_provider){
        s_provider->setFrame("local",{});
        s_provider->setFrame("remote",{});
    }
    ++m_localRevision;
    ++m_remoteRevision;
    emit localFrameChanged();
    emit remoteFrameChanged();
}

void CallManager::processPackets()
{
    while(m_buffer.size()>=4){
        quint32 n=qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(m_buffer.constData()));
        if(n<1||n>16*1024*1024){emit callError("Invalid media packet");hangup();return;}
        if(m_buffer.size()<int(n+4))return;
        QByteArray packet=m_buffer.mid(4,n);m_buffer.remove(0,n+4);
        const auto type=PacketType(quint8(packet[0]));const QByteArray payload=packet.mid(1);
        m_lastInboundAt=QDateTime::currentMSecsSinceEpoch();
        if(type==AuthHello){
            quint8 version=0;
            QDataStream stream(payload);
            stream.setVersion(QDataStream::Qt_6_5);
            stream>>version;
            if(m_state!="idle"||m_handshakeStage!=HandshakeStage::None
                ||stream.status()!=QDataStream::Ok||!stream.atEnd()
                ||version!=MediaProtocolVersion){
                qCWarning(lcCall)<<"Rejected invalid media authentication hello";
                emit callError("Media authentication failed");
                endCall(false);
                return;
            }
            m_authChallenge=randomAuthenticationBytes(MediaChallengeSize);
            m_handshakeStage=HandshakeStage::AwaitingInvite;
            QByteArray response;
            QDataStream output(&response,QIODevice::WriteOnly);
            output.setVersion(QDataStream::Qt_6_5);
            output<<MediaProtocolVersion<<m_authChallenge;
            sendPacket(AuthChallenge,response);
        }
        else if(type==AuthChallenge){
            quint8 version=0;
            QByteArray challenge;
            QDataStream stream(payload);
            stream.setVersion(QDataStream::Qt_6_5);
            stream>>version>>challenge;
            if(m_state!="calling"||m_handshakeStage!=HandshakeStage::AwaitingChallenge
                ||stream.status()!=QDataStream::Ok||!stream.atEnd()
                ||version!=MediaProtocolVersion||challenge.size()!=MediaChallengeSize
                ||m_activeAuthKey.size()!=MediaAuthorizationKeySize){
                qCWarning(lcCall)<<"Rejected invalid media authentication challenge";
                emit callError("Media authentication failed");
                endCall(false);
                return;
            }
            m_authChallenge=challenge;
            const QByteArray proof=authenticationProof(
                m_activeAuthKey,QByteArrayLiteral("landrop-media-invite-v2"),
                m_authChallenge,m_mode);
            QByteArray invitation;
            QDataStream output(&invitation,QIODevice::WriteOnly);
            output.setVersion(QDataStream::Qt_6_5);
            output<<MediaProtocolVersion<<m_mode<<proof;
            m_handshakeStage=HandshakeStage::AwaitingResponse;
            sendPacket(Invite,invitation);
        }
        else if(type==Invite){
            quint8 version=0;
            QString requestedMode;
            QByteArray proof;
            QDataStream stream(payload);
            stream.setVersion(QDataStream::Qt_6_5);
            stream>>version>>requestedMode>>proof;
            if(m_state!="idle"||m_handshakeStage!=HandshakeStage::AwaitingInvite
                ||stream.status()!=QDataStream::Ok||!stream.atEnd()
                ||version!=MediaProtocolVersion
                ||(requestedMode!="camera"&&requestedMode!="screen")
                ||proof.size()!=MediaProofSize||m_authChallenge.size()!=MediaChallengeSize){
                qCWarning(lcCall)<<"Rejected malformed media invitation";
                emit callError("Media authentication failed");
                endCall(false);
                return;
            }
            QString matchedPeerId;
            QByteArray matchedKey;
            for(auto it=m_allowedPeers.cbegin();it!=m_allowedPeers.cend();++it){
                if(normalizedIp(it->ip)!=m_peerIp||it->key.size()!=MediaAuthorizationKeySize)
                    continue;
                const QByteArray expected=authenticationProof(
                    it->key,QByteArrayLiteral("landrop-media-invite-v2"),
                    m_authChallenge,requestedMode);
                if(constantTimeEqual(proof,expected)){
                    matchedPeerId=it.key();
                    matchedKey=it->key;
                    break;
                }
            }
            if(matchedPeerId.isEmpty()){
                qCWarning(lcCall)<<"Rejected media invitation with invalid proof from"<<m_peerIp;
                emit callError("Media authentication failed");
                endCall(false);
                return;
            }
            m_peerId=matchedPeerId;
            m_activeAuthKey=matchedKey;
            m_mode=requestedMode;
            m_handshakeStage=HandshakeStage::AwaitingDecision;
            qCInfo(lcCall)<<"Authenticated media invite from"<<m_peerId<<m_peerIp
                          <<"mode"<<m_mode;
            setState("incoming");
            emit incomingCall(m_peerIp,m_mode);
            const QPointer<QTcpSocket> pendingSocket=m_socket;
            QTimer::singleShot(CallResponseTimeoutMs,m_socket,[this,pendingSocket]{
                if(!pendingSocket||m_socket!=pendingSocket||m_state!="incoming"
                    ||m_handshakeStage!=HandshakeStage::AwaitingDecision)return;
                qCInfo(lcCall)<<"Incoming call expired without a response";
                sendPacket(Reject,replyPayload(Reject));
                endCall(false);
            });
        }
        else if(type==Accept){
            if(m_state!="calling"||m_handshakeStage!=HandshakeStage::AwaitingResponse
                ||!verifyReply(Accept,payload)){
                qCWarning(lcCall)<<"Rejected unauthenticated call acceptance";
                emit callError("Media authentication failed");
                endCall(false);
                return;
            }
            qCInfo(lcCall)<<"Authenticated call acceptance from"<<m_peerId;
            m_handshakeStage=HandshakeStage::Authenticated;
            setState("connected");
            startCapture();
        }
        else if(type==Reject){
            if(m_state!="calling"||m_handshakeStage!=HandshakeStage::AwaitingResponse
                ||!verifyReply(Reject,payload)){
                qCWarning(lcCall)<<"Rejected unauthenticated call rejection";
                emit callError("Media authentication failed");
            }
            endCall(false);
            return;
        }
        else if(type==Hangup){
            if(m_state!="connected"||m_handshakeStage!=HandshakeStage::Authenticated){
                emit callError("Invalid media packet");
            }
            endCall(false);
            return;
        }
        else if(type==SwitchMode){if(m_state!="connected"||m_handshakeStage!=HandshakeStage::Authenticated||(payload!="camera"&&payload!="screen")){emit callError("Invalid media mode change");endCall(false);return;}m_mode=payload=="screen"?"screen":"camera";qCInfo(lcCall)<<"Peer switched mode to"<<m_mode;emit stateChanged();}
        else if(type==VideoFrame){
            if(m_state!="connected"||m_handshakeStage!=HandshakeStage::Authenticated||payload.size()>4*1024*1024){qCWarning(lcCall)<<"Invalid remote video frame"<<payload.size();emit callError("Invalid video frame");endCall(false);return;}
            QBuffer source;source.setData(payload);source.open(QIODevice::ReadOnly);QImageReader reader(&source,"JPG");const QSize dimensions=reader.size();
            if(!dimensions.isValid()||dimensions.width()>MaxRemoteFrameDimension||dimensions.height()>MaxRemoteFrameDimension||qint64(dimensions.width())*dimensions.height()>MaxRemoteFramePixels){emit callError("Remote video dimensions are invalid");endCall(false);return;}
            QSize decodedSize=dimensions;decodedSize.scale(960,540,Qt::KeepAspectRatio);reader.setScaledSize(decodedSize);QImage image=reader.read();
            if(!image.isNull()){if(!m_loggedFirstRemoteFrame){m_loggedFirstRemoteFrame=true;qCInfo(lcCall)<<"First remote frame"<<image.size();}if(s_provider)s_provider->setFrame("remote",image);++m_remoteRevision;emit remoteFrameChanged();}
        }
        else if(type==AudioFrame){if(m_state!="connected"||m_handshakeStage!=HandshakeStage::Authenticated||payload.size()>256*1024){emit callError("Invalid audio frame");endCall(false);return;}if(m_audioOutput)m_audioOutput->write(payload);}
        else {emit callError("Unknown media packet");endCall(false);return;}
    }
}

void CallManager::startCapture()
{
    qCInfo(lcCall)<<"startCapture begin mode"<<m_mode<<"audioMuted"<<m_audioMuted<<"videoMuted"<<m_videoMuted;
    stopCapture();
    m_loggedFirstLocalFrame=false;
    m_loggedFirstRemoteFrame=false;
    m_localFrameCount=0;
    if(!m_videoMuted && m_mode=="screen"){
        QScreen *primary=QGuiApplication::primaryScreen();qCInfo(lcCall)<<"Creating QScreenCapture; primary screen"<<primary;
        if(!primary){qCWarning(lcCall)<<"No primary screen available";emit callError("No screen available");}
        else{m_screen=new QScreenCapture(this);const QPointer<QScreenCapture> screen=m_screen;connect(m_screen,&QScreenCapture::errorOccurred,this,[this,screen](QScreenCapture::Error error,const QString &message){if(screen&&m_screen==screen){qCCritical(lcCall)<<"Screen capture error"<<error<<message;emit callError(message);}});m_screen->setScreen(primary);m_capture->setScreenCapture(m_screen);m_screen->setActive(true);qCInfo(lcCall)<<"QScreenCapture activation requested; active="<<m_screen->isActive();}
    } else if(!m_videoMuted) {
#ifdef Q_OS_ANDROID
        if(!hasAndroidPermission("android.permission.CAMERA")){qCWarning(lcCall)<<"Camera permission is not granted";emit callError("Camera permission is required");}
        else
#endif
        {
        const auto videoInput=QMediaDevices::defaultVideoInput();
        qCInfo(lcCall)<<"Default video input"<<videoInput.description()<<videoInput.id();
        if(videoInput.isNull()) {
            qCWarning(lcCall)<<"No camera available";
            emit callError("No camera available");
        } else {
            m_camera=new QCamera(videoInput,this);const QPointer<QCamera> camera=m_camera;connect(m_camera,&QCamera::errorOccurred,this,[this,camera](QCamera::Error error,const QString &message){if(camera&&m_camera==camera){qCCritical(lcCall)<<"Camera error"<<error<<message;emit callError(message);}});m_capture->setCamera(m_camera);m_camera->start();qCInfo(lcCall)<<"Camera start requested; active="<<m_camera->isActive();
        }
        }
    }
    QAudioFormat audioFormat; audioFormat.setSampleRate(16000); audioFormat.setChannelCount(1); audioFormat.setSampleFormat(QAudioFormat::Int16);
    const auto inputDevice=QMediaDevices::defaultAudioInput();
    const auto outputDevice=QMediaDevices::defaultAudioOutput();
    qCInfo(lcCall)<<"Audio devices input"<<inputDevice.description()<<"output"<<outputDevice.description()
                  <<"inputFormatSupported"<<inputDevice.isFormatSupported(audioFormat)
                  <<"outputFormatSupported"<<outputDevice.isFormatSupported(audioFormat);
    bool inputPermission=true;
#ifdef Q_OS_ANDROID
    inputPermission=hasAndroidPermission("android.permission.RECORD_AUDIO");
    if(!m_audioMuted&&!inputPermission){qCWarning(lcCall)<<"Microphone permission is not granted";emit callError("Microphone permission is required");}
#endif
    if(!m_audioMuted&&inputPermission&&!inputDevice.isNull()&&inputDevice.isFormatSupported(audioFormat)){
        m_audioSource=new QAudioSource(inputDevice,audioFormat,this);m_audioInput=m_audioSource->start();
        if(m_audioInput){const QPointer<QIODevice> input=m_audioInput;connect(m_audioInput,&QIODevice::readyRead,this,[this,input]{if(!input||m_audioInput!=input)return;const QByteArray pcm=input->readAll();if(!m_audioMuted&&!pcm.isEmpty()&&m_state=="connected"&&m_socket&&m_socket->bytesToWrite()<2*1024*1024)sendPacket(AudioFrame,pcm);});}
    }
    if(!outputDevice.isNull()&&outputDevice.isFormatSupported(audioFormat)){m_audioSink=new QAudioSink(outputDevice,audioFormat,this);m_audioOutput=m_audioSink->start();}
    qCInfo(lcCall)<<"startCapture complete camera"<<m_camera<<"screen"<<m_screen
                  <<"audioInput"<<m_audioInput<<"audioOutput"<<m_audioOutput;
}
void CallManager::stopCapture()
{
    qCInfo(lcCall)<<"stopCapture begin camera"<<m_camera<<"screen"<<m_screen
                  <<"audioSource"<<m_audioSource<<"audioSink"<<m_audioSink;
    m_audioInput=nullptr;m_audioOutput=nullptr;
    if(m_audioSource){m_audioSource->stop();m_audioSource->deleteLater();m_audioSource=nullptr;}
    if(m_audioSink){m_audioSink->stop();m_audioSink->deleteLater();m_audioSink=nullptr;}
    if(m_camera){m_camera->stop();m_capture->setCamera(nullptr);m_camera->deleteLater();m_camera=nullptr;}
    if(m_screen){m_screen->setActive(false);m_capture->setScreenCapture(nullptr);m_screen->deleteLater();m_screen=nullptr;}
    qCInfo(lcCall)<<"stopCapture complete";
}

#ifdef Q_OS_ANDROID
extern "C" Q_DECL_EXPORT void JNICALL
Java_org_landrop_app_LanTransferActivity_nativeMediaPermissionsChanged(JNIEnv *, jobject)
{
    if(!androidCallManager)return;
    QMetaObject::invokeMethod(androidCallManager,[] {
        if(androidCallManager)androidCallManager->refreshPermissions();
    },Qt::QueuedConnection);
}
#endif
