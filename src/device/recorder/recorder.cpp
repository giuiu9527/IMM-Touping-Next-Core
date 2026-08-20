#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>

#include "compat.h"
#include "recorder.h"

static const AVRational SCRCPY_TIME_BASE = { 1, 1000000 }; // timestamps in us

static void aacStreamParameters(const AVPacket *config, int &sampleRate, int &channels)
{
    // Android's scrcpy server sends an AAC AudioSpecificConfig packet before
    // audio frames.  The MP4 muxer needs these values in addition to the raw
    // config bytes; leaving them unset corrupts the muxer state on FFmpeg 4.
    static const int sampleRates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                      24000, 22050, 16000, 12000, 11025, 8000,
                                      7350};
    sampleRate = 48000;
    channels = 2;
    if (!config || config->size < 2) {
        return;
    }
    const quint16 bits = (quint16(config->data[0]) << 8) | config->data[1];
    const int frequencyIndex = (bits >> 7) & 0x0f;
    const int channelConfig = (bits >> 3) & 0x0f;
    if (frequencyIndex >= 0 && frequencyIndex < int(sizeof(sampleRates) / sizeof(sampleRates[0]))) {
        sampleRate = sampleRates[frequencyIndex];
    }
    if (channelConfig > 0 && channelConfig <= 8) {
        channels = channelConfig;
    }
}

Recorder::Recorder(const QString &fileName, QObject *parent) : QThread(parent), m_fileName(fileName), m_format(guessRecordFormat(fileName)) {}

Recorder::~Recorder()
{
    if (m_audioConfig) {
        av_packet_free(&m_audioConfig);
    }
}

AVPacket *Recorder::packetNew(const AVPacket *packet)
{
    AVPacket *rec = av_packet_alloc();
    if (!rec) {
        return Q_NULLPTR;
    }

    if (av_packet_ref(rec, packet)) {
        delete rec;
        return Q_NULLPTR;
    }
    return rec;
}

void Recorder::packetDelete(AVPacket *packet)
{
    av_packet_unref(packet);
    av_packet_free(&packet);
}

void Recorder::queueClear()
{
    while (!m_queue.isEmpty()) {
        packetDelete(m_queue.dequeue());
    }
}

void Recorder::setFrameSize(const QSize &declaredFrameSize)
{
    m_declaredFrameSize = declaredFrameSize;
}

void Recorder::setFormat(Recorder::RecorderFormat format)
{
    m_format = format;
}

void Recorder::setAudio(AVCodecID codec, const AVPacket *config)
{
    m_audioCodec = codec;
    if (m_audioConfig) av_packet_free(&m_audioConfig);
    m_audioConfig = config ? av_packet_clone(config) : Q_NULLPTR;
}

bool Recorder::open()
{
    // codec
    const AVCodec* inputCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!inputCodec) {
        qCritical("H.264 decoder not found");
        return false;
    }

    QString formatName = recorderGetFormatName(m_format);
    Q_ASSERT(!formatName.isEmpty());
    const AVOutputFormat *format = findMuxer(formatName.toUtf8());
    if (!format) {
        qCritical("Could not find muxer");
        return false;
    }

    m_formatCtx = avformat_alloc_context();
    if (!m_formatCtx) {
        qCritical("Could not allocate output context");
        return false;
    }

    // contrary to the deprecated API (av_oformat_next()), av_muxer_iterate()
    // returns (on purpose) a pointer-to-const, but AVFormatContext.oformat
    // still expects a pointer-to-non-const (it has not be updated accordingly)
    // <https://github.com/FFmpeg/FFmpeg/commit/0694d8702421e7aff1340038559c438b61bb30dd>

    m_formatCtx->oformat = (AVOutputFormat *)format;

    QString comment = "Recorded by QtScrcpy " + QCoreApplication::applicationVersion();
    av_dict_set(&m_formatCtx->metadata, "comment", comment.toUtf8(), 0);

    AVStream *outStream = avformat_new_stream(m_formatCtx, inputCodec);
    if (!outStream) {
        avformat_free_context(m_formatCtx);
        m_formatCtx = Q_NULLPTR;
        return false;
    }

#ifdef QTSCRCPY_LAVF_HAS_NEW_CODEC_PARAMS_API
    outStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    outStream->codecpar->codec_id = inputCodec->id;
    outStream->codecpar->format = AV_PIX_FMT_YUV420P;
    outStream->codecpar->width = m_declaredFrameSize.width();
    outStream->codecpar->height = m_declaredFrameSize.height();
#else
    outStream->codec->codec_type = AVMEDIA_TYPE_VIDEO;
    outStream->codec->codec_id = inputCodec->id;
    outStream->codec->pix_fmt = AV_PIX_FMT_YUV420P;
    outStream->codec->width = m_declaredFrameSize.width();
    outStream->codec->height = m_declaredFrameSize.height();
