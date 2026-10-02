#include "transport/serialtransport.h"

namespace {

// 每目标 Qt 读写缓冲上限，与 ARCHITECTURE §7 的窗口预算一致：
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

    connect(m_port, &QSerialPort::readyRead, this, [this] {
        const auto payload = m_port->readAll();
        if (!payload.isEmpty())
            emit dataReceived(m_generation, makeSource(m_generation), payload,
                int(Traffic::Framing::Stream));
    });
    connect(m_port, &QSerialPort::errorOccurred, this, &SerialTransport::handlePortError);

    m_opening = true;
    const bool opened = m_port->open(QIODevice::ReadWrite);
    m_opening = false;
    if (opened) {
        emit stateChanged(m_generation, int(Transport::State::Open),
            tr("%1 @%2").arg(portName).arg(baud));
        return;
    }
    emit errorOccurred(m_generation, tr("打开串口 %1 失败：%2").arg(portName, m_port->errorString()));
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

void SerialTransport::handlePortError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError || !m_port)
        return; // NoError 会在 clearError() 等场合发出，忽略
    if (m_opening)
        return; // 打开失败路径由 start() 统一播报；在 open() 内部 close() 会重入崩溃
    // 首版把一切设备错误视为致命：拔出、占用、驱动错误都直接关闭，
    // 原因交给 errorOccurred；比猜测哪类错误可恢复更诚实。
    const auto message = m_port->errorString();
    m_port->close();
    emit errorOccurred(m_generation, tr("串口错误：%1").arg(message));
    emit stateChanged(m_generation, int(Transport::State::Closed), tr("设备错误"));
}
