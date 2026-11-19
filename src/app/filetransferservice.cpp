#include "filetransferservice.h"

#include "app/endpointruntime.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>

#include <algorithm>

namespace {
constexpr quint8 Soh = 0x01;
constexpr quint8 Stx = 0x02;
constexpr quint8 Eot = 0x04;
constexpr quint8 Ack = 0x06;
constexpr quint8 Nak = 0x15;
constexpr quint8 Can = 0x18;
constexpr quint8 CrcRequest = 0x43;
}

FileTransferService::FileTransferService(QObject *parent) : QObject(parent)
{
    // 单发：每一包发出去之后再按 m_rawGapMs 排下一包。间隔可以是 0。
    m_sendTimer.setSingleShot(true);
    connect(&m_sendTimer, &QTimer::timeout, this, &FileTransferService::sendRawChunk);
    m_protocolTimer.setSingleShot(true);
    connect(&m_protocolTimer, &QTimer::timeout, this, [this] {
        if (!m_busy) return;
        if (m_state == State::PreWait) {
            enterBody();
            return;
        }
        if (m_state == State::PostWait) {
            if (!m_after.isEmpty()) sendBytes(m_after);
            complete(m_finishText);
            return;
        }
        if (m_state == State::RawSending || m_state == State::RawReceiving) {
            fail(tr("文件传输超时。"), false);
            return;
        }
        if (++m_retryCount > MaxRetries) {
            fail(tr("%1 超时，已超过最大重试次数。").arg(protocolName()));
            return;
        }
        switch (m_state) {
        case State::TxWaitStart:
            armProtocolTimeout();
            break;
        case State::TxWaitHeaderC:
            ++m_retries;
            sendTxHeader();
            publishStats();
            break;
        case State::TxWaitHeaderAck:
        case State::TxWaitDataAck:
        case State::TxWaitEotAck:
        case State::TxWaitEndAck:
            retryLastPacket();
            break;
        case State::TxWaitEndC:
            ++m_retries;
            sendTxEot();
            publishStats();
            break;
        case State::RxWaitHeader:
            // 先用 C 要 CRC。对端一直不应时再改发 NAK，兼容只认校验和的经典发送端。
            if (m_mode == Mode::XModem && !m_checkLocked && ++m_receiverPolls >= 2) {
                m_checksumMode = true;
                m_checkLocked = true;
                sendBytes(QByteArray(1, char(Nak)));
            } else {
                sendBytes(QByteArray(1, char(CrcRequest)));
            }
            armProtocolTimeout(ReceiverPollMs);
            break;
        case State::RxWaitData:
            ++m_retries;
            sendBytes(QByteArray(1, char(Nak)));
            publishStats();
            armProtocolTimeout();
            break;
        case State::RxWaitEot:
        case State::RxWaitEotSecond:
            ++m_retries;
            sendBytes(QByteArray(1, char(Nak)));
            publishStats();
            armProtocolTimeout();
            break;
        case State::RxWaitEndHeader:
            ++m_retries;
            sendBytes(QByteArray(1, char(CrcRequest)));
            publishStats();
            armProtocolTimeout();
            break;
        case State::RxPurgeStx:
            ++m_retries;
            m_input.clear();
            m_state = m_mode == Mode::XModem && m_expectedBlock == 1
                ? State::RxWaitHeader : State::RxWaitData;
            sendBytes(QByteArray(1, char(Nak)));
            publishStats();
            armProtocolTimeout();
            break;
        case State::Idle:
        case State::PreWait:
        case State::PostWait:
        case State::RawSending:
        case State::RawReceiving:
            break;
        }
    });
}

void FileTransferService::setRuntime(EndpointRuntime *runtime)
{
    if (m_runtime)
        disconnect(m_runtime, &EndpointRuntime::filePayloadAccepted, this, &FileTransferService::rawPayloadAccepted);
    m_runtime = runtime;
    if (runtime)
        connect(runtime, &EndpointRuntime::filePayloadAccepted, this, &FileTransferService::rawPayloadAccepted);
}

