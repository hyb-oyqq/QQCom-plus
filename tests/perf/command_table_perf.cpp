// 指令表格删除性能探针。
//
// 背景：removeRows() 采用"从后往前逐个 removeRow + removeAt"的写法，每次 removeRow
// 都要让 QTableWidget 搬动其后所有行的 item/cellWidget（O(n)），因此删除 k 行是 O(k·n)，
// 删掉一半行就是 O(n²)。用户直觉"项目多了删除会卡"值得量化验证，而不是凭感觉判断。
//
// 用法：QQCom_command_perf.exe [每次删多少][表格总行数]
// 默认 500 行表格里删 250 行（勾选一半），打印一次删除耗时。
#include <QApplication>
#include <QElapsedTimer>
#include <QTableWidget>
#include <QCheckBox>
#include <QPushButton>
#include <QDebug>
#include <QMessageBox>
#include <QTimer>

#include "ui/panels/commands/commandtablewidget.h"
#include "domain/commandset.h"

static CommandSet makeSet(int count)
{
    CommandSet set(QStringLiteral("perf"));
    set.commands.reserve(count);
    for (int i = 0; i < count; ++i) {
        CommandItem cmd;
        cmd.content = QStringLiteral("CMD-%1").arg(i);
        cmd.description = QStringLiteral("指令 %1").arg(i);
        cmd.delayMs = 10;
        set.commands.append(cmd);
    }
    return set;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    const int deleteCount = argc > 1 ? QString::fromLatin1(argv[1]).toInt() : 250;
    const int total = argc > 2 ? QString::fromLatin1(argv[2]).toInt() : 500;
    if (deleteCount <= 0 || total < deleteCount) return 2;

    CommandTableWidget table;
    table.setCommandSet(makeSet(total));
    table.show();

    // 释放刷新事件，让布局稳定，避免把首次布局成本算进删除。
    QApplication::processEvents();

    auto *grid = table.findChild<QTableWidget *>();
    if (!grid) { qWarning("no grid"); return 2; }

    // 只勾选前 deleteCount 行，底栏真实删除入口据此确定待删范围。
    for (int row = 0; row < total; ++row) {
        if (auto *cb = grid->cellWidget(row, 0) ? grid->cellWidget(row, 0)->findChild<QCheckBox *>() : nullptr)
            cb->setChecked(row < deleteCount);
    }

    auto *remove = table.findChild<QPushButton *>(QStringLiteral("removeChecked"));
    if (!remove) return 2;
    QElapsedTimer timer;
    // 走真实界面入口，在接受确认后计时，不为探针暴露私有行操作。
    QTimer::singleShot(0, &table, [&] {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            timer.start();
            dialog->done(QMessageBox::Yes);
        }
    });
    remove->click();
    if (!timer.isValid() || table.rowCount() != total - deleteCount) return 3;
    const qint64 ms = timer.elapsed();

    qInfo().noquote() << QStringLiteral("总行数=%1 删除=%2 剩余=%3 耗时=%4 ms")
                             .arg(total).arg(deleteCount).arg(table.rowCount()).arg(ms);

    // Windows GUI 子系统会吞 stdout，直接写文件最可靠。
    {
        FILE *f = fopen("QQCom_cmdperf_out.txt", "a");
        if (f) {
            fprintf(f, "总行数=%d 删除=%d 剩余=%d 耗时=%lld ms\n",
                    total, deleteCount, table.rowCount(), static_cast<long long>(ms));
            fflush(f);
            fclose(f);
        }
    }
    return 0;
}
