#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QImage>
#include <QQuickImageProvider>
#include <QMutex>
#include <QHash>

class QCamera;
class QAudioSink;
class QAudioSource;
class QIODevice;
class QMediaCaptureSession;
class QScreenCapture;
class QTcpServer;
class QTcpSocket;
class QVideoSink;

class CallFrameProvider final : public QQuickImageProvider
{
public:
    CallFrameProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
    void setFrame(const QString &channel, const QImage &image);
private:
    QImage m_local;
    QImage m_remote;
    QMutex m_mutex;
};

class CallManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(quint16 serverPort READ serverPort CONSTANT)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY stateChanged)
    Q_PROPERTY(int localFrameRevision READ localFrameRevision NOTIFY localFrameChanged)
    Q_PROPERTY(int remoteFrameRevision READ remoteFrameRevision NOTIFY remoteFrameChanged)
    Q_PROPERTY(bool audioMuted READ audioMuted WRITE setAudioMuted NOTIFY controlsChanged)
    Q_PROPERTY(bool videoMuted READ videoMuted WRITE setVideoMuted NOTIFY controlsChanged)
    Q_PROPERTY(bool cameraPermissionGranted READ cameraPermissionGranted NOTIFY permissionsChanged)
    Q_PROPERTY(bool microphonePermissionGranted READ microphonePermissionGranted NOTIFY permissionsChanged)

public:
    explicit CallManager(QObject *parent = nullptr);
    ~CallManager() override;
    static void setFrameProvider(CallFrameProvider *provider);
    quint16 serverPort() const;
    QString state() const { return m_state; }
    QString mode() const { return m_mode; }
    int localFrameRevision() const { return m_localRevision; }
    int remoteFrameRevision() const { return m_remoteRevision; }
    bool audioMuted() const { return m_audioMuted; }
    bool videoMuted() const { return m_videoMuted; }
    bool cameraPermissionGranted() const;
    bool microphonePermissionGranted() const;
    void setAudioMuted(bool muted);
    void setVideoMuted(bool muted);

    Q_INVOKABLE void startCall(const QString &peerId, const QString &ip, quint16 port,
                               const QString &mode);
    Q_INVOKABLE void acceptCall();
    Q_INVOKABLE void rejectCall();
    Q_INVOKABLE void hangup();
    Q_INVOKABLE void switchMode(const QString &mode);
    Q_INVOKABLE void startLocalDiagnostic(const QString &mode);
    Q_INVOKABLE void allowPeer(const QString &peerId, const QString &ip,
                               const QString &mediaToken);
    Q_INVOKABLE void revokePeer(const QString &peerId, const QString &ip);
    Q_INVOKABLE void requestMediaPermissions();
    Q_INVOKABLE void openApplicationSettings();
    Q_INVOKABLE void refreshPermissions();

signals:
    void stateChanged();
    void incomingCall(const QString &ip, const QString &mode);
    void localFrameChanged();
    void remoteFrameChanged();
    void callError(const QString &message);
    void controlsChanged();
    void permissionsChanged();

private:
    friend class CallManagerIntegrationTest;
    enum PacketType : quint8 { Invite=1, Accept=2, Reject=3, Hangup=4, VideoFrame=5,
                               SwitchMode=6, AudioFrame=7, AuthHello=8, AuthChallenge=9 };
    enum class HandshakeStage { None, AwaitingChallenge, AwaitingInvite,
                                AwaitingDecision, AwaitingResponse, Authenticated };
    struct AuthorizedPeer {
        QString ip;
        QByteArray key;
    };
    void attachSocket(QTcpSocket *socket, bool outgoing);
    void sendPacket(PacketType type, const QByteArray &payload = {});
    void processPackets();
    void startCapture();
    void stopCapture();
    void endCall(bool notifyPeer);
    void resetFrames();
    void checkConnectionHealth();
    bool ensurePermissionsForMode(const QString &mode);
    void setState(const QString &state);
    void clearActiveAuthentication();
    static QByteArray authenticationProof(const QByteArray &key, const QByteArray &label,
                                          const QByteArray &challenge, const QString &mode);
    QByteArray replyPayload(PacketType type) const;
    bool verifyReply(PacketType type, const QByteArray &payload) const;

    static CallFrameProvider *s_provider;
    QTcpServer *m_server = nullptr;
    QTcpSocket *m_socket = nullptr;
    QMediaCaptureSession *m_capture = nullptr;
    QCamera *m_camera = nullptr;
    QScreenCapture *m_screen = nullptr;
    QVideoSink *m_sink = nullptr;
    QAudioSource *m_audioSource = nullptr;
    QAudioSink *m_audioSink = nullptr;
    QIODevice *m_audioInput = nullptr;
    QIODevice *m_audioOutput = nullptr;
    QByteArray m_buffer;
    QByteArray m_activeAuthKey;
    QByteArray m_authChallenge;
    QString m_peerId;
    QString m_peerIp;
    QString m_state = "idle";
    QString m_mode = "camera";
    int m_localRevision = 0;
    int m_remoteRevision = 0;
    qint64 m_lastFrameAt = 0;
    qint64 m_lastInboundAt = 0;
    bool m_audioMuted = false;
    bool m_videoMuted = false;
    QHash<QString,AuthorizedPeer> m_allowedPeers;
    HandshakeStage m_handshakeStage = HandshakeStage::None;
    bool m_loggedFirstLocalFrame = false;
    bool m_loggedFirstRemoteFrame = false;
    quint64 m_localFrameCount = 0;
};