#endif

    if (m_audioCodec != AV_CODEC_ID_NONE) {
        AVStream *audioStream = avformat_new_stream(m_formatCtx, avcodec_find_decoder(m_audioCodec));
        if (!audioStream) { avformat_free_context(m_formatCtx); m_formatCtx = Q_NULLPTR; return false; }
        audioStream->time_base = SCRCPY_TIME_BASE;
        int sampleRate = 48000;
        int channels = 2;
        if (m_audioCodec == AV_CODEC_ID_AAC) {
            aacStreamParameters(m_audioConfig, sampleRate, channels);
        }
#ifdef QTSCRCPY_LAVF_HAS_NEW_CODEC_PARAMS_API
        audioStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        audioStream->codecpar->codec_id = m_audioCodec;
        audioStream->codecpar->sample_rate = sampleRate;
#if LIBAVCODEC_VERSION_MAJOR >= 61
        av_channel_layout_default(&audioStream->codecpar->ch_layout, channels);
#else
        audioStream->codecpar->channels = channels;
        audioStream->codecpar->channel_layout = av_get_default_channel_layout(channels);
#endif
        audioStream->codecpar->format = AV_SAMPLE_FMT_FLTP;
        if (m_audioConfig) {
            audioStream->codecpar->extradata = (quint8 *)av_mallocz(m_audioConfig->size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!audioStream->codecpar->extradata) { avformat_free_context(m_formatCtx); m_formatCtx = Q_NULLPTR; return false; }
            memcpy(audioStream->codecpar->extradata, m_audioConfig->data, m_audioConfig->size);
            audioStream->codecpar->extradata_size = m_audioConfig->size;
        }
#else
        audioStream->codec->codec_type = AVMEDIA_TYPE_AUDIO;
        audioStream->codec->codec_id = m_audioCodec;
        audioStream->codec->sample_rate = sampleRate;
        audioStream->codec->channels = channels;
        audioStream->codec->channel_layout = av_get_default_channel_layout(channels);
        audioStream->codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
        if (m_audioConfig) {
            audioStream->codec->extradata = (quint8 *)av_mallocz(m_audioConfig->size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!audioStream->codec->extradata) { avformat_free_context(m_formatCtx); m_formatCtx = Q_NULLPTR; return false; }
            memcpy(audioStream->codec->extradata, m_audioConfig->data, m_audioConfig->size);
            audioStream->codec->extradata_size = m_audioConfig->size;
        }
#endif
    }

    int ret = avio_open(&m_formatCtx->pb, m_fileName.toUtf8().toStdString().c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        char errorbuf[255] = { 0 };
        av_strerror(ret, errorbuf, 254);
        qCritical() << QString("Failed to open output file: %1 %2").arg(errorbuf).arg(m_fileName).toUtf8().toStdString().c_str();
        // ostream will be cleaned up during context cleaning
        avformat_free_context(m_formatCtx);
        m_formatCtx = Q_NULLPTR;
        return false;
    }

    return true;
}

void Recorder::close()
{
    if (Q_NULLPTR != m_formatCtx) {
        if (m_headerWritten) {
            int ret = av_write_trailer(m_formatCtx);
            if (ret < 0) {
                qCritical() << QString("Failed to write trailer to %1").arg(m_fileName).toUtf8().toStdString().c_str();
                m_failed = true;
            } else {
                qInfo() << QString("success record %1").arg(m_fileName).toStdString().c_str();
            }
        } else {
            // the recorded file is empty
            m_failed = true;
        }
        avio_close(m_formatCtx->pb);
        avformat_free_context(m_formatCtx);
        m_formatCtx = Q_NULLPTR;
    }
}

bool Recorder::write(AVPacket *packet)
{
    if (!m_headerWritten) {
        if (packet->stream_index == 1) return true;
        if (packet->pts != AV_NOPTS_VALUE) {
            qCritical("The first packet is not a config packet");
            return false;
        }
        bool ok = recorderWriteHeader(packet);
        if (!ok) {
            return false;
        }
        m_headerWritten = true;
        return true;
    }

    if (packet->pts == AV_NOPTS_VALUE) {
        // ignore config packets
        return true;
    }

    recorderRescalePacket(packet);
    return av_interleaved_write_frame(m_formatCtx, packet) >= 0;
}

const AVOutputFormat *Recorder::findMuxer(const char *name)
{
#ifdef QTSCRCPY_LAVF_HAS_NEW_MUXER_ITERATOR_API
    void *opaque = Q_NULLPTR;
#endif
    const AVOutputFormat *outFormat = Q_NULLPTR;
    do {
#ifdef QTSCRCPY_LAVF_HAS_NEW_MUXER_ITERATOR_API
        outFormat = av_muxer_iterate(&opaque);
#else
        outFormat = av_oformat_next(outFormat);
#endif
        // until null or with name "name"
    } while (outFormat && strcmp(outFormat->name, name));
    return outFormat;
}

bool Recorder::recorderWriteHeader(const AVPacket *packet)
{
    AVStream *ostream = m_formatCtx->streams[0];
    quint8 *extradata = (quint8 *)av_malloc(packet->size * sizeof(quint8));
    if (!extradata) {
        qCritical("Cannot allocate extradata");
        return false;
    }
    // copy the first packet to the extra data
    memcpy(extradata, packet->data, packet->size);

#ifdef QTSCRCPY_LAVF_HAS_NEW_CODEC_PARAMS_API
    ostream->codecpar->extradata = extradata;
    ostream->codecpar->extradata_size = packet->size;
#else
    ostream->codec->extradata = extradata;
    ostream->codec->extradata_size = packet->size;
#endif

    int ret = avformat_write_header(m_formatCtx, NULL);
    if (ret < 0) {
        qCritical("Failed to write header recorder file");
        return false;
    }
    return true;
}

