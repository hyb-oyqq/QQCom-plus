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
            fail(tr("YMODEM 超时，已超过最大重试次数。"));
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
            sendBytes(QByteArray(1, char(CrcRequest)));
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
    m_runtime = runtime;
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
    m_before = sides.before;
    m_after = sides.after;
    m_beforeDelayMs = qMax(0, sides.beforeDelayMs);
    m_afterDelayMs = qMax(0, sides.afterDelayMs);
    m_finishText.clear();
    m_transferred = 0;
    m_checksum.clear();
    m_inBody = false;
    m_clock.invalidate();
    m_nextBlock = 1;
    m_expectedBlock = 1;
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
        setStatus(tr("YMODEM 等待接收端开始信号"));
        armProtocolTimeout();
        emit progressChanged(0, m_total);
        publishStats();
        break;
    case State::RxWaitHeader:
        setStatus(tr("YMODEM 正在等待发送端"));
        sendBytes(QByteArray(1, char(CrcRequest)));
        armProtocolTimeout(ReceiverPollMs);
        emit progressChanged(0, 0);
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

void PayloadChecksum::clear()
{
    crc16 = 0;
    m_crc32 = 0xffffffffu;
}

void PayloadChecksum::feed(const char *data, int size)
{
    if (!data || size <= 0) return;
    for (int i = 0; i < size; ++i) {
        const quint8 byte = quint8(data[i]);
        crc16 ^= quint16(byte) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc16 = (crc16 & 0x8000)
                ? quint16((crc16 << 1) ^ 0x1021)
                : quint16(crc16 << 1);
        }
        static const quint32 table[256] = {
            0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu,
            0xe963a535u, 0x9e6495a3u, 0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u,
            0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u, 0x1db71064u, 0x6ab020f2u,
            0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
            0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u,
            0xfa0f3d63u, 0x8d080df5u, 0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u,
            0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu, 0x35b5a8fau, 0x42b2986cu,
            0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
            0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u,
            0xcfba9599u, 0xb8bda50fu, 0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u,
            0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du, 0x76dc4190u, 0x01db7106u,
            0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
            0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du,
            0x91646c97u, 0xe6635c01u, 0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu,
            0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u, 0x65b0d9c6u, 0x12b7e950u,
            0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
            0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u,
            0xa4d1c46du, 0xd3d6f4fbu, 0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u,
            0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u, 0x5005713cu, 0x270241aau,
            0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
            0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u,
            0xb7bd5c3bu, 0xc0ba6cadu, 0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au,
            0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u, 0xe3630b12u, 0x94643b84u,
            0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
            0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu,
            0x196c3671u, 0x6e6b06e7u, 0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu,
            0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u, 0xd6d6a3e8u, 0xa1d1937eu,
            0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
            0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u,
            0x316e8eefu, 0x4669be79u, 0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u,
            0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu, 0xc5ba3bbeu, 0xb2bd0b28u,
            0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
            0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu,
            0x72076785u, 0x05005713u, 0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u,
            0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u, 0x86d3d2d4u, 0xf1d4e242u,
            0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
            0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u,
            0x616bffd3u, 0x166ccf45u, 0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u,
            0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu, 0xaed16a4au, 0xd9d65adcu,
            0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
            0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u,
            0x54de5729u, 0x23d967bfu, 0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u,
            0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du};
        m_crc32 = table[(m_crc32 ^ byte) & 0xff] ^ (m_crc32 >> 8);
    }
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
    if (m_sourceFile.isOpen()) m_sourceFile.close();
    if (m_destinationFile.isOpen()) m_destinationFile.cancelWriting();
    m_input.clear();
    m_lastPacket.clear();
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
    if (notifyPeer && m_mode == Mode::YModem) sendCancel();
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
    if (!m_busy || m_state != State::RawSending) return;
    const QByteArray chunk = m_sourceFile.read(m_rawChunkSize);
    if (chunk.isEmpty()) {
        succeed(tr("RAW 文件发送完成：%1").arg(QFileInfo(m_sourceFile).fileName()));
        return;
    }
    sendBytes(chunk);
    noteFileBytes(chunk.constData(), chunk.size());
    m_transferred += chunk.size();
    emit progressChanged(m_transferred, m_total);
    publishStats();
    // 最后一包后面不再空等，直接进入后置指令。间隔只落在两包之间。
    if (m_sourceFile.atEnd()) {
        succeed(tr("RAW 文件发送完成：%1").arg(QFileInfo(m_sourceFile).fileName()));
        return;
    }
    m_sendTimer.start(m_rawGapMs);
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
    if (m_mode == Mode::YModem) {
        m_input.append(payload);
        processIncoming();
    }
}