void FileTransferService::begin(int endpoint, Mode mode, State state, const FileTransferSides &sides)
{
    if (m_busy) {
        emit transferFailed(tr("已有文件传输正在进行。"));
        return;
    }
    if (!m_runtime || endpoint < 0 || endpoint >= EndpointRuntime::EndpointCount) {
        emit transferFailed(tr("端点运行时未就绪。"));
        return;
    }
    m_endpoint = endpoint;
    m_mode = mode;
    m_bodyState = state;
    m_state = state;
    m_busy = true;
    m_retryCount = 0;
    m_retries = 0;
    m_crcErrors = 0;
    m_receiverPolls = 0;
    m_input.clear();
    m_lastPacket.clear();
    m_pendingData.clear();
    m_dataPending = false;
    m_before = sides.before;
    m_after = sides.after;
    m_beforeDelayMs = qMax(0, sides.beforeDelayMs);
    m_afterDelayMs = qMax(0, sides.afterDelayMs);
    m_finishText.clear();
    m_transferred = 0;
    m_checksum.clear();
    m_inBody = false;
    m_clock.invalidate();
    m_rawPending.clear();
    m_rawRequest = {};
    m_nextBlock = 1;
    m_expectedBlock = 1;
    m_pendingData.clear();
    m_dataPending = false;
    m_checksumMode = false;
    m_checkLocked = false;
    emit busyChanged(true);
    emit transferStarted();
    publishStats();
}

void FileTransferService::beginAfterValidated()
{
    if (!m_busy) return;
    if (m_before.isEmpty() && m_beforeDelayMs <= 0) {
        enterBody();
        return;
    }
    if (!m_before.isEmpty()) {
        sendBytes(m_before);
        setStatus(tr("已发送前置指令，等待 %1 ms").arg(m_beforeDelayMs));
    } else {
        setStatus(tr("等待 %1 ms 后开始传输").arg(m_beforeDelayMs));
    }
    m_state = State::PreWait;
    m_protocolTimer.start(qMax(0, m_beforeDelayMs));
}

void FileTransferService::enterBody()
{
    if (!m_busy) return;
    m_input.clear();
    m_state = m_bodyState;
    m_inBody = true;
    switch (m_bodyState) {
    case State::RawSending:
        setStatus(tr("RAW 正在发送 %1").arg(QFileInfo(m_sourceFile).fileName()));
        publishStats();
        sendRawChunk();
        break;
    case State::RawReceiving:
        m_destinationFile.setFileName(m_destinationPath);
        if (!m_destinationFile.open(QIODevice::WriteOnly)) {
            fail(tr("无法创建接收文件：%1").arg(m_destinationPath), false);
            return;
        }
        setStatus(tr("RAW 正在接收，点击停止后保存文件"));
        emit progressChanged(0, 0);
        publishStats();
        break;
    case State::TxWaitStart:
        setStatus(tr("%1 等待接收端开始信号").arg(protocolName()));
        armProtocolTimeout();
        emit progressChanged(0, m_total);
        publishStats();
        break;
    case State::RxWaitHeader:
        setStatus(tr("%1 正在等待发送端").arg(protocolName()));
        sendBytes(QByteArray(1, char(CrcRequest)));
        armProtocolTimeout(ReceiverPollMs);
        emit progressChanged(0, m_mode == Mode::XModem ? m_receiveSize : 0);
        publishStats();
        break;
    default:
        break;
    }
}

void FileTransferService::succeed(const QString &text)
{
    if (!m_busy) return;
    m_sendTimer.stop();
    m_protocolTimer.stop();
    publishStats();
    m_inBody = false;
    if (m_after.isEmpty() && m_afterDelayMs <= 0) {
        complete(text);
        return;
    }
    m_finishText = text;
    m_state = State::PostWait;
    setStatus(m_afterDelayMs > 0
            ? tr("文件已完成，%1 ms 后发送后置指令").arg(m_afterDelayMs)
            : tr("文件已完成，正在发送后置指令"));
    m_protocolTimer.start(qMax(0, m_afterDelayMs));
}

void FileTransferService::noteFileBytes(const char *data, int size)
{
    m_checksum.feed(data, size);
}

void FileTransferService::noteInteraction()
{
    if (!m_inBody || m_clock.isValid()) return;
    m_clock.start();
}

void FileTransferService::publishStats()
{
    const qint64 elapsed = m_clock.isValid() ? m_clock.elapsed() : 0;
    emit statsChanged(m_transferred, m_total, m_retries, m_crcErrors, int(m_checksum.crc16),
        m_checksum.crc32(), elapsed);
}

