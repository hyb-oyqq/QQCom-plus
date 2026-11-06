#include "app/commandmanager.h"
#include "persistence/preferencesstore.h"
#include "ui/common/apptheme.h"
#include "ui/panels/commands/commandpanel.h"
#include "ui/panels/commands/commandtablewidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QTableWidget>
#include <QTemporaryDir>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    AppTheme::setMode(AppTheme::Mode::Light);
    const int rows = argc > 1 ? QString::fromLocal8Bit(argv[1]).toInt() : 500;
    if (rows < 1) return 2;
    QTemporaryDir directory;
    PreferencesStore store(nullptr, directory.filePath("preferences.ini"));
    auto *manager = CommandManager::instance();
    manager->load(&store);
    auto source = manager->allSets().first();
    for (int i = 0; i < rows; ++i) {
        CommandItem command;
        command.content = QStringLiteral("AT+COMMAND=%1").arg(i);
        source.commands.append(command);
    }
    manager->updateSet(source.id, source);
    for (int i = 0; i < 3; ++i) {
        CommandSet other(QStringLiteral("Other%1").arg(i));
        other.commands = source.commands;
        manager->addSet(other);
    }
    manager->save(&store);
    CommandPanel panel;
    panel.setPreferencesStore(&store);
    panel.setExpanded(true);
    panel.setAttribute(Qt::WA_DontShowOnScreen);
    panel.resize(900, 500);
    panel.show();
    app.processEvents();
    auto *table = panel.findChild<CommandTableWidget *>();
    auto *grid = table->findChild<QTableWidget *>();
    auto *check = grid->cellWidget(0, 0)->findChild<QCheckBox *>();
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 60; ++i) check->setChecked(!check->isChecked());
    const auto editUs = timer.nsecsElapsed() / 1000;
    timer.restart();
    qsizetype sum = 0;
    for (int i = 0; i < 100; ++i) sum += table->commandSet().commands.size();
    const auto snapshotUs = timer.nsecsElapsed() / 1000;
    auto other = manager->allSets().last();
    timer.restart();
    for (int i = 0; i < 5; ++i) {
        other.name = QStringLiteral("Renamed%1").arg(i);
        manager->updateSet(other.id, other);
        app.processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    const auto metadataUs = timer.nsecsElapsed() / 1000;
    if (sum != rows * 100 || table->commandSet().commands.size() != rows) return 3;
    const auto result = QStringLiteral("rows=%1 groups=4 edits60_us=%2 snapshots100_us=%3 unrelated5_us=%4")
        .arg(rows).arg(editUs).arg(snapshotUs).arg(metadataUs);
    qInfo().noquote() << result;
    QFile output(QStringLiteral("command_panel_perf.txt"));
    if (!output.open(QIODevice::WriteOnly | QIODevice::Append)) return 4;
    output.write(result.toUtf8() + '\n');
}