void Recorder::recorderRescalePacket(AVPacket *packet)
{
    AVStream *ostream = m_formatCtx->streams[packet->stream_index == 1 ? 1 : 0];
    av_packet_rescale_ts(packet, SCRCPY_TIME_BASE, ostream->time_base);
}

QString Recorder::recorderGetFormatName(Recorder::RecorderFormat format)
{
    switch (format) {
    case RECORDER_FORMAT_MP4:
        return "mp4";
    case RECORDER_FORMAT_MKV:
        return "matroska";
    default:
        return "";
    }
}

Recorder::RecorderFormat Recorder::guessRecordFormat(const QString &fileName)
{
    if (4 > fileName.length()) {
        return Recorder::RECORDER_FORMAT_NULL;
    }
    QFileInfo fileInfo = QFileInfo(fileName);
    QString ext = fileInfo.suffix();
    if (0 == ext.compare("mp4")) {
        return Recorder::RECORDER_FORMAT_MP4;
    }
    if (0 == ext.compare("mkv")) {
        return Recorder::RECORDER_FORMAT_MKV;
    }

    return Recorder::RECORDER_FORMAT_NULL;
}

void Recorder::run()
{
    qint64 ptsOrigin = AV_NOPTS_VALUE;
    for (;;) {
        AVPacket *rec = Q_NULLPTR;
        {
            QMutexLocker locker(&m_mutex);
            while (!m_stopped && m_queue.isEmpty()) {
                m_recvDataCond.wait(&m_mutex);
            }

            // if stopped is set, continue to process the remaining events (to
            // finish the recording) before actually stopping
            if (m_stopped && m_queue.isEmpty()) {
                AVPacket *last = m_previous;
                if (last) {
                    last->pts -= ptsOrigin;
                    last->dts = last->pts;
                    // assign an arbitrary duration to the last packet
                    last->duration = 100000;
                    bool ok = write(last);
                    if (!ok) {
                        // failing to write the last frame is not very serious, no
                        // future frame may depend on it, so the resulting file
                        // will still be valid
                        qWarning("Could not record last packet");
                    }
                    packetDelete(last);
                }
                break;
            }

            rec = m_queue.dequeue();
        }

        // recorder->previous is only written from this thread, no need to lock
        if (rec->stream_index == 1) {
            if (rec->pts != AV_NOPTS_VALUE) {
                if (m_audioPtsOrigin == AV_NOPTS_VALUE) m_audioPtsOrigin = rec->pts;
                rec->pts -= m_audioPtsOrigin; rec->dts = rec->pts;
                if (!write(rec)) { packetDelete(rec); break; }
            }
            packetDelete(rec);
            continue;
        }
        AVPacket *previous = m_previous;
        m_previous = rec;

        if (!previous) {
            // we just received the first packet
            continue;
        }

        // config packets have no PTS, we must ignore them
        if (rec->pts != AV_NOPTS_VALUE && previous->pts != AV_NOPTS_VALUE) {
            // we now know the duration of the previous packet
            previous->duration = rec->pts - previous->pts;
        }

        if (previous->pts != AV_NOPTS_VALUE) {
            if (ptsOrigin == AV_NOPTS_VALUE) {
                ptsOrigin = previous->pts;
            }
            previous->pts -= ptsOrigin;
            previous->dts = previous->pts;
        }        

        bool ok = write(previous);
        packetDelete(previous);
        if (!ok) {
            qCritical("Could not record packet");
            QMutexLocker locker(&m_mutex);
            m_failed = true;
            // discard pending packets
            queueClear();
            break;
        }
    }

    qDebug("Recorder thread ended");
}

bool Recorder::startRecorder()
{
    start();
    return true;
}

void Recorder::stopRecorder()
{
    QMutexLocker locker(&m_mutex);
    m_stopped = true;
    m_recvDataCond.wakeOne();
}

bool Recorder::push(const AVPacket *packet)
{
    QMutexLocker locker(&m_mutex);
    Q_ASSERT(!m_stopped);

    if (m_failed) {
        // reject any new packet (this will stop the stream)
        return false;
    }

    AVPacket *rec = packetNew(packet);
    if (rec) {
        rec->stream_index = 0;
        m_queue.enqueue(rec);
        m_recvDataCond.wakeOne();
    }
    return rec != Q_NULLPTR;
}

bool Recorder::pushAudio(const AVPacket *packet)
{
    QMutexLocker locker(&m_mutex);
    if (m_stopped || m_failed) return false;
    AVPacket *rec = packetNew(packet);
    if (rec) { rec->stream_index = 1; m_queue.enqueue(rec); m_recvDataCond.wakeOne(); }
    return rec != Q_NULLPTR;
}
