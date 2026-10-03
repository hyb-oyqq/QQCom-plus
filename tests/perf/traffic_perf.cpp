// 监视区刷新性能基准：在真实 MainWindow + TrafficPanel 上，按"每 16ms 一批新消息"
// 的节奏持续灌入数据，测每个刷新周期的墙钟耗时。
//
// 这件事只能测出货代码：卡顿来自面板与视图的协作方式（攒批怎么落、文本怎么追加、
// 跟随怎么滚动），任何用假模型搭出来的复现都测不到同一个瓶颈。
//
// 用法：easyDebug_traffic_perf.exe [载荷字节数] [每批条数] [批数] [拖动窗口基准:0/1]
// 判读：mean/p50 是流畅度，max 与 over100 是"卡顿"——用户感知到的正是长尾。
#include "mainwindow.h"
#include "ui/panels/traffic/trafficpanel.h"
#include "ui/panels/traffic/traffictextview.h"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QPushButton>
#include <QScrollBar>
#include <QtTest>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <windows.h>
#include <psapi.h>

namespace {

struct Stats
{
    double mean = 0, p50 = 0, p95 = 0, max = 0;
    int over100 = 0, over40 = 0;
};

Stats summarize(QVector<double> v)
{
    Stats s;
    if (v.isEmpty()) return s;
    const int n = int(v.size());
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) {
        sum += x;
        if (x > 100.0) ++s.over100;
        if (x > 40.0) ++s.over40;
    }
    s.mean = sum / n;
    s.p50 = v.at(n / 2);
    s.p95 = v.at(qMin(n - 1, int(n * 0.95)));
    s.max = v.last();
    return s;
}

QByteArray makePayload(int bytes, quint32 seed)
{
    QByteArray b(bytes, Qt::Uninitialized);
    quint32 s = seed * 2654435761u + 1u;
    for (int i = 0; i < bytes; ++i) {
        s = s * 1664525u + 1013904223u;
        b[i] = char((s >> 16) & 0xFF);
    }
    return b;
}

