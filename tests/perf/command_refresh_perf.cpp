// 指令表格「刷新 + 结尾符列」性能探针。
//
// 背景（用户报告）：指令列表里点「结尾符」下拉时卡顿。定位到的两处热点都在 refreshTable()
// 的收尾列宽计算里，它们是**随行数线性放大**的：
//   ① updateTailColumnWidth() 逐行取 combo->sizeHint()——每行答案完全一样，等于把同一个
//      结果重算 N 遍；
//   ② updateRow() 每行都调 tailLabels()（内含 tr()）重建一次标签表。
// 改法：标签表与列宽在 refreshTable 开头算一次（见 commandtablewidget.cpp）。
//
// 本探针量化完整建行和布局成本；切换分组身份强制重建，避免把同组无变化的快捷返回算作刷新。
// 用法：QQCom_command_refresh_perf.exe [行数]（默认 2000，重复 5 轮取中位数）
#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QPushButton>
#include <QTableWidget>
#include <QDebug>
#include <algorithm>

#include "ui/panels/commands/commandtablewidget.h"
#include "domain/commandset.h"

static CommandSet makeSet(int count)
{
    CommandSet set(QStringLiteral("perf"));
    set.commands.reserve(count);
    for (int i = 0; i < count; ++i) {
        CommandItem cmd;
        cmd.content = QStringLiteral("AA 06 05 %1").arg(i % 256, 2, 16, QLatin1Char('0'));
        cmd.description = QStringLiteral("指令 %1").arg(i);
        cmd.delayMs = 50;
        set.commands.append(cmd);
    }
    return set;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    const int total = argc > 1 ? QString::fromLatin1(argv[1]).toInt() : 2000;

    CommandTableWidget table;
    table.show();
    QApplication::processEvents();

    auto *grid = table.findChild<QTableWidget *>();
    if (!grid) { qWarning("no grid"); return 2; }

    const auto set = makeSet(total);
    const auto reload = [&] {
        auto next = set;
        next.id = QUuid::createUuid();
        table.setCommandSet(next);
    };

    // 连续刷新前后统计"本控件下的子控件总数"：如果它在增长，说明旧的 cellWidget
    // 没有被真正回收（deleteLater 只在返回事件循环时才执行，连续刷新期间会越积越多）。
    // 这是"越用越卡"的量化证据，也是判断清理是否生效的唯一硬指标。
    QString leakInfo;
    {
        const int before = table.findChildren<QWidget *>().size();
        reload();
        const int afterBuild = table.findChildren<QWidget *>().size();
        QApplication::processEvents();
        const int afterPump = table.findChildren<QWidget *>().size();
        leakInfo = QStringLiteral("子控件 建前=%1 建后=%2 泵后=%3").arg(before).arg(afterBuild).arg(afterPump);
    }

    // 分阶段计时：把"建行/控件"与"processEvents 里的布局重排"分开，
    // 否则只知道总时长，不知道超线性成本出在哪一段。
    QElapsedTimer phase;
    phase.start();
    reload();
    const qint64 buildMs = phase.elapsed();
    phase.restart();
    QApplication::processEvents();
    const qint64 layoutMs = phase.elapsed();

    // 反复整表刷新（= 打开指令集 / 增删行 / 切换分组时走的路径），取中位数。
    // 5 轮在大行数下太久（布局是超线性的），行数越大轮数越少，保证探针能在几分钟内出结果。
    // 逐轮也单独计时：首轮 vs 后续轮的差异能区分"一次性成本"与"累积成本"。
    const int rounds = total >= 1000 ? 3 : 5;
    QList<qint64> samples;
    QStringList perRound;
    for (int round = 0; round < rounds; ++round) {
        // 先把上一轮排队的延迟删除（deleteLater）全部执行掉，否则本轮要额外背上
        // 上一轮遗留控件的析构成本——这正是"刷新一轮比一轮慢"的来源（探针目的）。
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QElapsedTimer timer;
        timer.start();
        reload();
        const qint64 buildRound = timer.elapsed();
        timer.restart();
        QApplication::processEvents(); // 把布局/重绘一并算进去，这才是用户等的那段时间
        const qint64 layoutRound = timer.elapsed();
        samples.append(buildRound + layoutRound);
        perRound.append(QStringLiteral("%1+%2").arg(buildRound).arg(layoutRound));
    }
    std::sort(samples.begin(), samples.end());
    const qint64 medianMs = samples.at(samples.size() / 2);
    const QString roundsText = perRound.join(QStringLiteral(" "));

    // 结尾符列宽是否已不再是"每行重算"：量一次 updateRow 的 sizeHint 调用规模。
    // 这里用几何证据——所有行的结尾符 combo 宽度应完全一致（同一探针量出的列宽）。
    int minComboWidth = 1 << 30;
    int maxComboWidth = 0;
    for (int row = 0; row < grid->rowCount(); ++row) {
        if (auto *host = grid->cellWidget(row, 5)) {
            if (auto *box = host->findChild<QComboBox *>()) {
                minComboWidth = qMin(minComboWidth, box->width());
                maxComboWidth = qMax(maxComboWidth, box->width());
            }
        }
    }

    const QString line = QStringLiteral("行数=%1 首刷 建行=%2ms 布局=%3ms | 刷新中位=%4 ms | 各轮(建+布)=%5 | combo宽=%6..%7 | %8")
                             .arg(total).arg(buildMs).arg(layoutMs).arg(medianMs)
                             .arg(roundsText).arg(minComboWidth).arg(maxComboWidth).arg(leakInfo);
    qInfo().noquote() << line;

    // Windows GUI 子系统吞 stdout，写文件最可靠。
    {
        FILE *f = fopen("QQCom_cmdrefresh_out.txt", "a");
        if (f) {
            fprintf(f, "%s\n", line.toUtf8().constData());
            fflush(f);
            fclose(f);
        }
    }
    return 0;
}