void FileTransferService::processIncoming()
{
    while (m_busy && !m_input.isEmpty()) {
        if (quint8(m_input.at(0)) == Can) {
            m_input.remove(0, 1);
            fail(tr("对端取消了 YMODEM 传输。"), false);
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
        if (m_input.at(0) != char(Soh) && m_input.at(0) != char(Stx)) {
            m_input.remove(0, 1);
            continue;
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
        if (byte == CrcRequest) sendTxHeader();
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
            if (m_transferred >= m_total) sendTxEot();
            else sendTxData();
        } else if (byte == Nak) {
            ++m_crcErrors;
            retryLastPacket();
        }
        break;
    case State::TxWaitEotAck:
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
    if (m_state == State::RxWaitHeader || m_state == State::RxWaitEndHeader) {
        handleRxHeader(data, valid);
    } else if (m_state == State::RxWaitData) {
        handleRxData(block, data, valid);
    }
}

bool FileTransferService::takePacket(quint8 &start, quint8 &block, QByteArray &data, bool &valid)
{
    if (m_input.isEmpty()) return false;
    start = quint8(m_input.at(0));
    const int blockSize = start == Stx ? 1024 : 128;
    const int packetSize = 3 + blockSize + 2;
    if (m_input.size() < packetSize) return false;
    const QByteArray packet = m_input.left(packetSize);
    m_input.remove(0, packetSize);
    block = quint8(packet.at(1));
    const quint8 inverse = quint8(packet.at(2));
    data = packet.mid(3, blockSize);
    const quint16 received = (quint16(quint8(packet.at(packetSize - 2))) << 8)
        | quint8(packet.at(packetSize - 1));
    valid = (quint8(block ^ inverse) == 0xff) && crc16(data) == received;
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
    const QByteArray data = m_sourceFile.read(m_blockSize);
    if (data.isEmpty() && m_transferred < m_total) {
        fail(tr("读取文件失败。"));
        return;
    }
    if (!data.isEmpty()) noteFileBytes(data.constData(), data.size());
    m_lastPacket = makePacket(m_nextBlock, data, m_blockSize, 0x1a);
    sendBytes(m_lastPacket);
    m_transferred += data.size();
    ++m_nextBlock;
    m_state = State::TxWaitDataAck;
    m_retryCount = 0;
    emit progressChanged(m_transferred, m_total);
    publishStats();
    setStatus(tr("YMODEM 正在发送数据：%1/%2").arg(m_transferred).arg(m_total));
    armProtocolTimeout();
}

void FileTransferService::sendTxEot()
{
    m_lastPacket = QByteArray(1, char(Eot));
    sendBytes(m_lastPacket);
    m_state = State::TxWaitEotAck;
    m_retryCount = 0;
    setStatus(tr("YMODEM 正在结束传输"));
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
        const qint64 remaining = qMax<qint64>(0, m_receiveSize - m_transferred);
        const qint64 count = qMin<qint64>(remaining, data.size());
        if (count > 0 && m_destinationFile.write(data.constData(), count) != count) {
            fail(tr("写入接收文件失败。"));
            return;
        }
        if (count > 0) noteFileBytes(data.constData(), int(count));
        m_transferred += count;
        ++m_expectedBlock;
        sendBytes(QByteArray(1, char(Ack)));
        emit progressChanged(m_transferred, m_receiveSize);
        publishStats();
        if (m_transferred >= m_receiveSize) m_state = State::RxWaitEot;
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
    quint8 fillByte)
{
    QByteArray packet;
    packet.reserve(blockSize + 5);
    packet.append(char(blockSize == 1024 ? Stx : Soh));
    packet.append(char(block));
    packet.append(char(~block));
    packet.append(data);
    if (data.size() < blockSize) packet.append(QByteArray(blockSize - data.size(), char(fillByte)));
    const quint16 crc = crc16(packet.mid(3));
    packet.append(char((crc >> 8) & 0xff));
    packet.append(char(crc & 0xff));
    return packet;
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
