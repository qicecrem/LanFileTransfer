#include "callmanager.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QCoreApplication>
#include <QDateTime>
#include <QDataStream>
#include <QHostAddress>
#include <QImage>
#include <QSize>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QtEndian>

#include <cstdio>
#include <functional>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::fflush(stderr);
    }
    return condition;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return condition();
}

}

class CallManagerIntegrationTest {
public:
    static void sendRawPacket(QTcpSocket *socket,CallManager::PacketType type,
                              const QByteArray &payload)
    {
        QByteArray packet;
        packet.append(char(type));
        packet.append(payload);
        const quint32 size=qToBigEndian<quint32>(packet.size());
        socket->write(reinterpret_cast<const char *>(&size),sizeof(size));
        socket->write(packet);
        socket->flush();
    }

    static void expireConnectedCall(CallManager &manager)
    {
        manager.m_lastInboundAt = QDateTime::currentMSecsSinceEpoch() - 30000;
        manager.checkConnectionHealth();
    }

    static int run()
    {
        const QString localhost = QStringLiteral("127.0.0.1");
        CallFrameProvider provider;
        CallManager::setFrameProvider(&provider);

        CallManager blocked;
        bool rogueConnected = false;
        QTcpSocket rogue;
        QObject::connect(&rogue, &QTcpSocket::connected, &rogue,
                         [&] { rogueConnected = true; });
        rogue.connectToHost(localhost, blocked.serverPort());
        if (!require(waitUntil([&] {
                return rogueConnected && rogue.state() == QAbstractSocket::UnconnectedState;
            }), "unpaired media socket was not rejected")) return 1;
        if (!require(blocked.state() == QStringLiteral("idle"),
                     "unpaired media socket changed call state")) return 2;

        const QString mediaToken=QString::fromLatin1(QByteArray(32,'k').toBase64());
        const QString wrongMediaToken=QString::fromLatin1(QByteArray(32,'x').toBase64());
        CallManager authenticatedOnly;
        CallManager impostor;
        authenticatedOnly.allowPeer(QStringLiteral("expected-peer"),localhost,mediaToken);
        impostor.allowPeer(QStringLiteral("authenticated-only"),localhost,wrongMediaToken);
        int forgedIncomingCalls=0;
        QObject::connect(&authenticatedOnly,&CallManager::incomingCall,&authenticatedOnly,
                         [&](const QString &,const QString &){++forgedIncomingCalls;});
        impostor.startCall(QStringLiteral("authenticated-only"),localhost,
                           authenticatedOnly.serverPort(),QStringLiteral("camera"));
        if (!require(waitUntil([&] {
                return impostor.state()==QStringLiteral("idle")
                    && authenticatedOnly.state()==QStringLiteral("idle")
                    && impostor.m_socket==nullptr&&authenticatedOnly.m_socket==nullptr;
            }), "same-IP peer with the wrong media key was not rejected")) return 15;
        if (!require(forgedIncomingCalls==0,
                     "invalid media proof reached the incoming-call prompt")) return 16;

        CallManager forgedResponderCaller;
        forgedResponderCaller.allowPeer(QStringLiteral("forged-responder"),localhost,mediaToken);
        QTcpServer forgedResponder;
        if (!require(forgedResponder.listen(QHostAddress::LocalHost,0),
                     "could not start forged media responder")) return 17;
        forgedResponderCaller.startCall(QStringLiteral("forged-responder"),localhost,
                                        forgedResponder.serverPort(),QStringLiteral("camera"));
        if (!require(waitUntil([&]{return forgedResponder.hasPendingConnections();}),
                     "forged media responder did not receive connection")) return 18;
        QTcpSocket *forgedSocket=forgedResponder.nextPendingConnection();
        if (!require(waitUntil([&]{return forgedSocket->bytesAvailable()>0;}),
                     "caller did not send media authentication hello")) return 19;
        forgedSocket->readAll();
        const QByteArray forgedChallenge(32,'c');
        QByteArray challengePayload;
        QDataStream challengeStream(&challengePayload,QIODevice::WriteOnly);
        challengeStream.setVersion(QDataStream::Qt_6_5);
        challengeStream<<quint8(2)<<forgedChallenge;
        sendRawPacket(forgedSocket,CallManager::AuthChallenge,challengePayload);
        if (!require(waitUntil([&]{return forgedSocket->bytesAvailable()>0;}),
                     "caller did not answer media authentication challenge")) return 20;
        forgedSocket->readAll();
        QByteArray forgedAcceptPayload;
        QDataStream acceptStream(&forgedAcceptPayload,QIODevice::WriteOnly);
        acceptStream.setVersion(QDataStream::Qt_6_5);
        acceptStream<<quint8(2)<<QByteArray(32,'x');
        sendRawPacket(forgedSocket,CallManager::Accept,forgedAcceptPayload);
        if (!require(waitUntil([&]{
                return forgedResponderCaller.state()==QStringLiteral("idle")
                    &&forgedResponderCaller.m_socket==nullptr;
            }), "caller accepted a forged media response")) return 21;
        forgedSocket->deleteLater();

        provider.setFrame(QStringLiteral("local"), QImage(80, 60, QImage::Format_RGB32));
        provider.setFrame(QStringLiteral("remote"), QImage(120, 90, QImage::Format_RGB32));
        QSize localSize;
        QSize remoteSize;
        provider.requestImage(QStringLiteral("local"), &localSize, {});
        provider.requestImage(QStringLiteral("remote"), &remoteSize, {});
        if (!require(localSize == QSize(80, 60) && remoteSize == QSize(120, 90),
                     "frame provider setup failed")) return 3;
        const int localRevision = blocked.localFrameRevision();
        const int remoteRevision = blocked.remoteFrameRevision();
        blocked.hangup();
        provider.requestImage(QStringLiteral("local"), &localSize, {});
        provider.requestImage(QStringLiteral("remote"), &remoteSize, {});
        if (!require(localSize == QSize(16, 9) && remoteSize == QSize(16, 9),
                     "hangup retained stale call frames")) return 4;
        if (!require(blocked.localFrameRevision() > localRevision
                     && blocked.remoteFrameRevision() > remoteRevision,
                     "hangup did not invalidate QML frame URLs")) return 5;

        CallManager left;
        CallManager right;
        left.allowPeer(QStringLiteral("right-peer"),localhost,mediaToken);
        right.allowPeer(QStringLiteral("left-peer"),localhost,mediaToken);
        int incomingCalls = 0;
        bool acceptIncoming = false;
        QObject::connect(&right, &CallManager::incomingCall, &right,
                         [&](const QString &, const QString &) {
            ++incomingCalls;
            if (acceptIncoming) right.acceptCall();
            else right.rejectCall();
        });

        left.startCall(QStringLiteral("right-peer"),localhost,right.serverPort(),QStringLiteral("camera"));
        if (!require(waitUntil([&] {
                return incomingCalls == 1 && left.state() == QStringLiteral("idle")
                    && right.state() == QStringLiteral("idle");
            }), "rejected call did not return both peers to idle")) return 6;

        QTcpServer portReservation;
        if (!require(portReservation.listen(QHostAddress::LocalHost, 0),
                     "could not reserve a test port")) return 7;
        const quint16 unavailablePort = portReservation.serverPort();
        portReservation.close();
        bool connectionError = false;
        QObject::connect(&left, &CallManager::callError, &left,
                         [&](const QString &) { connectionError = true; });
        left.startCall(QStringLiteral("right-peer"),localhost,unavailablePort,QStringLiteral("camera"));
        if (!require(waitUntil([&] {
                return connectionError && left.state() == QStringLiteral("idle")
                    && left.m_socket == nullptr;
            }), "failed outgoing call remained stuck in calling state")) return 8;

        left.startCall(QStringLiteral("right-peer"),localhost,right.serverPort(),QStringLiteral("screen"));
        if (!require(waitUntil([&] {
                return incomingCalls == 2 && left.state() == QStringLiteral("idle")
                    && right.state() == QStringLiteral("idle");
            }), "new call could not start after a connection failure")) return 9;

        left.setAudioMuted(true);
        left.setVideoMuted(true);
        right.setAudioMuted(true);
        right.setVideoMuted(true);
        acceptIncoming = true;
        left.startCall(QStringLiteral("right-peer"),localhost,right.serverPort(),QStringLiteral("screen"));
        if (!require(waitUntil([&] {
                return incomingCalls == 3 && left.state() == QStringLiteral("connected")
                    && right.state() == QStringLiteral("connected");
            }), "accepted call did not connect both peers")) return 10;
        provider.setFrame(QStringLiteral("remote"), QImage(160, 90, QImage::Format_RGB32));
        const int timeoutRevision = left.remoteFrameRevision();
        expireConnectedCall(left);
        if (!require(waitUntil([&] {
                return left.state() == QStringLiteral("idle")
                    && right.state() == QStringLiteral("idle")
                    && left.m_socket == nullptr && right.m_socket == nullptr;
            }), "silent media timeout did not release both connected peers")) return 11;
        provider.requestImage(QStringLiteral("remote"), &remoteSize, {});
        if (!require(remoteSize == QSize(16, 9)
                     && left.remoteFrameRevision() > timeoutRevision,
                     "silent media timeout retained the last remote frame")) return 12;

        left.startCall(QStringLiteral("right-peer"),localhost,right.serverPort(),QStringLiteral("camera"));
        if (!require(waitUntil([&] {
                return incomingCalls == 4 && left.state() == QStringLiteral("connected")
                    && right.state() == QStringLiteral("connected");
            }), "new call could not start after media timeout")) return 13;
        left.hangup();
        if (!require(waitUntil([&] {
                return left.state() == QStringLiteral("idle")
                    && right.state() == QStringLiteral("idle")
                    && left.m_socket == nullptr && right.m_socket == nullptr;
            }), "hangup did not release both connected peers")) return 14;

        CallManager::setFrameProvider(nullptr);
        return 0;
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    return CallManagerIntegrationTest::run();
}