void FileTransferService::reset()
{
    m_sendTimer.stop();
    m_protocolTimer.stop();
    m_rawPending.clear();
    m_rawRequest = {};
    if (m_sourceFile.isOpen()) m_sourceFile.close();
    if (m_destinationFile.isOpen()) m_destinationFile.cancelWriting();
    m_input.clear();
    m_lastPacket.clear();
    m_pendingData.clear();
    m_dataPending = false;
    m_before.clear();
    m_after.clear();
    m_beforeDelayMs = 0;
    m_afterDelayMs = 0;
    m_finishText.clear();
    m_endpoint = -1;
    m_total = 0;
    m_receiveSize = 0;
    m_transferred = 0;
    m_checksum.clear();
    m_inBody = false;
    m_clock.invalidate();
    m_state = State::Idle;
    m_bodyState = State::Idle;
}

void FileTransferService::setStatus(const QString &text)
{
    emit statusChanged(text);
}

void FileTransferService::sendBytes(const QByteArray &bytes)
{
    if (!m_runtime || bytes.isEmpty() || m_endpoint < 0) return;
    noteInteraction();
    QMetaObject::invokeMethod(m_runtime, "sendPayload", Qt::QueuedConnection,
        Q_ARG(int, m_endpoint), Q_ARG(QByteArray, bytes),
        Q_ARG(int, int(Traffic::Origin::Manual)));
}

void FileTransferService::sendCancel()
{
    sendBytes(QByteArray(2, char(Can)));
}

void FileTransferService::armProtocolTimeout(int interval)
{
    m_protocolTimer.start(interval);
}

void FileTransferService::stopProtocolTimers()
{
    m_sendTimer.stop();
    m_protocolTimer.stop();
}

void FileTransferService::fail(const QString &text, bool notifyPeer)
{
    if (!m_busy) return;
    if (notifyPeer && (m_mode == Mode::XModem || m_mode == Mode::YModem)) sendCancel();
    stopProtocolTimers();
    reset();
    m_busy = false;
    emit busyChanged(false);
    emit transferFailed(text);
}

void FileTransferService::complete(const QString &text)
{
    if (!m_busy) return;
    stopProtocolTimers();
    if (m_sourceFile.isOpen()) m_sourceFile.close();
    m_busy = false;
    m_state = State::Idle;
    emit progressChanged(m_total > 0 ? m_total : m_transferred, m_total);
    emit busyChanged(false);
    emit transferFinished(text);
    m_endpoint = -1;
    m_input.clear();
    m_lastPacket.clear();
    m_pendingData.clear();
    m_dataPending = false;
}

void FileTransferService::startRawSend(int endpoint, const QString &path,
    const FileTransferSides &sides, int chunkSize, int gapMs)
{
    m_rawChunkSize = std::clamp(chunkSize, 1, 64 * 1024);
    m_rawGapMs = std::clamp(gapMs, 0, 600000);
    begin(endpoint, Mode::Raw, State::RawSending, sides);
    if (!m_busy) return;
    m_sourceFile.setFileName(path);
    if (!m_sourceFile.open(QIODevice::ReadOnly)) {
        fail(tr("无法打开文件：%1").arg(path), false);
        return;
    }
    m_total = m_sourceFile.size();
    if (m_total <= 0 || m_total > MaxFileSize) {
        fail(m_total <= 0 ? tr("不能发送空文件。") : tr("文件超过 100 MB 限制。"), false);
        return;
    }
    beginAfterValidated();
}

void FileTransferService::startRawReceive(int endpoint, const QString &path, const FileTransferSides &sides)
{
    begin(endpoint, Mode::Raw, State::RawReceiving, sides);
    if (!m_busy) return;
    m_destinationPath = path;
    const QFileInfo info(path);
    if (info.fileName().isEmpty() || (info.dir().exists() == false && !QDir().mkpath(info.absolutePath()))) {
        fail(tr("无法创建接收文件：%1").arg(path), false);
        return;
    }
    beginAfterValidated();
}

void FileTransferService::sendRawChunk()
{
    if (!m_busy || m_state != State::RawSending || !m_rawRequest.isNull()) return;
    m_rawPending = m_sourceFile.read(m_rawChunkSize);
    if (m_sourceFile.error() != QFileDevice::NoError) {
        fail(tr("读取发送文件失败：%1").arg(m_sourceFile.errorString()), false);
        return;
    }
    if (m_rawPending.isEmpty()) {
        fail(tr("发送文件在传输期间发生变化，文件传输已停止。"), false);
        return;
    }
    // 只允许一块在途；本地 write 接受之前不累计进度，不读下一块，也不宣布完成。
    m_rawRequest = QUuid::createUuid();
    noteInteraction();
    armProtocolTimeout();
    if (!m_runtime || !QMetaObject::invokeMethod(m_runtime, "sendFilePayload", Qt::QueuedConnection,
            Q_ARG(int, m_endpoint), Q_ARG(QByteArray, m_rawPending), Q_ARG(QUuid, m_rawRequest)))
        fail(tr("端点运行时不可用，文件传输已停止。"), false);
}

