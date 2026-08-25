#include "VideoDecoder.h"
#include "LogManager.h"
#include <QDebug>
#include <QtEndian>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

VideoDecoder::VideoDecoder(QObject* parent)
    : QObject(parent)
{
}

VideoDecoder::~VideoDecoder() {
    destroyDecoder();
}

void VideoDecoder::decode(const QByteArray& data, bool hasPtsPrefix) {
    static int decodeCallCount = 0;
    decodeCallCount++;

    // Type-byte framing (Mac sender): raw AVCC NALUs, no PTS prefix
    // Legacy framing (Swift/Android): [PTS: 8 bytes][NALUs...]
    int headerSize = hasPtsPrefix ? 8 : 0;

    if (data.size() <= headerSize) {
        if (decodeCallCount <= 5) {
            LogManager::instance().log(QString("Decoder: frame %1 too small (%2 bytes), skipping")
                .arg(decodeCallCount).arg(data.size()));
        }
        return;
    }

    const uint8_t* raw = reinterpret_cast<const uint8_t*>(data.constData());

    const uint8_t* videoData = raw + headerSize;
    int videoLen = data.size() - headerSize;

    // Which codec this is, decided once per stream from the first parameter
    // set seen. Re-sniffed only while unknown, so a mid-stream slice cannot
    // flip it.
    if (m_codec == Codec::Unknown) {
        const Codec sniffed = sniffCodec(videoData, videoLen);
        if (sniffed != Codec::Unknown) {
            m_codec = sniffed;
            LogManager::instance().log(
                QString("Decoder: stream is %1")
                    .arg(m_codec == Codec::Hevc ? "H.265 (HEVC)" : "H.264"));
        }
    }
    const bool hevc = (m_codec == Codec::Hevc);

    // Scan for SPS/PPS in AVCC-framed NALUs: [4-byte big-endian length][NALU data]
    int offset = 0;
    int naluCount = 0;
    while (offset + 4 <= videoLen) {
        uint32_t naluLen = qFromBigEndian<uint32_t>(videoData + offset);
        // Compare in 64-bit. A corrupt length above INT_MAX used to cast to a negative
        // int, slip past this guard, and drive `offset` backwards — so the next read
        // landed gigabytes outside the buffer and took the process down with an access
        // violation instead of dropping one bad frame.
        if (naluLen == 0 ||
            static_cast<qint64>(offset) + 4 + static_cast<qint64>(naluLen) > static_cast<qint64>(videoLen)) {
            if (decodeCallCount <= 5) {
                LogManager::instance().log(QString("Decoder: frame %1 NALU scan stopped at offset %2, naluLen=%3, videoLen=%4")
                    .arg(decodeCallCount).arg(offset).arg(naluLen).arg(videoLen));
            }
            break;
        }

        // HEVC's header is two bytes and puts the type in bits 1-6 of the
        // first; H.264's is one byte, low 5 bits.
        const uint8_t hdr = videoData[offset + 4];
        uint8_t naluType = hevc ? uint8_t((hdr >> 1) & 0x3F) : uint8_t(hdr & 0x1F);
        naluCount++;

        if (decodeCallCount <= 3) {
            LogManager::instance().log(QString("Decoder: frame %1 NALU #%2: type=%3, len=%4")
                .arg(decodeCallCount).arg(naluCount).arg(naluType).arg(naluLen));
        }

        // VPS 32, SPS 33, PPS 34 in HEVC; SPS 7, PPS 8 in H.264.
        const uint8_t spsType = hevc ? 33 : 7;
        const uint8_t ppsType = hevc ? 34 : 8;
        const char* nalu = reinterpret_cast<const char*>(videoData + offset + 4);

        if (hevc && naluType == 32) {
            m_vps = QByteArray(nalu, static_cast<int>(naluLen));
            LogManager::instance().log(QString("Decoder: Got VPS (%1 bytes)").arg(naluLen));
        } else if (naluType == spsType) {
            m_sps = QByteArray(nalu, static_cast<int>(naluLen));
            LogManager::instance().log(QString("Decoder: Got SPS (%1 bytes)").arg(naluLen));
        } else if (naluType == ppsType) {
            m_pps = QByteArray(nalu, static_cast<int>(naluLen));
            LogManager::instance().log(QString("Decoder: Got PPS (%1 bytes)").arg(naluLen));
        }

        offset += 4 + static_cast<int>(naluLen);
    }

    // Initialize or reinitialize decoder once every parameter set is in hand.
    // HEVC needs three; H.264 has no VPS.
    const bool haveSets = !m_sps.isEmpty() && !m_pps.isEmpty() && (!hevc || !m_vps.isEmpty());
    if (haveSets) {
        bool needsInit = !m_codecCtx;
        // Also reinit if the sets changed (new stream or resolution change)
        if (m_codecCtx && (m_sps != m_activeSps || m_pps != m_activePps || m_vps != m_activeVps)) {
            LogManager::instance().log("Decoder: parameter sets changed — reinitializing for new stream");
            destroyDecoder();
            needsInit = true;
        }
        if (needsInit) {
            LogManager::instance().log(
                QString("Decoder: Initializing %1 with SPS(%2) + PPS(%3)%4")
                    .arg(hevc ? "H.265" : "H.264").arg(m_sps.size()).arg(m_pps.size())
                    .arg(hevc ? QString(" + VPS(%1)").arg(m_vps.size()) : QString()));
            const bool ok = hevc
                ? initHevcDecoder()
                : initDecoder(reinterpret_cast<const uint8_t*>(m_sps.constData()), m_sps.size(),
                              reinterpret_cast<const uint8_t*>(m_pps.constData()), m_pps.size());
            if (ok) {
                m_activeSps = m_sps;
                m_activePps = m_pps;
                m_activeVps = m_vps;
            }
        }
    }

    if (m_codecCtx) {
        if (hevc) {
            // FFmpeg reads the HEVC parameter sets below as Annex B, which
            // puts its parser in Annex B mode - so the packets have to match.
            // Rewriting the four-byte lengths into start codes is cheaper than
            // building an hvcC record and getting its profile fields wrong.
            m_annexB.clear();
            m_annexB.reserve(videoLen + 16);
            int p = 0;
            while (p + 4 <= videoLen) {
                const uint32_t n = qFromBigEndian<uint32_t>(videoData + p);
                if (n == 0 || static_cast<qint64>(p) + 4 + n > videoLen) break;
                m_annexB.append("\x00\x00\x00\x01", 4);
                m_annexB.append(reinterpret_cast<const char*>(videoData + p + 4),
                                static_cast<int>(n));
                p += 4 + static_cast<int>(n);
            }
            if (!m_annexB.isEmpty()) {
                decodeNalus(reinterpret_cast<const uint8_t*>(m_annexB.constData()),
                            m_annexB.size());
            }
        } else {
            decodeNalus(videoData, videoLen);
        }
    } else if (decodeCallCount <= 10) {
        LogManager::instance().log(QString("Decoder: frame %1 — no codec context yet (waiting for SPS/PPS)")
            .arg(decodeCallCount));
    }
}

