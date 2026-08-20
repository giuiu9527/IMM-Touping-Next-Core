#include "audiodemuxer.h"
#include "videosocket.h"
#define HEADER_SIZE 12
#define SC_PACKET_FLAG_CONFIG (UINT64_C(1) << 62)
#define SC_PACKET_PTS_MASK ((UINT64_C(1) << 61) - 1)
static quint32 read32be(const quint8 *buf) { return (quint32(buf[0]) << 24) | (quint32(buf[1]) << 16) | (quint32(buf[2]) << 8) | quint32(buf[3]); }
static quint64 read64be(const quint8 *buf) { return (quint64(read32be(buf)) << 32) | read32be(buf + 4); }
AudioDemuxer::AudioDemuxer(QObject *parent) : QThread(parent) {}
void AudioDemuxer::installAudioSocket(VideoSocket *socket) { if (socket) { socket->moveToThread(this); m_socket = socket; } }
bool AudioDemuxer::startDecode() { if (!m_socket) return false; start(); return true; }
void AudioDemuxer::stopDecode() { wait(); }
qint32 AudioDemuxer::recvData(quint8 *buf, qint32 size) { return m_socket ? m_socket->subThreadRecvData(buf, size) : 0; }
void AudioDemuxer::run() {
    quint8 codecBytes[4];
    AVCodecID codec = AV_CODEC_ID_NONE;
    if (recvData(codecBytes, 4) == 4) {
        const quint32 fourcc = read32be(codecBytes);
        if (fourcc == 0x00616163) codec = AV_CODEC_ID_AAC; else if (fourcc == 0x6f707573) codec = AV_CODEC_ID_OPUS; else if (fourcc == 0x666c6163) codec = AV_CODEC_ID_FLAC;
    }
    while (codec != AV_CODEC_ID_NONE) {
        quint8 header[HEADER_SIZE]; if (recvData(header, HEADER_SIZE) != HEADER_SIZE) break;
        const quint64 flags = read64be(header); const quint32 size = read32be(header + 8); if (!size) break;
        AVPacket *packet = av_packet_alloc();
        if (!packet || av_new_packet(packet, int(size)) || recvData(packet->data, int(size)) != int(size)) { if (packet) av_packet_free(&packet); break; }
        const bool config = flags & SC_PACKET_FLAG_CONFIG; packet->pts = config ? AV_NOPTS_VALUE : qint64(flags & SC_PACKET_PTS_MASK); packet->dts = packet->pts;
        emit getAudioPacket(packet, config, codec); av_packet_free(&packet);
    }
    if (m_socket) { m_socket->close(); delete m_socket; m_socket = Q_NULLPTR; }
    emit onStreamStop();
}
