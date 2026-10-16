#ifndef TCPCLIENTTRANSPORT_H
#define TCPCLIENTTRANSPORT_H

#include <QAbstractSocket>
#include "transport/abstracttransport.h"

class QTcpSocket;
class QTimer;

// TCP 客户端端点。connectToHost 是异步的：start() 后先到 Opening，
// connected 后到 Open；失败/超时回 Closed 并发 errorOccurred。
// 超时由本地定时器实现（Qt 不提供连接超时），期间重复点击由上层
// 用 closeEndpoint 取消——abort 后不会有遗留回调。
class TcpClientTransport : public AbstractTransport
{
    Q_OBJECT
public:
    explicit TcpClientTransport(int endpointIndex, QObject *parent = nullptr);

    void start(const PortDraft &draft, quint64 generation) override;
    void stop() override;
    qint64 write(const QByteArray &payload, const QUuid &peerId) override;
    qint64 discardPendingInput() override;
    QString writeAddress(const QUuid &peerId) const override;

private:
    void drainSocket();
    QString peerText() const;
    QTcpSocket *m_socket = nullptr;
    QTimer *m_timeout = nullptr;
    bool m_open = false;      // 已进入 Open（用于区分错误发生在哪个阶段）
    bool m_userStop = false;  // 用户主动关闭的断开不再当错误报
    bool m_failureReported = false; // 本次 start 已经报过失败，避免 abort 重入再写一条
};

#endif
