#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QByteArray>
#include <cstdint>

// TCP client that sends video/audio data using BetterCast wire protocol.
// Outbound: [4B BE length][1B type (0x01=video, 0x02=audio)][payload]
//
// The same socket carries input back from the receiver, on a different framing:
// [4B BE length][InputEvent JSON] — no type byte. See InputEvent::toPacket().
class NetworkSender : public QObject {
    Q_OBJECT
public:
    explicit NetworkSender(QObject* parent = nullptr);
    ~NetworkSender() override;

    void connectTo(const QString& host, uint16_t port);
    void disconnect();
    bool isConnected() const;

    bool sendVideo(const QByteArray& payload);
    void sendAudio(const QByteArray& payload);

signals:
    void connected();
    void disconnected();
    void error(const QString& message);
    // One complete InputEvent JSON body, length prefix already stripped.
    void inputPacket(const QByteArray& json);

private:
    bool sendPacket(uint8_t type, const QByteArray& payload);
    void attemptConnect();
    void onReadyRead();

    QTcpSocket* m_socket = nullptr;
    QByteArray m_rxBuffer;   // accumulates partial frames across reads
    static constexpr int MaxInputPacket = 64 * 1024;  // sanity bound
    QString m_host;
    uint16_t m_port = 0;
    int m_retryCount = 0;
    static constexpr int MaxRetries = 4;
    QTimer m_retryTimer;
    static constexpr qint64 MaxQueuedVideoBytes = 512 * 1024;
};
