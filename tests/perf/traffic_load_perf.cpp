// 有换行的百万行历史 + 跨线程逐条投递；独立临时偏好与日志，不读取用户配置。
// 参数：历史条数、每秒速率、持续秒数、载荷字节数、记录开关。
#include "ui/panels/traffic/trafficpanel.h"
#include "persistence/preferencesstore.h"
#include "app/endpointruntime.h"
#include "app/filetransferservice.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTimer>
#include <QThread>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
#include <windows.h>
#include <psapi.h>

static double memoryMiB()
{
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    return counters.WorkingSetSize / 1048576.0;
}

// 真正的 RAW 文件发送服务 → I/O 线程 TCP → 独立线程接收并 SHA-256 对账。
static int transferProbe(TrafficPanel &panel, const QString &directory, bool logging)
{
    const QString path = directory + "/payload.txt";
    QFile source(path);
    if (!source.open(QIODevice::WriteOnly)) return 2;
    QByteArray unit(127, 'a');
    unit += '\n';
    QByteArray payload;
    for (int i = 0; i < 8192; ++i) payload += unit;
    QCryptographicHash sourceHash(QCryptographicHash::Sha256);
    for (int i = 0; i < 32; ++i) { source.write(payload); sourceHash.addData(payload); }
    source.close();
    constexpr qint64 FileBytes = 32LL * 1024 * 1024;
    std::promise<quint16> listening;
    std::atomic<qint64> received{0};
    std::atomic<bool> peerDone{false};
    QByteArray receivedHash;
    std::thread peer([&] {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) { listening.set_value(0); peerDone = true; return; }
        listening.set_value(server.serverPort());
        if (server.waitForNewConnection(15000)) {
            auto *socket = server.nextPendingConnection();
            QCryptographicHash hash(QCryptographicHash::Sha256);
            while (received < FileBytes && socket->state() == QAbstractSocket::ConnectedState) {
                if (socket->bytesAvailable() == 0 && !socket->waitForReadyRead(15000)) break;
                const auto data = socket->readAll();
                received += data.size();
                hash.addData(data);
            }
            receivedHash = hash.result();
        }
        peerDone = true;
    });
    const quint16 portNumber = listening.get_future().get();
    auto *runtime = new EndpointRuntime;
    QThread io;
    runtime->moveToThread(&io);
    QObject::connect(&io, &QThread::finished, runtime, &QObject::deleteLater);
    QObject::connect(runtime, &EndpointRuntime::messageReceived, &panel, &TrafficPanel::appendMessage);
    bool open = false;
    QObject::connect(runtime, &EndpointRuntime::endpointStateChanged, &panel,
        [&](int, int state, const QString &) { open = state == int(Transport::State::Open); });
    io.start();
    PortDraft port;
    port.kind = PortDraft::TcpClient;
    port.values["client.host"] = "127.0.0.1";
    port.values["client.port"] = portNumber;
    panel.setPorts({port});
    QMetaObject::invokeMethod(runtime, [=] { runtime->openEndpoint(0, port); }, Qt::QueuedConnection);
    QElapsedTimer timer;
    timer.start();
    while (!open && timer.elapsed() < 10000) QTest::qWait(1);
    FileTransferService transfer;
    transfer.setRuntime(runtime);
    bool failed = !open;
    QObject::connect(&transfer, &FileTransferService::transferFailed, &panel,
        [&](const QString &reason) { std::fprintf(stderr, "%s\n", qPrintable(reason)); failed = true; });
    auto *record = panel.findChild<QAction *>("recordTraffic");
    record->setChecked(logging);
    QVector<double> lags;
    QElapsedTimer tick;
    tick.start();
    QTimer heartbeat;
    heartbeat.setInterval(10);
    QObject::connect(&heartbeat, &QTimer::timeout, &panel, [&] {
        lags.append(qMax(0.0, tick.nsecsElapsed() / 1e6 - 10)); tick.restart();
    });
    heartbeat.start();
    timer.restart();
    if (open) transfer.startRawSend(0, path, {}, 16 * 1024, 0);
    while (timer.elapsed() < 120000 && (!peerDone || transfer.isBusy()))
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    const qint64 elapsed = timer.elapsed();
    transfer.stop();
    QMetaObject::invokeMethod(runtime, &EndpointRuntime::shutdown, Qt::BlockingQueuedConnection);
    io.quit(); io.wait();
    peer.join();
    QTest::qWait(100);
    record->setChecked(false);
    heartbeat.stop();
    std::sort(lags.begin(), lags.end());
    const bool hashOk = receivedHash == sourceHash.result();
    std::printf("transfer_log=%d bytes=%lld expected=%lld sha256_ok=%d total_ms=%lld MiB_s=%.2f lag_p95_ms=%.2f lag_max_ms=%.2f\n",
        logging, received.load(), FileBytes, hashOk, elapsed, elapsed ? 32000.0 / elapsed : 0.0,
        lags.isEmpty() ? 0 : lags.at(lags.size() * 95 / 100), lags.isEmpty() ? 0 : lags.last());
    return !failed && received == FileBytes && hashOk ? 0 : 1;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    QFile style(":/styles/workbench.qss");
    if (style.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(style.readAll()));
    const int history = argc > 1 ? std::atoi(argv[1]) : 1000000;
    const int rate = argc > 2 ? std::atoi(argv[2]) : 10000;
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 10;
    const int bytes = argc > 4 ? std::atoi(argv[4]) : 64;
    const bool logging = argc > 5 && std::atoi(argv[5]);
    QTemporaryDir dir;
    PreferencesStore prefs(nullptr, dir.filePath("perf.ini"));
    prefs.setValue("traffic/format", argc > 7 ? QString::fromLocal8Bit(argv[7]) : QStringLiteral("UTF-8"));
    prefs.setValue("traffic/recordPath", dir.filePath("load.log"));
    TrafficPanel panel;
    panel.setPreferences(&prefs);
    PortDraft port;
    panel.setPorts({port});
    panel.resize(1400, 900);
    panel.show();
    QTest::qWait(100);
    if (argc > 6 && QByteArray(argv[6]) == "transfer") return transferProbe(panel, dir.path(), logging);
    auto *model = panel.findChild<TrafficModel *>();
    auto *text = panel.findChild<TrafficTextView *>();
    auto *apply = panel.findChild<QPushButton *>("filterRebuild");
    auto *record = panel.findChild<QAction *>("recordTraffic");
    const auto message = [port, bytes](quint64 id) {
        Traffic::Message m;
        m.id = id;
        m.timestamp = QDateTime::currentDateTime();
        m.direction = Traffic::Direction::Receive;
        m.source.endpointId = port.id;
        m.source.endpointIndex = 0;
        m.source.endpointKind = PortDraft::TcpClient;
        m.raw = QByteArray(qMax(16, bytes), 'x');
        const auto tag = QByteArray::number(id);
        std::copy(tag.cbegin(), tag.cend(), m.raw.begin());
        m.raw.back() = '\n';
        m.rawLength = m.raw.size();
        return m;
    };
    std::printf("Qt=%s history=%d rate=%d seconds=%d bytes=%d log=%d\n",
        qVersion(), history, rate, seconds, bytes, logging);
    std::fflush(stdout);
    for (int start = 0; start < history; start += 5000) {
        QList<Traffic::Message> batch;
        for (int n = start; n < qMin(history, start + 5000); ++n) batch.append(message(n + 1));
        model->appendRecords(std::move(batch));
    }
    QVector<double> lags;
    std::atomic<int> posted{0};
    int maxBacklog = 0;
    QElapsedTimer tick;
    tick.start();
    QTimer heartbeat;
    heartbeat.setTimerType(Qt::PreciseTimer);
    heartbeat.setInterval(10);
    QObject::connect(&heartbeat, &QTimer::timeout, &panel, [&] {
        const double lag = qMax(0.0, tick.nsecsElapsed() / 1e6 - 10);
        lags.append(lag);
        maxBacklog = qMax(maxBacklog, posted.load() - qMax(0, model->rowCount() - history));
        if (lag > 100) std::printf("lag_ms=%.2f model_rows=%d text_lines=%d projecting=%d\n",
            lag, model->rowCount(), text->lineCount(), panel.isReprojecting());
        tick.restart();
    });
    heartbeat.start();
    QElapsedTimer projection;
    projection.start();
    apply->setEnabled(true); // 直接预填模型后，模拟正常接收已启用的「应用过滤」。
    apply->click();
    const double callMs = projection.nsecsElapsed() / 1e6;
    while (projection.elapsed() < 120000
           && (panel.isReprojecting() || text->lineCount() < qMax(1, history)))
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QTest::qWait(40);
    std::printf("projection_call_ms=%.2f projection_total_ms=%lld lines=%d memory_MiB=%.0f lag_max_ms=%.2f\n",
        callMs, projection.elapsed(), text->lineCount(), memoryMiB(),
        lags.isEmpty() ? 0 : *std::max_element(lags.cbegin(), lags.cend()));
    lags.clear(); tick.restart();
    std::fflush(stdout);
    record->setChecked(logging);
    std::atomic<bool> done{false};
    const int total = rate * seconds;
    QElapsedTimer stream;
    stream.start();
    if (argc > 6 && QByteArray(argv[6]) == "changes") {
        QTimer::singleShot(2000, &panel, [&] {
            auto *stamp = panel.findChild<QAction *>("prefixTimestamp");
            stamp->setChecked(false);
            apply->click();
            stamp->setChecked(true);
            apply->click();
        });
    }
    std::thread producer([&] {
        const auto start = std::chrono::steady_clock::now();
        for (int n = 0; n < total; ++n) {
            if (n % qMax(1, rate / 200) == 0)
                std::this_thread::sleep_until(start + std::chrono::microseconds(qint64(n) * 1000000 / rate));
            auto m = message(quint64(history) + n + 1);
            QMetaObject::invokeMethod(&panel, [&panel, m = std::move(m)] { panel.appendMessage(m); }, Qt::QueuedConnection);
            posted = n + 1;
        }
        done = true;
    });
    const int expected = history + total;
    while (stream.elapsed() < qMax(120000, seconds * 1000 + 30000)
           && (!done || model->rowCount() < expected || panel.isReprojecting())) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    producer.join();
    const qint64 elapsed = stream.elapsed();
    record->setChecked(false);
    QTest::qWait(50);
    heartbeat.stop();
    std::sort(lags.begin(), lags.end());
    const double p95 = lags.isEmpty() ? 0 : lags.at(qMin(lags.size() - 1, lags.size() * 95 / 100));
    const double max = lags.isEmpty() ? 0 : lags.last();
    qint64 logBytes = 0;
    for (const auto &info : QDir(dir.path()).entryInfoList({"load-*.log"}, QDir::Files)) logBytes += info.size();
    std::printf("stream_ms=%lld rows=%d expected=%d lines=%d lag_p95_ms=%.2f lag_max_ms=%.2f max_backlog=%d memory_MiB=%.0f log_bytes=%lld\n",
        elapsed, model->rowCount(), expected, text->lineCount(), p95, max, maxBacklog, memoryMiB(), logBytes);
    return model->rowCount() == expected && text->lineCount() >= qMax(1, expected) ? 0 : 1;
}
