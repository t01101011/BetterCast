#include "NetworkSender.h"
#include "../LogManager.h"
#include <QDebug>
#include <QtEndian>

NetworkSender::NetworkSender(QObject* parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &NetworkSender::attemptConnect);

    connect(m_socket, &QTcpSocket::connected, this, [this]() {
        m_retryCount = 0;
        m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        LogManager::instance().log("Sender: TCP connected to receiver");
        emit connected();
    });

    connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
        qDebug() << "Sender: TCP disconnected";
        m_rxBuffer.clear();
        emit disconnected();
    });

    // The receiver sends input back over this same socket.
    connect(m_socket, &QTcpSocket::readyRead, this, &NetworkSender::onReadyRead);

    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError err) {
        if (err == QAbstractSocket::ConnectionRefusedError && m_retryCount < MaxRetries) {
            m_retryCount++;
            int delayMs = m_retryCount * 1000;  // 1s, 2s, 3s, 4s
            LogManager::instance().log(QString("Sender: Connection refused, retry %1/%2 in %3s...")
                .arg(m_retryCount).arg(MaxRetries).arg(delayMs / 1000));
            m_retryTimer.start(delayMs);
            return;
        }
        QString errMsg = m_socket->errorString();
        if (err == QAbstractSocket::ConnectionRefusedError) {
            errMsg += QString("\nThe receiver at %1:%2 is not accepting connections. "
                              "Check that:\n"
                              "  1. Receiver mode is started on the target device\n"
                              "  2. The receiver's firewall allows incoming TCP on this port\n"
                              "  3. The discovered port matches the receiver's actual listening port")
                          .arg(m_host).arg(m_port);
        }
        qWarning() << "Sender: TCP error:" << errMsg;
        LogManager::instance().log(QString("Sender error: %1").arg(errMsg));
        emit error(errMsg);
    });
}

NetworkSender::~NetworkSender() {
    disconnect();
}

void NetworkSender::connectTo(const QString& host, uint16_t port) {
    m_host = host;
    m_port = port;
    m_retryCount = 0;
    m_rxBuffer.clear();
    attemptConnect();
}

// Reassemble [4B BE length][JSON] frames. TCP gives no message boundaries, so
// a packet can arrive split across reads or several can arrive coalesced.
void NetworkSender::onReadyRead() {
    m_rxBuffer.append(m_socket->readAll());

    while (m_rxBuffer.size() >= 4) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(m_rxBuffer.constData());
        const uint32_t len = (static_cast<uint32_t>(p[0]) << 24) |
                             (static_cast<uint32_t>(p[1]) << 16) |
                             (static_cast<uint32_t>(p[2]) << 8)  |
                              static_cast<uint32_t>(p[3]);

        if (len == 0 || len > MaxInputPacket) {
            // Desynchronised — no way to find the next boundary reliably.
            LogManager::instance().log(
                QString("Sender: Bad input frame length %1, resetting stream").arg(len));
            m_rxBuffer.clear();
            return;
        }

        if (static_cast<uint32_t>(m_rxBuffer.size()) < 4 + len) return;  // await remainder

        emit inputPacket(m_rxBuffer.mid(4, static_cast<int>(len)));
        m_rxBuffer.remove(0, static_cast<int>(4 + len));
    }
}

void NetworkSender::attemptConnect() {
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
    }
    LogManager::instance().log(QString("Sender: Connecting to %1:%2").arg(m_host).arg(m_port));
    m_socket->connectToHost(m_host, m_port);
}

void NetworkSender::disconnect() {
    m_retryTimer.stop();
    m_retryCount = MaxRetries;  // prevent further retries
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
    }
}

bool NetworkSender::isConnected() const {
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

bool NetworkSender::sendPacket(uint8_t type, const QByteArray& payload) {
    if (!isConnected()) return false;

    const qint64 packetBytes = 5 + static_cast<qint64>(payload.size());
    const qint64 queued = m_socket->bytesToWrite();
    // The budget bounds accumulated stale data, not the encoded frame size.
    // Permit one oversized IDR on an empty socket; rejecting it forever would
    // trap recovery in a request-keyframe/reject-keyframe loop.
    if (type == 0x01 && queued > 0 && queued + packetBytes > MaxQueuedVideoBytes) {
        return false;
    }

    // BetterCast TCP framing: [4B BE length][1B type][payload]
    // length = 1 (type byte) + payload size
    uint32_t totalLen = 1 + static_cast<uint32_t>(payload.size());
    uint32_t lenBE = qToBigEndian(totalLen);

    QByteArray packet;
    packet.reserve(static_cast<int>(packetBytes));
    packet.append(reinterpret_cast<const char*>(&lenBE), 4);
    packet.append(reinterpret_cast<const char*>(&type), 1);
    packet.append(payload);
    return m_socket->write(packet) == packet.size();
}

bool NetworkSender::sendVideo(const QByteArray& payload) {
    return sendPacket(0x01, payload);
}

void NetworkSender::sendAudio(const QByteArray& payload) {
    (void)sendPacket(0x02, payload);
}
