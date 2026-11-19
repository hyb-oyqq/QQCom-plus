#include "transport/serialtransport.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

// 每目标 Qt 读写缓冲上限：
// 读侧有限缓冲让无数据消费时形成背压；写侧有限缓冲让 write() 在
// 设备/驱动来不及消化时返回部分接受，而不是把滞留藏进无界队列。
constexpr qint64 SerialReadBufferBytes = 256 * 1024;
constexpr qint64 SerialWriteBufferBytes = 256 * 1024;

QSerialPort::DataBits dataBitsOf(const QString &text)
{
    // 草稿里是 "5"–"8" 的字符串；越界值回落 8，与界面默认一致。
    switch (text.toInt()) {
    case 5: return QSerialPort::Data5;
    case 6: return QSerialPort::Data6;
    case 7: return QSerialPort::Data7;
    default: return QSerialPort::Data8;
    }
}

QSerialPort::Parity parityOf(const QString &text)
{
    if (text == QLatin1String("Even")) return QSerialPort::EvenParity;
    if (text == QLatin1String("Odd")) return QSerialPort::OddParity;
    if (text == QLatin1String("Mark")) return QSerialPort::MarkParity;
    if (text == QLatin1String("Space")) return QSerialPort::SpaceParity;
    return QSerialPort::NoParity;
}

QSerialPort::StopBits stopBitsOf(const QString &text)
{
    if (text == QLatin1String("1.5")) return QSerialPort::OneAndHalfStop;
    if (text == QLatin1String("2")) return QSerialPort::TwoStop;
    return QSerialPort::OneStop;
}

QSerialPort::FlowControl flowControlOf(const QString &text)
{
    if (text == QLatin1String("RTS/CTS")) return QSerialPort::HardwareControl;
    if (text == QLatin1String("XON/XOFF")) return QSerialPort::SoftwareControl;
    return QSerialPort::NoFlowControl;
}

bool initializeControlLines(QSerialPort *port, QString &error)
{
#ifdef Q_OS_WIN
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!::GetCommState(port->handle(), &dcb)) {
        error = QObject::tr("读取串口控制线失败（Windows 错误 %1）").arg(::GetLastError());
        return false;
    }
    // Qt 的流控设置不会清掉旧 DSR 握手/接收门控。与控制线一起设置，
    // 避免 UI 已选无流控，驱动仍因 DSR 无效而暂停收发。
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    if (port->flowControl() != QSerialPort::HardwareControl)
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
    if (!::SetCommState(port->handle(), &dcb)) {
        error = QObject::tr("初始化串口控制线失败（Windows 错误 %1）").arg(::GetLastError());
        return false;
    }
    return true;
#else
    // 先释放 RTS，再释放 DTR，避免部分自动下载电路短暂进入复位状态。
    if ((port->flowControl() == QSerialPort::HardwareControl || port->setRequestToSend(false))
        && port->setDataTerminalReady(false))
        return true;
    error = port->errorString();
    return false;
#endif
}

} // namespace

SerialTransport::SerialTransport(int endpointIndex, QObject *parent)
    : AbstractTransport(parent)
{
    m_endpointIndex = endpointIndex;
}

