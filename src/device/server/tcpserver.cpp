#include "tcpserver.h"
#include "videosocket.h"

TcpServer::TcpServer(QObject *parent) : QTcpServer(parent) {}

TcpServer::~TcpServer() {}

void TcpServer::setAudioEnabled(bool enabled)
{
    m_audioEnabled = enabled;
    m_nextSocket = 0;
}

void TcpServer::incomingConnection(qintptr handle)
{
    if (m_nextSocket == 0 || (m_audioEnabled && m_nextSocket == 1)) {
        VideoSocket *socket = new VideoSocket();
        socket->setSocketDescriptor(handle);
        addPendingConnection(socket);

        ++m_nextSocket;
    } else {
        QTcpSocket *socket = new QTcpSocket();
        socket->setSocketDescriptor(handle);
        addPendingConnection(socket);
        ++m_nextSocket;
    }
}