VideoDecoder::Codec VideoDecoder::sniffCodec(const uint8_t* videoData, int videoLen) {
    // Walk the AVCC NALUs and look for a parameter set that only one of the
    // two codecs could have produced.
    int offset = 0;
    while (offset + 5 <= videoLen) {
        const uint32_t naluLen = qFromBigEndian<uint32_t>(videoData + offset);
        if (naluLen == 0 ||
            static_cast<qint64>(offset) + 4 + naluLen > static_cast<qint64>(videoLen)) {
            break;
        }
        const uint8_t hdr = videoData[offset + 4];

        // HEVC: VPS 32, SPS 33, PPS 34. A VPS is decisive - H.264 has no such
        // thing, and reading one as H.264 gives type 0, which is undefined.
        const uint8_t hevcType = (hdr >> 1) & 0x3F;
        if (hevcType >= 32 && hevcType <= 34) return Codec::Hevc;

        // H.264: SPS 7, PPS 8, and the top bit is the forbidden zero bit.
        const uint8_t h264Type = hdr & 0x1F;
        if ((hdr & 0x80) == 0 && (h264Type == 7 || h264Type == 8)) return Codec::H264;

        offset += 4 + static_cast<int>(naluLen);
    }
    return Codec::Unknown;
}

void VideoDecoder::appendAnnexB(QByteArray& out, const QByteArray& nalu) const {
    if (nalu.isEmpty()) return;
    out.append("\x00\x00\x00\x01", 4);
    out.append(nalu);
}

