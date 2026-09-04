#include "callmanager.h"

#include <QBuffer>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QCamera>
#include <QDateTime>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QScreenCapture>
#include <QGuiApplication>
#include <QScreen>
#include <QPointer>
#include <QLoggingCategory>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVideoFrame>
#include <QVideoSink>
#include <QtEndian>

Q_LOGGING_CATEGORY(lcCall, "landrop.call")

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
    qCInfo(lcCall) << "CallManager constructing";
    m_server=new QTcpServer(this);
    if(!m_server->listen(QHostAddress::AnyIPv4,0)){qCCritical(lcCall)<<"Media server listen failed"<<m_server->errorString();emit callError(m_server->errorString());}
    else qCInfo(lcCall)<<"Media server listening on"<<m_server->serverPort();
    connect(m_server,&QTcpServer::newConnection,this,[this]{
        auto *next=m_server->nextPendingConnection();
        const QString peerIp=next->peerAddress().toString().remove("::ffff:");
        qCInfo(lcCall)<<"Incoming media socket from"<<peerIp;
        if(!m_allowedPeers.contains(peerIp)){qCWarning(lcCall)<<"Rejected unpaired media peer"<<peerIp;next->disconnectFromHost();next->deleteLater();return;}
        if(m_socket) { next->disconnectFromHost(); next->deleteLater(); return; }
        attachSocket(next);
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
}

CallManager::~CallManager() { qCInfo(lcCall)<<"CallManager destroying";stopCapture(); }
quint16 CallManager::serverPort() const { return m_server->serverPort(); }
void CallManager::setState(const QString &s) { if(s==m_state)return;qCInfo(lcCall)<<"Call state"<<m_state<<"->"<<s;m_state=s; emit stateChanged(); }
void CallManager::setAudioMuted(bool muted) { if(m_audioMuted==muted)return; m_audioMuted=muted; emit controlsChanged(); if(m_state=="connected")startCapture(); }
void CallManager::setVideoMuted(bool muted) { if(m_videoMuted==muted)return; m_videoMuted=muted; emit controlsChanged(); if(m_state=="connected")startCapture(); }
void CallManager::allowPeer(const QString &ip) { if(!ip.isEmpty()){m_allowedPeers.insert(ip);qCInfo(lcCall)<<"Allowed media peer"<<ip;} }
void CallManager::revokePeer(const QString &ip) { m_allowedPeers.remove(ip);qCInfo(lcCall)<<"Revoked media peer"<<ip; }

void CallManager::attachSocket(QTcpSocket *socket)
{
    qCInfo(lcCall)<<"Attaching media socket"<<socket<<"peer"<<socket->peerAddress().toString();
    m_socket=socket; m_buffer.clear();
    const QPointer<QTcpSocket> guardedSocket(socket);
    connect(socket,&QTcpSocket::readyRead,this,[this,guardedSocket]{
        if(!guardedSocket||m_socket!=guardedSocket)return;
        m_buffer+=guardedSocket->readAll(); processPackets();
    });
    connect(socket,&QTcpSocket::disconnected,this,[this,socket]{
        qCInfo(lcCall)<<"Media socket disconnected"<<socket->errorString();
        if(m_socket==socket){m_socket=nullptr;stopCapture();setState("idle");} socket->deleteLater();
    });
    connect(socket,&QTcpSocket::errorOccurred,this,[this,guardedSocket](QAbstractSocket::SocketError){
        if(guardedSocket&&m_socket==guardedSocket){qCWarning(lcCall)<<"Media socket error"<<guardedSocket->errorString();emit callError(guardedSocket->errorString());}
    });
}

void CallManager::startCall(const QString &ip,quint16 port,const QString &mode)
{
    qCInfo(lcCall)<<"startCall"<<ip<<port<<mode;
    if(m_socket || ip.isEmpty() || port==0) return;
    if(!m_allowedPeers.contains(ip)){qCWarning(lcCall)<<"Call rejected because peer is not paired"<<ip;emit callError("Pair with this device before starting a call");return;}
    m_mode=mode=="screen"?"screen":"camera"; emit stateChanged();
    auto *socket=new QTcpSocket(this); attachSocket(socket); setState("calling");
    connect(socket,&QTcpSocket::connected,this,[this]{sendPacket(Invite,m_mode.toUtf8());});
    socket->connectToHost(ip,port);
}

void CallManager::acceptCall() { qCInfo(lcCall)<<"acceptCall state"<<m_state<<"mode"<<m_mode;if(!m_socket||m_state!="incoming")return; sendPacket(Accept,m_mode.toUtf8()); setState("connected"); startCapture(); }
void CallManager::rejectCall() { qCInfo(lcCall)<<"rejectCall";if(m_socket)sendPacket(Reject); hangup(); }
void CallManager::hangup() { qCInfo(lcCall)<<"hangup";if(m_socket){sendPacket(Hangup);auto *s=m_socket;m_socket=nullptr;s->disconnectFromHost();} stopCapture();setState("idle"); }
void CallManager::switchMode(const QString &mode) { qCInfo(lcCall)<<"switchMode"<<mode;m_mode=mode=="screen"?"screen":"camera"; sendPacket(SwitchMode,m_mode.toUtf8()); if(m_state=="connected")startCapture(); emit stateChanged(); }
void CallManager::startLocalDiagnostic(const QString &mode)
{
    qCInfo(lcCall) << "startLocalDiagnostic" << mode;
    if (m_socket)
        hangup();
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

void CallManager::processPackets()
{
    while(m_buffer.size()>=4){
        quint32 n=qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(m_buffer.constData()));
        if(n<1||n>16*1024*1024){emit callError("Invalid media packet");hangup();return;}
        if(m_buffer.size()<int(n+4))return;
        QByteArray packet=m_buffer.mid(4,n);m_buffer.remove(0,n+4);
        const auto type=PacketType(quint8(packet[0]));const QByteArray payload=packet.mid(1);
        if(type==Invite){m_mode=payload=="screen"?"screen":"camera";qCInfo(lcCall)<<"Received invite mode"<<m_mode;setState("incoming");emit incomingCall(m_socket->peerAddress().toString().remove("::ffff:"),m_mode);}
        else if(type==Accept){qCInfo(lcCall)<<"Call accepted by peer";setState("connected");startCapture();}
        else if(type==Reject||type==Hangup){hangup();return;}
        else if(type==SwitchMode){m_mode=payload=="screen"?"screen":"camera";qCInfo(lcCall)<<"Peer switched mode to"<<m_mode;emit stateChanged();}
        else if(type==VideoFrame){if(payload.size()>4*1024*1024){qCWarning(lcCall)<<"Oversized remote video frame"<<payload.size();emit callError("Video frame is too large");hangup();return;}QImage image=QImage::fromData(payload,"JPG");if(!image.isNull()){if(!m_loggedFirstRemoteFrame){m_loggedFirstRemoteFrame=true;qCInfo(lcCall)<<"First remote frame"<<image.size();}if(s_provider)s_provider->setFrame("remote",image);++m_remoteRevision;emit remoteFrameChanged();}}
        else if(type==AudioFrame){if(payload.size()>256*1024){emit callError("Audio frame is too large");hangup();return;}if(m_audioOutput)m_audioOutput->write(payload);}
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
        qCInfo(lcCall)<<"Creating QScreenCapture; primary screen"<<QGuiApplication::primaryScreen();
        m_screen=new QScreenCapture(this);m_screen->setScreen(QGuiApplication::primaryScreen());m_capture->setScreenCapture(m_screen);m_screen->setActive(true);
        qCInfo(lcCall)<<"QScreenCapture activation requested; active="<<m_screen->isActive();
        const QPointer<QScreenCapture> screen=m_screen;
        connect(m_screen,&QScreenCapture::errorOccurred,this,[this,screen](QScreenCapture::Error error,const QString &message){if(screen&&m_screen==screen){qCCritical(lcCall)<<"Screen capture error"<<error<<message;emit callError(message);}});
    } else if(!m_videoMuted) {
        const auto videoInput=QMediaDevices::defaultVideoInput();
        qCInfo(lcCall)<<"Default video input"<<videoInput.description()<<videoInput.id();
        if(videoInput.isNull()) {
            qCWarning(lcCall)<<"No camera available";
            emit callError("No camera available");
        } else {
            m_camera=new QCamera(videoInput,this);m_capture->setCamera(m_camera);m_camera->start();
            qCInfo(lcCall)<<"Camera start requested; active="<<m_camera->isActive();
            const QPointer<QCamera> camera=m_camera;
            connect(m_camera,&QCamera::errorOccurred,this,[this,camera](QCamera::Error error,const QString &message){if(camera&&m_camera==camera){qCCritical(lcCall)<<"Camera error"<<error<<message;emit callError(message);}});
        }
    }
    QAudioFormat audioFormat; audioFormat.setSampleRate(16000); audioFormat.setChannelCount(1); audioFormat.setSampleFormat(QAudioFormat::Int16);
    const auto inputDevice=QMediaDevices::defaultAudioInput();
    const auto outputDevice=QMediaDevices::defaultAudioOutput();
    qCInfo(lcCall)<<"Audio devices input"<<inputDevice.description()<<"output"<<outputDevice.description()
                  <<"inputFormatSupported"<<inputDevice.isFormatSupported(audioFormat)
                  <<"outputFormatSupported"<<outputDevice.isFormatSupported(audioFormat);
    if(!m_audioMuted&&!inputDevice.isNull()&&inputDevice.isFormatSupported(audioFormat)){
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