void FileTransferService::rawPayloadAccepted(const QUuid &requestId, qint64 accepted)
{
    if (!m_busy || m_state != State::RawSending || requestId != m_rawRequest) return;
    m_rawRequest = {};
    m_protocolTimer.stop();
    const auto size = m_rawPending.size();
    const auto written = qBound<qint64>(0, accepted, size);
    noteFileBytes(m_rawPending.constData(), int(written));
    m_rawPending.clear();
    m_transferred += written;
    emit progressChanged(m_transferred, m_total);
    publishStats();
    if (accepted != size) {
        fail(tr("RAW 本地写入未完整接受（%1/%2 字节），文件传输已停止；慢速设备可增大发送间隔")
            .arg(written).arg(size), false);
        return;
    }
    // 最后一包后面不再空等，直接进入后置指令。间隔只落在两包之间。
    if (m_sourceFile.atEnd()) {
        if (m_transferred != m_total) {
            fail(tr("发送文件在传输期间发生变化，文件传输已停止。"), false);
            return;
        }
        succeed(tr("RAW 文件发送完成：%1").arg(QFileInfo(m_sourceFile).fileName()));
        return;
    }
    m_sendTimer.start(m_rawGapMs);
}

void FileTransferService::startXModemSend(int endpoint, const QString &path,
    const FileTransferSides &sides)
{
    begin(endpoint, Mode::XModem, State::TxWaitStart, sides);
    if (!m_busy) return;
    startYModemSendInternal(path, 128);
}

void FileTransferService::startXModemReceive(int endpoint, const QString &path,
    const FileTransferSides &sides)
{
    begin(endpoint, Mode::XModem, State::RxWaitHeader, sides);
    if (!m_busy) return;
    openReceiveFile(path);
    if (!m_busy) return;
    m_receiveSize = 0;
    m_receiverPolls = 0;
    beginAfterValidated();
}

void FileTransferService::startYModemSend(int endpoint, const QString &path, int blockSize,
    const FileTransferSides &sides)
{
    begin(endpoint, Mode::YModem, State::TxWaitStart, sides);
    if (!m_busy) return;
    startYModemSendInternal(path, blockSize == 128 ? 128 : 1024);
}

void FileTransferService::startYModemSendInternal(const QString &path, int blockSize)
{
    m_sourceFile.setFileName(path);
    if (!m_sourceFile.open(QIODevice::ReadOnly)) {
        fail(tr("无法打开文件：%1").arg(path), false);
        return;
    }
    m_total = m_sourceFile.size();
    if (m_total <= 0 || m_total > MaxFileSize) {
        fail(m_total <= 0 ? tr("不能发送空文件。") : tr("文件超过 100 MB 限制。"), false);
        return;
    }
    m_blockSize = blockSize;
    beginAfterValidated();
}

void FileTransferService::startYModemReceive(int endpoint, const QString &directory,
    const FileTransferSides &sides)
{
    begin(endpoint, Mode::YModem, State::RxWaitHeader, sides);
    if (!m_busy) return;
    startYModemReceiveInternal(directory);
}

void FileTransferService::startYModemReceiveInternal(const QString &directory)
{
    QDir dir(directory);
    if (!dir.exists() && !QDir().mkpath(directory)) {
        fail(tr("无法创建接收目录：%1").arg(directory), false);
        return;
    }
    m_receiveDirectory = dir.absolutePath();
    m_receiverPolls = 0;
    beginAfterValidated();
}

void FileTransferService::openReceiveFile(const QString &path)
{
    m_destinationPath = path;
    const QFileInfo info(path);
    if (info.fileName().isEmpty() || (info.dir().exists() == false && !QDir().mkpath(info.absolutePath()))) {
        fail(tr("无法创建接收文件：%1").arg(path), false);
        return;
    }
    m_destinationFile.setFileName(m_destinationPath);
    if (!m_destinationFile.open(QIODevice::WriteOnly))
        fail(tr("无法创建接收文件：%1").arg(m_destinationPath), false);
}

QString FileTransferService::protocolName() const
{
    return m_mode == Mode::XModem ? QStringLiteral("XMODEM") : QStringLiteral("YMODEM");
}