bool VideoDecoder::initHevcDecoder() {
    // Extradata as Annex B rather than hvcC. FFmpeg accepts either - it treats
    // extradata starting with 1 as hvcC and anything else as Annex B - and
    // Annex B avoids hand-building a configuration record whose profile, tier
    // and level fields would have to be parsed back out of the SPS to be
    // right. The packets are converted to match in decode().
    QByteArray extra;
    appendAnnexB(extra, m_vps);
    appendAnnexB(extra, m_sps);
    appendAnnexB(extra, m_pps);
    if (extra.isEmpty()) return false;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) {
        LogManager::instance().log("Decoder: no H.265 decoder in this FFmpeg build");
        return false;
    }

    if (m_codecCtx) destroyDecoder();

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    uint8_t* extradata = static_cast<uint8_t*>(
        av_malloc(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!extradata) {
        avcodec_free_context(&m_codecCtx);
        return false;
    }
    memcpy(extradata, extra.constData(), extra.size());
    memset(extradata + extra.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
    m_codecCtx->extradata = extradata;
    m_codecCtx->extradata_size = extra.size();

    // Same latency and resilience choices as the H.264 path.
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->thread_count = 4;
    m_codecCtx->thread_type = FF_THREAD_SLICE;
    m_codecCtx->err_recognition = 0;
    m_codecCtx->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        LogManager::instance().log("Decoder: failed to open the H.265 decoder");
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();
    LogManager::instance().log("Decoder: H.265 decoder initialized");
    return true;
}

bool VideoDecoder::initDecoder(const uint8_t* sps, int spsLen, const uint8_t* pps, int ppsLen) {
    // Build extradata in AVCC format for FFmpeg
    // Format: [1 byte version][1 byte profile][1 byte compat][1 byte level]
    //         [1 byte NALU length size - 1][1 byte num SPS | 0xE0]
    //         [2 byte SPS length][SPS data]
    //         [1 byte num PPS][2 byte PPS length][PPS data]

    if (spsLen < 4) return false;

    int extradataSize = 6 + 2 + spsLen + 1 + 2 + ppsLen;
    uint8_t* extradata = static_cast<uint8_t*>(av_malloc(extradataSize + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!extradata) return false;
    memset(extradata, 0, extradataSize + AV_INPUT_BUFFER_PADDING_SIZE);

    int idx = 0;
    extradata[idx++] = 1;           // version
    extradata[idx++] = sps[1];     // profile
    extradata[idx++] = sps[2];     // compatibility
    extradata[idx++] = sps[3];     // level
    extradata[idx++] = 0xFF;       // 4 bytes NALU length size (0xFF = 3 + 1)
    extradata[idx++] = 0xE1;       // 1 SPS (0xE0 | 1)
    extradata[idx++] = static_cast<uint8_t>((spsLen >> 8) & 0xFF);
    extradata[idx++] = static_cast<uint8_t>(spsLen & 0xFF);
    memcpy(extradata + idx, sps, spsLen);
    idx += spsLen;
    extradata[idx++] = 1;          // 1 PPS
    extradata[idx++] = static_cast<uint8_t>((ppsLen >> 8) & 0xFF);
    extradata[idx++] = static_cast<uint8_t>(ppsLen & 0xFF);
    memcpy(extradata + idx, pps, ppsLen);

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        qWarning() << "H.264 decoder not found";
        av_free(extradata);
        return false;
    }

    // If we already have a context, check for dimension change
    if (m_codecCtx) {
        // We'll destroy and recreate — dimension change detected via SPS
        destroyDecoder();
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        av_free(extradata);
        return false;
    }

    m_codecCtx->extradata = extradata;
    m_codecCtx->extradata_size = extradataSize;

    // Low latency settings with error resilience
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->thread_count = 4; // more threads to keep up at high FPS
    m_codecCtx->thread_type = FF_THREAD_SLICE;

    // Error concealment — show best-effort frames instead of artifacts
    m_codecCtx->err_recognition = 0;  // Don't reject frames with errors
    m_codecCtx->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        qWarning() << "Failed to open H.264 decoder";
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();

    qDebug() << "H.264 decoder initialized";
    return true;
}

void VideoDecoder::reset() {
    LogManager::instance().log("Decoder: reset — clearing state for new stream");
    destroyDecoder();
    m_sps.clear();
    m_pps.clear();
    m_vps.clear();
    m_activeSps.clear();
    m_activePps.clear();
    m_activeVps.clear();
    m_annexB.clear();
    // The next sender may be a different one, streaming a different codec.
    m_codec = Codec::Unknown;
}

void VideoDecoder::destroyDecoder() {
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    m_currentWidth = 0;
    m_currentHeight = 0;
}

void VideoDecoder::decodeNalus(const uint8_t* data, int size) {
    static int sendCount = 0;
    static int outputCount = 0;

    // Build a single packet with all NALUs (AVCC framing)
    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = size;

    sendCount++;
    int ret = avcodec_send_packet(m_codecCtx, m_packet);
    if (ret < 0) {
        if (sendCount <= 10 || sendCount % 100 == 0) {
            LogManager::instance().log(QString("Decoder: send_packet #%1 failed: %2")
                .arg(sendCount).arg(ret));
        }
        if (ret == AVERROR_INVALIDDATA) {
            avcodec_flush_buffers(m_codecCtx);
            emit keyframeNeeded();
        }
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            if (outputCount <= 5) {
                LogManager::instance().log(QString("Decoder: receive_frame error: %1").arg(ret));
            }
            break;
        }

        outputCount++;
        if (outputCount <= 3 || outputCount % 300 == 0) {
            LogManager::instance().log(QString("Decoder: decoded frame #%1 — %2x%3 format=%4")
                .arg(outputCount).arg(m_frame->width).arg(m_frame->height).arg(m_frame->format));
        }

        // Check for dimension change (orientation switch)
        if (m_frame->width != m_currentWidth || m_frame->height != m_currentHeight) {
            m_currentWidth = m_frame->width;
            m_currentHeight = m_frame->height;
            LogManager::instance().log(QString("Decoder: dimensions changed to %1x%2")
                .arg(m_currentWidth).arg(m_currentHeight));
            emit dimensionsChanged(m_currentWidth, m_currentHeight);
        }

        emit frameDecoded(m_frame);
    }
}
