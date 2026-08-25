#pragma once

#include <QObject>
#include <QByteArray>
#include <QSize>

// Forward declarations for FFmpeg types
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

class VideoDecoder : public QObject {
    Q_OBJECT

public:
    explicit VideoDecoder(QObject* parent = nullptr);
    ~VideoDecoder();

    void decode(const QByteArray& data, bool hasPtsPrefix = true);
    void reset();

signals:
    // Emitted when a frame is decoded. Receiver must copy data before returning.
    void frameDecoded(AVFrame* frame);
    void dimensionsChanged(int width, int height);
    void keyframeNeeded();  // Emitted on decode errors — receiver should request IDR from sender

private:
    bool initDecoder(const uint8_t* sps, int spsLen, const uint8_t* pps, int ppsLen);
    void destroyDecoder();
    void decodeNalus(const uint8_t* data, int size);

    // Which codec the stream is carrying.
    //
    // Nothing on the wire says. The frame is [type byte][PTS][AVCC NALUs] for
    // both, so it is read out of the first NAL header: H.264 puts the type in
    // the low 5 bits of one byte, HEVC in bits 1-6 of the first of two. An
    // HEVC stream read as H.264 reports a VPS as type 0, which H.264 does not
    // define, and an IDR slice as a parameter set - which is exactly what a
    // Mac sending H.265 looked like here: "Got PPS (470181 bytes)".
    enum class Codec { Unknown, H264, Hevc };
    static Codec sniffCodec(const uint8_t* videoData, int videoLen);
    bool initHevcDecoder();
    void appendAnnexB(QByteArray& out, const QByteArray& nalu) const;

    AVCodecContext* m_codecCtx = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_packet = nullptr;

    Codec m_codec = Codec::Unknown;

    // Cached SPS/PPS (from current packet scan)
    QByteArray m_sps;
    QByteArray m_pps;
    // HEVC only: the video parameter set, which H.264 has no equivalent of.
    QByteArray m_vps;
    // Active SPS/PPS (what the decoder was initialized with)
    QByteArray m_activeSps;
    QByteArray m_activePps;
    QByteArray m_activeVps;

    // HEVC packets are rewritten into this rather than allocated per frame.
    QByteArray m_annexB;

    // Track dimensions for change detection (orientation switch)
    int m_currentWidth = 0;
    int m_currentHeight = 0;
};