double wsMiB()
{
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.WorkingSetSize / 1024.0 / 1024.0;
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    QFile style(QStringLiteral(":/styles/workbench.qss"));
    if (style.open(QIODevice::ReadOnly)) qApp->setStyleSheet(QString::fromUtf8(style.readAll()));

    const int payloadBytes = argc > 1 ? std::atoi(argv[1]) : 1024;
    const int batchSize = argc > 2 ? std::atoi(argv[2]) : 20;
    const int cycles = argc > 3 ? std::atoi(argv[3]) : 200;
    const bool resizeBench = argc > 4 ? std::atoi(argv[4]) != 0 : false;

    MainWindow window;
    window.resize(1400, 900);
    window.show();
    QTest::qWait(120); // 等样式完成布局

    auto *panel = window.findChild<TrafficPanel *>();
    auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
    auto *model = window.findChild<TrafficModel *>();
    auto *portA = window.findChild<QPushButton *>(QStringLiteral("showPortA"));
    Q_ASSERT(panel && text && model && portA);

    window.loadExample();
    QTest::qWait(60);

    const QUuid endpointId = portA->property("endpoint").toUuid();
    std::printf("========================================================\n");
    std::printf("监视区刷新基准  Qt %s  视图 文本框\n", qVersion());
    std::printf("载荷 %d B/条  每批 %d 条  批数 %d\n", payloadBytes, batchSize, cycles);
    std::printf("窗口 %dx%d  工作集 %.0f MiB\n", window.width(), window.height(), wsMiB());
    std::printf("========================================================\n");

    // 预先造好全部 payload，避免把随机数生成计入计时。
    // 预热也要从同一个池里取（40 批），池子必须按"预热 + 被测"的总量开，
    // 否则循环末尾会越界读取。
    constexpr int WarmupBatches = 40;
    const int total = (WarmupBatches + cycles) * batchSize;
    QVector<QByteArray> pool;
    pool.reserve(total);
    for (int i = 0; i < total; ++i) pool.append(makePayload(payloadBytes, quint32(i) + 11));

    quint64 nextId = 1;
    const auto makeBatch = [&](int from) {
        QList<Traffic::Message> batch;
        batch.reserve(batchSize);
        for (int k = 0; k < batchSize; ++k) {
            Traffic::Message m;
            m.id = nextId++;
            m.timestamp = QDateTime::currentDateTime();
            m.direction = Traffic::Direction::Receive;
            m.origin = Traffic::Origin::Automatic;
            m.framing = Traffic::Framing::Stream;
            m.source.endpointId = endpointId;
            m.source.endpointIndex = 0;
            m.source.endpointKind = PortDraft::Serial;
            m.source.endpointName = QStringLiteral("串口 A");
            m.raw = pool.at(from + k);
            m.rawLength = m.raw.size();
            batch.append(m);
        }
        return batch;
    };

    // 预热：先把文本历史灌起来，让稳态（含可能的头部淘汰）成为被测对象。
    for (int i = 0; i < WarmupBatches; ++i) {
        panel->appendMessages(makeBatch(i * batchSize));
        QTest::qWait(20);
    }
    QElapsedTimer t;
    std::printf("预热完成：模型 %d 行  文本 %d 行\n\n", model->rowCount(), text->lineCount());

    QVector<double> perCycle;
    QVector<double> perPace; // 每拍刻意等待攒批的时间，从净工作耗时里扣除
    perCycle.reserve(cycles);
    perPace.reserve(cycles);
    int offset = WarmupBatches * batchSize;
    for (int c = 0; c < cycles; ++c) {
        const quint64 lastId = nextId + batchSize - 1;
        t.restart();
        panel->appendMessages(makeBatch(offset));
        offset += batchSize;
        // 攒批是 16ms 一拍的异步过程。等到"最后一条真的进了模型"为止，
        // 而不是等行数变大——记录数到达历史预算上限后，每批都是"丢最旧 + 加最新"，
        // 行数不再增长，只有消息 id 能证明这一拍刷新已经落地。
        // 至少等满 25ms：给防抖定时器留出触发的机会，
        // 否则测不到"数据停顿时才发作"的整秒冻结。
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
            const bool landed = model->rowCount() > 0
                && model->record(model->rowCount() - 1).id == lastId;
            if (landed && wait.elapsed() >= 25) break;
        }
        perPace.append(wait.nsecsElapsed() / 1e6);
        text->scrollToBottom();
        text->viewport()->repaint();
        perCycle.append(t.nsecsElapsed() / 1e6);
    }

    const Stats s = summarize(perCycle);
    const Stats pace = summarize(perPace);
    std::printf("刷新周期（追加 → 入模 → 滚底 → 重绘）\n");
    std::printf("  合计   mean %8.2f ms   p50 %8.2f   p95 %8.2f   max %9.2f ms\n",
                s.mean, s.p50, s.p95, s.max);
    std::printf("  净工作 mean %8.2f ms   （已扣除每拍刻意等待攒批的 %.1f ms）\n",
                s.mean - pace.mean, pace.mean);
    std::printf("  超过 100ms（肉眼可见卡顿）的周期 %d / %d\n", s.over100, cycles);
    std::printf("  帧率 净工作折合 %5.1f fps   最差一拍 %5.1f fps\n",
                s.mean > 0 ? 1000 / (s.mean - pace.mean) : 0, s.max > 0 ? 1000 / s.max : 0);
    std::printf("  模型 %d 行  文本 %d 行  工作集 %.0f MiB\n", model->rowCount(), text->lineCount(), wsMiB());

    // 拖动窗口基准：跟随开启 + 已有大量历史的情况下连续改窗口尺寸。
    // resize 事件与整窗重绘都是同步的，直接计时；攒批落在自己的定时器里不混入。
    if (resizeBench) {
        QVector<double> perResize;
        const int baseWidth = window.width();
        const int widthDelta = qMax(1, baseWidth / 4);
        if (offset + batchSize > pool.size()) offset = 0;
        panel->appendMessages(makeBatch(offset));
        offset += batchSize;
        QTest::qWait(40); // 让追加先落地，测量窗口缩放本身的成本
        for (int i = 0; i < 40; ++i) {
            t.restart();
            window.resize(i % 2 ? baseWidth - widthDelta : baseWidth, window.height());
            window.repaint(); // 同步重绘整窗，含文本视图的缩放处理与重绘
            perResize.append(t.nsecsElapsed() / 1e6);
        }
        const Stats r = summarize(perResize);
        std::printf("\n拖动窗口（跟随 + 历史就位，40 次改尺寸 ±%dpx，同步重绘）\n", widthDelta);
        std::printf("  mean %8.2f ms   p50 %8.2f   p95 %8.2f   max %9.2f ms   超100ms %d/40\n",
                    r.mean, r.p50, r.p95, r.max, r.over100);
    }

    return 0;
}