void SerialTransport::start(const PortDraft &draft, quint64 generation)
{
    m_draft = draft;
    m_generation = generation;

    // 一个实例只跑一次 start（运行时换配置就换实例），这里防御性清理旧口。
    if (m_port) {
        m_port->close();
        m_port->deleteLater();
        m_port = nullptr;
    }

    const auto portName = draft.values.value(QStringLiteral("serial.port")).toString();
    qint32 baud = draft.values.value(QStringLiteral("serial.baud"), 115200).toString().toInt();
    if (baud <= 0) baud = 115200;

    m_port = new QSerialPort(this);
    m_port->setPortName(portName);
    m_port->setBaudRate(baud);
    m_port->setDataBits(dataBitsOf(draft.values.value(QStringLiteral("serial.bits"), "8").toString()));
    m_port->setParity(parityOf(draft.values.value(QStringLiteral("serial.parity"), "None").toString()));
    m_port->setStopBits(stopBitsOf(draft.values.value(QStringLiteral("serial.stop"), "1").toString()));
    m_port->setFlowControl(flowControlOf(draft.values.value(QStringLiteral("serial.flow"), "None").toString()));
    m_port->setReadBufferSize(SerialReadBufferBytes);
    m_port->setWriteBufferSize(SerialWriteBufferBytes);

    connect(m_port, &QSerialPort::errorOccurred, this, &SerialTransport::handlePortError);

    m_opening = true;
    QString controlError;
    bool opened = m_port->open(QIODevice::ReadWrite);
    if (opened) {
        // 无流控不等于控制线已释放：Windows 驱动可能保留上个工具留下的 DTR，
        // 在 ESP 等设备上会影响复位/启动模式。RTS/CTS 模式的 RTS 必须交给驱动。
        opened = initializeControlLines(m_port, controlError);
        if (opened) {
            // Qt 默认在 close() 恢复打开前的 DCB，可能把刚释放的 DTR 再次置为有效。
            m_port->setSettingsRestoredOnClose(false);
        }
    }
    m_opening = false;
    if (opened) {
        // 只丢掉 Qt 缓冲里已经读上来的字节。空 readAll() 会走进 startAsyncRead()，
        // 和 open() 挂着的 WaitCommEvent 抢同一把端口，Windows 上偶发之后再也等不到 RXCHAR。
        discardPendingInput();
        connect(m_port, &QSerialPort::readyRead, this, &SerialTransport::drainPort);
        emit stateChanged(m_generation, int(Transport::State::Open),
            tr("%1 @%2").arg(portName).arg(baud));
        return;
    }
    const auto error = controlError.isEmpty() ? m_port->errorString() : controlError;
    if (m_port->isOpen())
        m_port->close();
    emit errorOccurred(m_generation, tr("打开串口 %1 失败：%2").arg(portName, error));
    emit stateChanged(m_generation, int(Transport::State::Closed), tr("打开失败"));
}

void SerialTransport::stop()
{
    if (m_port && m_port->isOpen())
        m_port->close();
}

qint64 SerialTransport::write(const QByteArray &payload, const QUuid &peerId)
{
    Q_UNUSED(peerId); // 串口没有对端选择
    if (!m_port || !m_port->isOpen())
        return -1;
    return m_port->write(payload);
}

void SerialTransport::drainPort()
{
    if (!m_port || !m_port->isOpen() || m_port->bytesAvailable() <= 0)
        return;
    const auto payload = m_port->readAll();
    if (payload.isEmpty())
        return;
    emit dataReceived(m_generation, makeSource(m_generation), payload,
        int(Traffic::Framing::Stream));
    // readyRead 不会嵌套发出。槽里又进来的字节留在缓冲里，等下一拍再读，避免同步桥接把栈打穿。
    if (m_port && m_port->isOpen() && m_port->bytesAvailable() > 0)
        QMetaObject::invokeMethod(this, [this] { drainPort(); }, Qt::QueuedConnection);
}

qint64 SerialTransport::discardPendingInput()
{
    // 缓冲为空时 readAll() 仍会调用 readData() → startAsyncRead()。打开后和桥接建立时
    // 都会走到这里；空读正是串口偶发收不到的原因，所以没有字节就直接返回。
    if (!m_port || !m_port->isOpen() || m_port->bytesAvailable() <= 0)
        return 0;
    return m_port->readAll().size();
}

void SerialTransport::handlePortError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError || !m_port)
        return; // NoError 会在 clearError() 等场合发出，忽略
    if (m_opening)
        return; // 打开及控制线初始化失败由 start() 统一播报；在 open() 内部 close() 会重入崩溃
    // 首版把一切设备错误视为致命：拔出、占用、驱动错误都直接关闭，
    // 原因交给 errorOccurred；比猜测哪类错误可恢复更诚实。
    const auto message = m_port->errorString();
    m_port->close();
    emit errorOccurred(m_generation, tr("串口错误：%1").arg(message));
    emit stateChanged(m_generation, int(Transport::State::Closed), tr("设备错误"));
}
