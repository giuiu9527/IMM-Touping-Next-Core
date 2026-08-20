#ifndef AUDIODEMUXER_H
#define AUDIODEMUXER_H

#include <QPointer>
#include <QThread>
extern "C" {
#include "libavcodec/avcodec.h"
}
class VideoSocket;
class AudioDemuxer : public QThread {
    Q_OBJECT
public:
    explicit AudioDemuxer(QObject *parent = Q_NULLPTR);
    void installAudioSocket(VideoSocket *socket);
    bool startDecode();
    void stopDecode();
signals:
    void getAudioPacket(AVPacket *packet, bool config, AVCodecID codec);
    void onStreamStop();
protected:
    void run() override;
private:
    qint32 recvData(quint8 *buf, qint32 size);
    QPointer<VideoSocket> m_socket;
};
#endif