void FileTransferService::stop()
{
    if (!m_busy) return;
    if (m_state == State::PreWait) {
        fail(tr("文件传输已停止。"), false);
        return;
    }
    if (m_state == State::PostWait) {
        complete(m_finishText.isEmpty() ? tr("文件传输已停止。") : m_finishText);
        return;
    }
    if (m_state == State::RawReceiving) {
        if (!m_destinationFile.commit()) {
            fail(tr("接收文件保存失败。"), false);
            return;
        }
        const QString path = m_destinationPath;
        succeed(tr("RAW 接收已保存：%1").arg(path));
        return;
    }
    fail(tr("文件传输已停止。"));
}

void FileTransferService::endpointStateChanged(int index, int state, const QString &detail)
{
    Q_UNUSED(detail);
    if (m_busy && index == m_endpoint && state != int(Transport::State::Open))
        fail(tr("端点已断开，文件传输已停止。"), false);
}

void FileTransferService::rawDataReceived(int index, const Traffic::Source &source,
    const QByteArray &payload)
{
    Q_UNUSED(source);
    if (!m_busy || index != m_endpoint || payload.isEmpty()) return;
    if (m_state == State::PreWait || m_state == State::PostWait) return;
    noteInteraction();
    if (m_state == State::RawReceiving) {
        if (m_transferred + payload.size() > MaxFileSize) {
            fail(tr("接收文件超过 100 MB 限制。"), false);
            return;
        }
        if (m_destinationFile.write(payload) != payload.size()) {
            fail(tr("写入接收文件失败。"), false);
            return;
        }
        noteFileBytes(payload.constData(), payload.size());
        m_transferred += payload.size();
        emit progressChanged(m_transferred, 0);
        publishStats();
        return;
    }
    if (m_mode == Mode::XModem || m_mode == Mode::YModem) {
        m_input.append(payload);
        processIncoming();
    }
}

void FileTransferService::processIncoming()
{
    while (m_busy && !m_input.isEmpty()) {
        if (quint8(m_input.at(0)) == Can) {
            m_input.remove(0, 1);
            fail(tr("对端取消了 %1 传输。").arg(protocolName()), false);
            return;
        }
        if (m_state == State::TxWaitStart || m_state == State::TxWaitHeaderAck
            || m_state == State::TxWaitHeaderC || m_state == State::TxWaitDataAck
            || m_state == State::TxWaitEotAck || m_state == State::TxWaitEndC
            || m_state == State::TxWaitEndAck) {
            const quint8 byte = quint8(m_input.at(0));
            m_input.remove(0, 1);
            processTxControl(byte);
            continue;
        }
        if (m_input.at(0) == char(Eot)) {
            m_input.remove(0, 1);
            handleRxEot();
            continue;
        }
        if (m_mode == Mode::XModem && m_input.at(0) == char(Stx)) {
            // 1K 包固定按 CRC 长度丢掉。校验和模式也不收 1024 字节块。
            const int stxSize = 1029;
            if (m_input.size() < stxSize && m_state != State::RxPurgeStx) {
                m_state = State::RxPurgeStx;
                armProtocolTimeout(200);
                return;
            }
            if (m_input.size() < stxSize) return;
            m_input.remove(0, stxSize);
            ++m_crcErrors;
            sendBytes(QByteArray(1, char(Nak)));
            publishStats();
            armProtocolTimeout();
            continue;
        }
        if (m_input.at(0) != char(Soh) && !(m_mode == Mode::YModem && m_input.at(0) == char(Stx))) {
            m_input.remove(0, 1);
            continue;
        }
        // 经典发送端只认 NAK，第一包就是 132 字节校验和帧，不会先回 C。
        // 132 字节齐了就锁定校验和；133 字节且末两字节是 CRC 则锁定 CRC。
        if (m_mode == Mode::XModem && !m_checkLocked && m_input.at(0) == char(Soh)) {
            if (m_input.size() < 132) return;
            if (m_input.size() == 132 || !crcMatches(m_input.mid(3, 128), m_input.mid(131, 2))) {
                m_checksumMode = true;
            }
            m_checkLocked = true;
        }
        quint8 start = 0;
        quint8 block = 0;
        QByteArray data;
        bool valid = false;
        if (!takePacket(start, block, data, valid)) return;
        processRxPacket(start, block, data, valid);
    }
}

