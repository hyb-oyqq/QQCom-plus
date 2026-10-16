#ifndef SERIALTRANSPORT_H
#define SERIALTRANSPORT_H

#include <QSerialPort>
#include "transport/abstracttransport.h"

// 串口端点。QSerialPort::open 是同步调用（毫秒级），打开成功即 Open；
// 参数来自草稿的 serial.* 键。拔出/占用等设备错误经 errorOccurred 事件
// 驱动感知（参考工程教训：不做秒级轮询），并立即回 Closed。
class SerialTransport : public AbstractTransport
{
    Q_OBJECT
public:
    explicit SerialTransport(int endpointIndex, QObject *parent = nullptr);

    void start(const PortDraft &draft, quint64 generation) override;
    void stop() override;
    qint64 write(const QByteArray &payload, const QUuid &peerId) override;
    qint64 discardPendingInput() override;

private:
    void drainPort();
    void handlePortError(QSerialPort::SerialPortError error);
    QSerialPort *m_port = nullptr;
    // open() 失败会在 open() 内部同步发出 errorOccurred；此标志让错误处理
    // 在打开阶段退避，由 start() 统一播报，避免 close() 重入崩溃。
    bool m_opening = false;
};

#endif
