#ifndef FILETRANSFERSERVICE_H
#define FILETRANSFERSERVICE_H

#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QSaveFile>
#include <QTimer>

#include "domain/trafficmessage.h"

class EndpointRuntime;

// 文件载荷校验。CRC16 为 XMODEM/CCITT（初值 0，与 YMODEM 包内算法相同，但是从文件头连续计算）。
// CRC32 为同一段字节的 IEEE 值。两者都不含协议头、填充和前置/后置指令。
struct PayloadChecksum
{
    quint16 crc16 = 0;

    quint32 crc32() const { return m_crc32 ^ 0xffffffffu; }
    void feed(const char *data, int size);
    void clear();

private:
    quint32 m_crc32 = 0xffffffffu;
};

// 文件正式开传前后各可选一段原始字节。等待期间到达的字节不进入文件，
// 也不进入 YMODEM 状态机，避免把指令回显当成文件内容或协议控制字。
struct FileTransferSides
{
    QByteArray before;
    int beforeDelayMs = 0;
    QByteArray after;
    int afterDelayMs = 0;
};

// 文件传输协议层。它只依赖 EndpointRuntime 的原始字节入口，因而不会把
// YMODEM 的控制字节混入 ReceiveFramer，也不要求传输层知道协议细节。
class FileTransferService final : public QObject
{
    Q_OBJECT

public:
    enum class Mode { Raw, YModem };

    explicit FileTransferService(QObject *parent = nullptr);

    void setRuntime(EndpointRuntime *runtime);
    bool isBusy() const { return m_busy; }

public slots:
    // chunkSize、gapMs 只用于 RAW 发送。chunkSize 是每次连续写出的字节数。
    // gapMs 是发完一包后再发下一包的等待，0 表示马上发下一包；第一包不等这一下。
    // YMODEM 仍用自己的 128/1024 块，不用这两个参数。
    void startRawSend(int endpoint, const QString &path, const FileTransferSides &sides = {},
        int chunkSize = 16 * 1024, int gapMs = 0);
    void startRawReceive(int endpoint, const QString &path, const FileTransferSides &sides = {});
    void startYModemSend(int endpoint, const QString &path, int blockSize,
        const FileTransferSides &sides = {});
    void startYModemReceive(int endpoint, const QString &directory,
        const FileTransferSides &sides = {});
    void stop();
    void endpointStateChanged(int index, int state, const QString &detail);
    void rawDataReceived(int index, const Traffic::Source &source, const QByteArray &payload);

signals:
    void statusChanged(const QString &text);
    void progressChanged(qint64 current, qint64 total);
    // CRC16 是文件载荷的 XMODEM/CCITT（初值 0，与 YMODEM 包内算法相同，但是跨整文件连续计算）。
    // CRC32 是同一段载荷的 IEEE 值。两者都不包含协议头、填充和前置/后置指令。
    // elapsedMs 从文件阶段第一次发出或收到字节起算，不含前置等待，也不含对端尚未响应时的空等。
    void statsChanged(qint64 transferred, qint64 total, int retries, int crcErrors,
        int payloadCrc16, quint32 payloadCrc32, qint64 elapsedMs);
    void transferStarted();
    void transferFinished(const QString &text);
    void transferFailed(const QString &text);
    void busyChanged(bool busy);

private:
    enum class State {
        Idle,
        PreWait,
        PostWait,
        RawSending,
        RawReceiving,
        TxWaitStart,
        TxWaitHeaderAck,
        TxWaitHeaderC,
        TxWaitDataAck,
        TxWaitEotAck,
        TxWaitEndC,
        TxWaitEndAck,
        RxWaitHeader,
        RxWaitData,
        RxWaitEot,
        RxWaitEotSecond,
        RxWaitEndHeader
    };

    static constexpr qint64 MaxFileSize = 100 * 1024 * 1024;
    static constexpr int ProtocolTimeoutMs = 10000;
    static constexpr int ReceiverPollMs = 3000;
    static constexpr int MaxRetries = 8;

    void begin(int endpoint, Mode mode, State state, const FileTransferSides &sides);
    void beginAfterValidated();
    void enterBody();
    void succeed(const QString &text);
    void reset();
    void setStatus(const QString &text);
    void noteFileBytes(const char *data, int size);
    void noteInteraction();
    void publishStats();
    void sendBytes(const QByteArray &bytes);
    void sendCancel();
    void armProtocolTimeout(int interval = ProtocolTimeoutMs);
    void stopProtocolTimers();
    void fail(const QString &text, bool notifyPeer = true);
    void complete(const QString &text);

    void sendRawChunk();
    void startYModemSendInternal(const QString &path, int blockSize);
    void startYModemReceiveInternal(const QString &directory);
    void processIncoming();
    void processTxControl(quint8 byte);
    void processRxPacket(quint8 start, quint8 block, const QByteArray &data, bool valid);
    bool takePacket(quint8 &start, quint8 &block, QByteArray &data, bool &valid);
    void sendTxHeader();
    void sendTxData();
    void sendTxEot();
    void sendTxEndHeader();
    void retryLastPacket();
    void handleRxHeader(const QByteArray &data, bool valid);
    void handleRxData(quint8 block, const QByteArray &data, bool valid);
    void handleRxEot();
    void commitReceivedFile();

    static QByteArray makePacket(quint8 block, const QByteArray &data, int blockSize,
        quint8 fillByte);
    static quint16 crc16(const QByteArray &data);
    static QString safeFileName(const QByteArray &rawName);

    EndpointRuntime *m_runtime = nullptr;
    State m_state = State::Idle;
    State m_bodyState = State::Idle;
    Mode m_mode = Mode::Raw;
    int m_endpoint = -1;
    int m_blockSize = 1024;
    int m_rawChunkSize = 16 * 1024;
    int m_rawGapMs = 0;
    bool m_busy = false;
    int m_retryCount = 0;
    int m_retries = 0;
    int m_crcErrors = 0;
    quint8 m_nextBlock = 1;
    quint8 m_expectedBlock = 1;
    QByteArray m_input;
    QByteArray m_lastPacket;
    QByteArray m_before;
    QByteArray m_after;
    int m_beforeDelayMs = 0;
    int m_afterDelayMs = 0;
    QString m_finishText;
    QFile m_sourceFile;
    QSaveFile m_destinationFile;
    QString m_destinationPath;
    QString m_receiveDirectory;
    qint64 m_total = 0;
    qint64 m_transferred = 0;
    qint64 m_receiveSize = 0;
    PayloadChecksum m_checksum;
    bool m_inBody = false;
    QElapsedTimer m_clock;
    QTimer m_sendTimer;
    QTimer m_protocolTimer;
    int m_receiverPolls = 0;
};

#endif // FILETRANSFERSERVICE_H