void FileTransferService::processTxControl(quint8 byte)
{
    switch (m_state) {
    case State::TxWaitStart:
        // C 锁定 CRC，NAK 锁定 1 字节校验和。两者都直接要第一包数据，没有文件头。
        // YMODEM 只认 C。
        if (m_mode == Mode::XModem && (byte == CrcRequest || byte == Nak)) {
            m_checksumMode = byte == Nak;
            m_checkLocked = true;
            sendTxData();
        } else if (byte == CrcRequest) {
            sendTxHeader();
        }
        break;
    case State::TxWaitHeaderAck:
        if (byte == Ack) {
            m_state = State::TxWaitHeaderC;
            m_retryCount = 0;
            setStatus(tr("YMODEM 等待数据开始信号"));
            armProtocolTimeout();
        }
        break;
    case State::TxWaitHeaderC:
        if (byte == CrcRequest) sendTxData();
        break;
    case State::TxWaitDataAck:
        if (byte == Ack) {
            m_retryCount = 0;
            m_dataPending = false;
            m_pendingData.clear();
            if (m_transferred >= m_total) sendTxEot();
            else sendTxData();
        } else if (byte == Nak) {
            ++m_crcErrors;
            retryLastPacket();
        }
        break;
    case State::TxWaitEotAck:
        if (m_mode == Mode::XModem) {
            if (byte == Ack) succeed(tr("XMODEM 文件发送完成"));
            else if (byte == Nak) retryLastPacket();
            break;
        }
        if (byte == Nak) {
            sendBytes(m_lastPacket);
            armProtocolTimeout();
        } else if (byte == Ack) {
            m_state = State::TxWaitEndC;
            m_retryCount = 0;
            setStatus(tr("YMODEM 等待结束确认信号"));
            armProtocolTimeout();
        }
        break;
    case State::TxWaitEndC:
        if (byte == CrcRequest) sendTxEndHeader();
        break;
    case State::TxWaitEndAck:
        if (byte == Ack) succeed(tr("YMODEM 文件发送完成"));
        break;
    default:
        break;
    }
}

void FileTransferService::processRxPacket(quint8 start, quint8 block, const QByteArray &data,
    bool valid)
{
    Q_UNUSED(start);
    if (m_mode == Mode::XModem && m_state == State::RxWaitHeader) {
        if (start != Soh) {
            ++m_crcErrors;
            sendBytes(QByteArray(1, char(Nak)));
            publishStats();
            armProtocolTimeout();
            return;
        }
        // 第一包就是数据，块号从 1 开始。没有文件头。
        m_state = State::RxWaitData;
        m_expectedBlock = 1;
        m_retryCount = 0;
        setStatus(tr("XMODEM 正在接收 %1").arg(QFileInfo(m_destinationPath).fileName()));
        handleRxData(block, data, valid);
        return;
    }
    if (m_state == State::RxWaitHeader || m_state == State::RxWaitEndHeader) {
        handleRxHeader(data, valid);
    } else if (m_state == State::RxWaitData) {
        if (m_mode == Mode::XModem && start != Soh) {
            ++m_crcErrors;
            sendBytes(QByteArray(1, char(Nak)));
            publishStats();
            armProtocolTimeout();
            return;
        }
        handleRxData(block, data, valid);
    }
}

bool FileTransferService::takePacket(quint8 &start, quint8 &block, QByteArray &data, bool &valid)
{
    if (m_input.isEmpty()) return false;
    start = quint8(m_input.at(0));
    const int blockSize = start == Stx ? 1024 : 128;
    // 校验和只用于 XMODEM 的 128 字节包。YMODEM 和 1K 包仍是 2 字节 CRC。
    const int checkBytes = (m_checksumMode && m_mode == Mode::XModem && start == Soh) ? 1 : 2;
    const int packetSize = 3 + blockSize + checkBytes;
    if (m_input.size() < packetSize) return false;
    const QByteArray packet = m_input.left(packetSize);
    m_input.remove(0, packetSize);
    block = quint8(packet.at(1));
    const quint8 inverse = quint8(packet.at(2));
    data = packet.mid(3, blockSize);
    bool checkOk = false;
    if (checkBytes == 1) {
        checkOk = quint8(packet.at(packetSize - 1)) == checksum8(data);
    } else {
        const quint16 received = (quint16(quint8(packet.at(packetSize - 2))) << 8)
            | quint8(packet.at(packetSize - 1));
        checkOk = crc16(data) == received;
    }
    valid = (quint8(block ^ inverse) == 0xff) && checkOk;
    return true;
}

