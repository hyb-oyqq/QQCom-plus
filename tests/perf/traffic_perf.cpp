// 监视区刷新性能基准：在真实 MainWindow + TrafficPanel 上，按"每 16ms 一批新消息"
// 的节奏持续灌入数据，测每个刷新周期的墙钟耗时。
//
// 这件事只能测出货代码：卡顿来自面板与视图的协作方式（攒批怎么落、文本怎么追加、
// 跟随怎么滚动），任何用假模型搭出来的复现都测不到同一个瓶颈。
//
// 用法：QQCom_traffic_perf.exe [载荷字节数] [每批条数] [批数] [拖动窗口基准:0/1] [投影基准行数]
// 判读：mean/p50 是流畅度，max 与 over100 是"卡顿"——用户感知到的正是长尾。
// 第 5 个参数 >0 时追加"视图投影基准"：量把 K 行重新渲染进文本框要多久，
// 显示设置/过滤条件"立即作用于已有内容"的代价就是它。
#include "mainwindow.h"
#include "ui/panels/traffic/trafficpanel.h"
#include "ui/panels/traffic/traffictextview.h"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
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
    // 第 5 个参数：视图投影基准的行数（0 = 跳过）。见文件末尾的"投影成本"段。
    const int projectionRows = argc > 5 ? std::atoi(argv[5]) : 0;

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

    // ── 视图投影成本 ──────────────────────────────────────────────────────────
    // 显示设置与过滤条件要能立刻作用于"已经收到的内容"，代价就是一次重投影：
    // 把 K 行从存储里重新渲染进文本框。这一段专门量这个代价——它决定了
    // "屏幕可重画的历史"能开多大，以及重投影要不要切片。
    //
    // 按行数梯度量（不是单点）：只有梯度能回答"成本与文档规模是线性还是超线性"，
    // 而这一点直接决定预算能开多大。打印折合每条 µs，趋势比绝对值更重要。
    // 载荷在这里遮成可打印 ASCII：随机字节里的 \r\n 会一条膨胀成好几行，
    // 把"每条成本"和"每行成本"混在一起，梯度就看不干净了。
    if (projectionRows > 0) {
        auto *view = new TrafficTextView;
        view->resize(1400, 900);
        view->show();
        QTest::qWait(120);

        const auto format = PayloadCodec::Format::Utf8;
        constexpr int Slice = 2000; // 与实现里的切片粒度一致
        const QString sample = QStringLiteral("12:34:56.789 RX: 温度 25.0 C  设备就绪");
        const char *const VariantNames[4] = {"setCharFormat + insertText（现状）",
            "整段包一层 editBlock", "insertText(text, format) 重载", "editBlock + 重载"};
        TrafficTextView::DisplayOptions opt;
        opt.prefix = true;
        opt.prefixLineNumber = true;
        opt.prefixTimestamp = true;
        opt.prefixLength = true;
        opt.alwaysShowHex = false;

        std::printf("\n========================================================\n");
        std::printf("视图投影基准  载荷 %d B/条（遮成可打印 ASCII，一条一行）\n", payloadBytes);
        std::printf("========================================================\n");
        std::printf("%10s %12s %12s %12s %10s\n",
            "行数", "整批 ms", "折合 µs/条", "文档行数", "MiB 字符");

        for (int k = qMax(1, projectionRows / 8); k <= projectionRows; k *= 2) {
            QList<Traffic::Message> rows;
            rows.reserve(k);
            for (int i = 0; i < k; ++i) {
                Traffic::Message m;
                m.id = quint64(i) + 1;
                m.timestamp = QDateTime::currentDateTime();
                m.direction = i % 3 == 0 ? Traffic::Direction::Transmit : Traffic::Direction::Receive;
                m.origin = Traffic::Origin::Automatic;
                m.framing = Traffic::Framing::Stream;
                m.source.endpointId = endpointId;
                m.source.endpointIndex = 0;
                m.source.endpointKind = PortDraft::Serial;
                m.source.endpointName = QStringLiteral("串口 A");
                QByteArray raw = pool.at(i % pool.size());
                for (auto &byte : raw) // 遮掉控制字符：\(\r\n\t\) 会让一条膨胀成多行
                    byte = char(0x20 + (quint8(byte) % 0x5F));
                m.raw = raw;
                m.rawLength = raw.size();
                rows.append(m);
            }

            view->clearAll();
            t.restart();
            view->appendMessages(rows, format, opt);
            const double ms = t.nsecsElapsed() / 1e6;
            std::printf("%10d %12.1f %12.1f %12d %10.2f\n", k, ms, ms * 1000.0 / k,
                view->lineCount(), view->document()->characterCount() / 1024.0 / 1024.0);
        }
        std::fflush(stdout);

        // 分片重投影：分片实现实际走的路径（切片粒度与实现一致）。
        std::printf("\n分片重投影（每片 %d 条，按 1/8 → 全量 梯度）\n", Slice);
        for (int k = qMax(1, projectionRows / 8); k <= projectionRows; k *= 2) {
            QList<Traffic::Message> rows;
            rows.reserve(k);
            for (int i = 0; i < k; ++i) {
                Traffic::Message m;
                m.id = quint64(i) + 1;
                m.timestamp = QDateTime::currentDateTime();
                m.direction = Traffic::Direction::Receive;
                m.origin = Traffic::Origin::Automatic;
                m.framing = Traffic::Framing::Stream;
                m.source.endpointId = endpointId;
                m.source.endpointKind = PortDraft::Serial;
                m.source.endpointName = QStringLiteral("串口 A");
                QByteArray raw = pool.at(i % pool.size());
                for (auto &byte : raw) byte = char(0x20 + (quint8(byte) % 0x5F));
                m.raw = raw;
                m.rawLength = raw.size();
                rows.append(m);
            }
            view->clearAll();
            t.restart();
            for (int from = 0; from < rows.size(); from += Slice)
                view->appendMessages(rows.mid(from, Slice), format, opt);
            const double ms = t.nsecsElapsed() / 1e6;
            std::printf("%10d %12.1f %12.1f\n", k, ms, ms * 1000.0 / k);
        }
        std::fflush(stdout);

        // ── 成本分解 ─────────────────────────────────────────────────────────
        // 360 µs/条 是不可接受的量级，必须知道它花在哪：解码、文档插入、
        // 还是"控件在屏幕上"带来的排版/滚动条开销。三个数一比就清楚。
        std::printf("\n成本分解（%d 条，载荷 %d B）\n", projectionRows, payloadBytes);
        {
            QList<Traffic::Message> rows;
            rows.reserve(projectionRows);
            for (int i = 0; i < projectionRows; ++i) {
                Traffic::Message m;
                m.id = quint64(i) + 1;
                m.timestamp = QDateTime::currentDateTime();
                m.direction = Traffic::Direction::Receive;
                m.origin = Traffic::Origin::Automatic;
                m.framing = Traffic::Framing::Stream;
                m.source.endpointId = endpointId;
                m.source.endpointKind = PortDraft::Serial;
                m.source.endpointName = QStringLiteral("串口 A");
                QByteArray raw = pool.at(i % pool.size());
                for (auto &byte : raw) byte = char(0x20 + (quint8(byte) % 0x5F));
                m.raw = raw;
                m.rawLength = raw.size();
                rows.append(m);
            }

            // ① 只解码 + 切行，完全不碰文档
            qsizetype chars = 0;
            t.restart();
            for (const auto &m : rows) chars += PayloadCodec::renderMultiline(m.raw, format).size();
            const double decodeMs = t.nsecsElapsed() / 1e6;
            std::printf("  ① 只解码切行            %8.1f ms  (%.1f µs/条, %lld 字符)\n",
                decodeMs, decodeMs * 1000.0 / projectionRows, static_cast<long long>(chars));

            // ② 写进"没上屏"的文本框：文档 + 排版都在，只是没有窗口/滚动条/重绘
            auto *off = new TrafficTextView;
            t.restart();
            off->appendMessages(rows, format, opt);
            const double hiddenMs = t.nsecsElapsed() / 1e6;
            std::printf("  ② 写进未上屏文本框      %8.1f ms  (%.1f µs/条)\n",
                hiddenMs, hiddenMs * 1000.0 / projectionRows);
            delete off;

            // ③ 写进屏幕上的文本框（= 前一段梯度测量的口径）
            view->clearAll();
            t.restart();
            view->appendMessages(rows, format, opt);
            const double shownMs = t.nsecsElapsed() / 1e6;
            std::printf("  ③ 写进已上屏文本框      %8.1f ms  (%.1f µs/条)\n",
                shownMs, shownMs * 1000.0 / projectionRows);
            std::printf("  ②-① = 文档写入 %.1f µs/条   ③-② = 上屏 %.1f µs/条\n",
                (hiddenMs - decodeMs) * 1000.0 / projectionRows,
                (shownMs - hiddenMs) * 1000.0 / projectionRows);
        }
        std::fflush(stdout);

        // ── 微实验：文档写入的固定开销 ────────────────────────────────────────
        // ②-① 是 220~250 µs/条，而每条消息只做 5~7 次 insertText → 摊到每次插入
        // 约 40 µs。这个**固定开销**（与插入内容多少无关）才是真正的敌人：
        // 降它只有两条路——减少插入次数，或让一次插入不再触发完整的文档编辑。
        // 四种用法各测一遍，先确认能压到多少，再决定实现怎么改。
        //
        // 探针必须**逐行插入块分隔符**：不插换行的话 N 次插入全落在同一个块里，
        // 变成一条几十万字符的长行，量到的是"超长单块"的布局成本，与实际渲染路径
        // （一条消息一个块）完全不是一回事——这个陷阱本身也值得记一笔。
        constexpr int Trials = 5000;
        const int SampleRows = 3; // 每轮插几行，摊薄固定开销后再折合到每次插入
        std::printf("\n微实验：文档写入的固定开销（%d 行 × %d 次插入/行，行内 %lld 字符）\n",
            Trials, SampleRows, static_cast<long long>(sample.length()));
        for (int variant = 0; variant < 4; ++variant) {
            auto *probe = new TrafficTextView;
            QTextCursor cursor(probe->document());
            cursor.movePosition(QTextCursor::End);
            QTextCharFormat body;
            body.setForeground(QColor(0x1C, 0x24, 0x30));
            const bool editBlock = variant == 1 || variant == 3;
            const bool overload = variant == 2 || variant == 3;
            const int inserts = Trials * SampleRows;
            if (editBlock) cursor.beginEditBlock();
            t.restart();
            for (int i = 0; i < Trials; ++i) {
                for (int k = 0; k < SampleRows; ++k) {
                    if (!probe->document()->isEmpty())
                        cursor.insertText(QString(QChar::ParagraphSeparator));
                    if (overload) cursor.insertText(sample, body);
                    else {
                        cursor.setCharFormat(body);
                        cursor.insertText(sample);
                    }
                }
            }
            if (editBlock) cursor.endEditBlock();
            const double ms = t.nsecsElapsed() / 1e6;
            std::printf("  %-30s %8.1f ms  %7.2f µs/次插入   %8.2f µs/行\n",
                VariantNames[variant], ms, ms * 1000.0 / inserts, ms * 1000.0 / Trials);
            delete probe;
        }
        std::fflush(stdout);
        view->hide();
        delete view;
        QCoreApplication::processEvents();
    }

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