void FileTransferService::sendTxHeader()
{
    const QByteArray fileName = QFileInfo(m_sourceFile).fileName().toUtf8().left(100);
    QByteArray header;
    header.append(fileName);
    header.append(char(0));
    header.append(QByteArray::number(m_total));
    m_lastPacket = makePacket(0, header, 128, 0);
    sendBytes(m_lastPacket);
    m_state = State::TxWaitHeaderAck;
    m_retryCount = 0;
    setStatus(tr("YMODEM 正在发送文件头"));
    armProtocolTimeout();
}

void FileTransferService::sendTxData()
{
    if (!m_dataPending) {
        const QByteArray data = m_sourceFile.read(m_blockSize);
        if (data.isEmpty() && m_transferred < m_total) {
            fail(tr("读取文件失败。"));
            return;
        }
        m_pendingData = data;
        m_dataPending = true;
        if (!data.isEmpty()) noteFileBytes(data.constData(), data.size());
        m_transferred += data.size();
        ++m_nextBlock;
    }
    // 重传用同一块号和同一段数据。块号在第一次读出时已经前进。
    m_lastPacket = makePacket(quint8(m_nextBlock - 1), m_pendingData, m_blockSize, 0x1a);
    sendBytes(m_lastPacket);
    m_state = State::TxWaitDataAck;
    m_retryCount = 0;
    emit progressChanged(m_transferred, m_total);
    publishStats();
    setStatus(tr("%1 正在发送数据：%2/%3").arg(protocolName()).arg(m_transferred).arg(m_total));
    armProtocolTimeout();
}

void FileTransferService::sendTxEot()
{
    m_lastPacket = QByteArray(1, char(Eot));
    sendBytes(m_lastPacket);
    m_state = State::TxWaitEotAck;
    m_retryCount = 0;
    setStatus(tr("%1 正在结束传输").arg(protocolName()));
    armProtocolTimeout();
}

void FileTransferService::sendTxEndHeader()
{
    m_lastPacket = makePacket(0, {}, 128, 0);
    sendBytes(m_lastPacket);
    m_state = State::TxWaitEndAck;
    m_retryCount = 0;
    armProtocolTimeout();
}

void FileTransferService::retryLastPacket()
{
    if (m_lastPacket.isEmpty()) return;
    ++m_retries;
    sendBytes(m_lastPacket);
    publishStats();
    armProtocolTimeout();
}

void FileTransferService::handleRxHeader(const QByteArray &data, bool valid)
{
    if (!valid) {
        ++m_crcErrors;
        sendBytes(QByteArray(1, char(Nak)));
        publishStats();
        armProtocolTimeout();
        return;
    }
    const int nameEnd = data.indexOf(char(0));
    if (nameEnd <= 0) {
        if (m_state == State::RxWaitEndHeader) {
            if (m_destinationFile.isOpen()) commitReceivedFile();
            sendBytes(QByteArray(1, char(Ack)));
            if (m_busy) succeed(tr("YMODEM 文件接收完成"));
        } else {
            sendBytes(QByteArray(1, char(Ack)));
            succeed(tr("YMODEM 未收到文件头"));
        }
        return;
    }
    const QString name = safeFileName(data.left(nameEnd));
    const int sizeStart = nameEnd + 1;
    const int sizeEnd = data.indexOf(char(0), sizeStart);
    bool ok = false;
    const qint64 size = data.mid(sizeStart, sizeEnd < 0 ? -1 : sizeEnd - sizeStart).trimmed()
                            .toLongLong(&ok);
    if (name.isEmpty() || !ok || size < 0 || size > MaxFileSize) {
        fail(tr("YMODEM 文件头无效。"));
        return;
    }
    m_destinationPath = QDir(m_receiveDirectory).filePath(name);
    m_destinationFile.setFileName(m_destinationPath);
    if (!m_destinationFile.open(QIODevice::WriteOnly)) {
        fail(tr("无法创建接收文件：%1").arg(m_destinationPath));
        return;
    }
    m_receiveSize = size;
    m_transferred = 0;
    m_expectedBlock = 1;
    m_state = State::RxWaitData;
    m_retryCount = 0;
    QByteArray ackAndCrc;
    ackAndCrc.append(char(Ack));
    ackAndCrc.append(char(CrcRequest));
    sendBytes(ackAndCrc);
    setStatus(tr("YMODEM 正在接收 %1").arg(name));
    emit progressChanged(0, m_receiveSize);
    armProtocolTimeout();
}

void FileTransferService::handleRxData(quint8 block, const QByteArray &data, bool valid)
{
    if (!valid) {
        ++m_crcErrors;
        sendBytes(QByteArray(1, char(Nak)));
        publishStats();
        armProtocolTimeout();
        return;
    }
    if (block == m_expectedBlock) {
        qint64 count = data.size();
        if (m_mode == Mode::YModem) {
            const qint64 remaining = qMax<qint64>(0, m_receiveSize - m_transferred);
            count = qMin<qint64>(remaining, data.size());
        } else if (m_transferred + count > MaxFileSize) {
            // XMODEM 不传长度，末包的 0x1A 无法和文件内容区分，整块都写入。
            fail(tr("接收文件超过 100 MB 限制。"));
            return;
        }
        if (count > 0 && m_destinationFile.write(data.constData(), count) != count) {
            fail(tr("写入接收文件失败。"));
            return;
        }
        if (count > 0) noteFileBytes(data.constData(), int(count));
        m_transferred += count;
        ++m_expectedBlock;
        sendBytes(QByteArray(1, char(Ack)));
        emit progressChanged(m_transferred, m_mode == Mode::YModem ? m_receiveSize : 0);
        publishStats();
        if (m_mode == Mode::YModem && m_transferred >= m_receiveSize) m_state = State::RxWaitEot;
        armProtocolTimeout();
    } else if (block == quint8(m_expectedBlock - 1)) {
        sendBytes(QByteArray(1, char(Ack)));
        armProtocolTimeout();
    } else {
        sendBytes(QByteArray(1, char(Nak)));
        armProtocolTimeout();
    }
}

void FileTransferService::handleRxEot()
{
    if (m_mode == Mode::XModem) {
        if (m_state != State::RxWaitData && m_state != State::RxWaitEot) return;
        sendBytes(QByteArray(1, char(Ack)));
        commitReceivedFile();
        if (m_busy) succeed(tr("XMODEM 文件接收完成"));
        return;
    }
    if (m_state == State::RxWaitData || m_state == State::RxWaitEot) {
        sendBytes(QByteArray(1, char(Nak)));
        m_state = State::RxWaitEotSecond;
        m_retryCount = 0;
        armProtocolTimeout();
    } else if (m_state == State::RxWaitEotSecond) {
        QByteArray ackAndCrc;
        ackAndCrc.append(char(Ack));
        ackAndCrc.append(char(CrcRequest));
        sendBytes(ackAndCrc);
        m_state = State::RxWaitEndHeader;
        m_retryCount = 0;
        armProtocolTimeout();
    }
}

void FileTransferService::commitReceivedFile()
{
    if (!m_destinationFile.isOpen()) return;
    if (!m_destinationFile.commit()) {
        fail(tr("接收文件保存失败。"), false);
        return;
    }
}

QByteArray FileTransferService::makePacket(quint8 block, const QByteArray &data, int blockSize,
    quint8 fillByte) const
{
    QByteArray packet;
    packet.reserve(blockSize + 5);
    packet.append(char(blockSize == 1024 ? Stx : Soh));
    packet.append(char(block));
    packet.append(char(~block));
    packet.append(data);
    if (data.size() < blockSize) packet.append(QByteArray(blockSize - data.size(), char(fillByte)));
    const QByteArray body = packet.mid(3);
    if (m_checksumMode) {
        packet.append(char(checksum8(body)));
    } else {
        const quint16 crc = crc16(body);
        packet.append(char((crc >> 8) & 0xff));
        packet.append(char(crc & 0xff));
    }
    return packet;
}

quint8 FileTransferService::checksum8(const QByteArray &data)
{
    quint8 sum = 0;
    for (char byte : data) sum = quint8(sum + quint8(byte));
    return sum;
}

bool FileTransferService::crcMatches(const QByteArray &data, const QByteArray &received)
{
    if (received.size() != 2) return false;
    const quint16 value = (quint16(quint8(received.at(0))) << 8) | quint8(received.at(1));
    return crc16(data) == value;
}

quint16 FileTransferService::crc16(const QByteArray &data)
{
    PayloadChecksum sum;
    sum.feed(data.constData(), data.size());
    return sum.crc16;
}

QString FileTransferService::safeFileName(const QByteArray &rawName)
{
    QString name = QString::fromUtf8(rawName).trimmed();
    if (name.isEmpty()) name = QString::fromLocal8Bit(rawName).trimmed();
    name = QFileInfo(name).fileName();
    if (name == QStringLiteral(".") || name == QStringLiteral("..")) return {};
    name.remove(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")));
    return name.left(180);
}
