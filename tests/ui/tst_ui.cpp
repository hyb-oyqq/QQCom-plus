#include "mainwindow.h"
#include "automation/capabilitycatalog.h"
#include "automation/clirunner.h"
#include "automation/localcontrol.h"
#include "app/commandmanager.h"
#include "domain/commandset.h"
#include "domain/receiveframing.h"
#include "ui/common/apptheme.h"
#include "ui/common/endpointalertstate.h"
#include "ui/common/endpointformat.h"
#include "ui/common/payloadcodec.h"
#include "ui/panels/commands/commandpanel.h"
#include "ui/panels/commands/commandtablewidget.h"
#include "ui/panels/sending/sendpanel.h"
#include "ui/panels/traffic/filterbar.h"
#include "ui/panels/traffic/trafficpanel.h"
#include "ui/panels/traffic/traffictextview.h"
#include "discovery/portdiscoveryservice.h"
#include "persistence/preferencesstore.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStatusBar>
#include <QHash>
#include <QImage>
#include <QInputDialog>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPoint>
#include <QProgressBar>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSet>
#include <QStyle>
#include <QStyleHints>
#include <QStyleOption>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyleOptionComboBox>
#include <QStyleOptionSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTcpServer>
#include <QUdpSocket>
#include <QTcpSocket>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QToolButton>
#include <QTimer>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

namespace {

// Every control in the port settings area must be able to paint its own content. The advanced
// combos once shipped with the value clipped away, because their width had been computed by
// hand instead of taken from the widget. This walks the area instead of naming fields, and
// checks the rect the value is painted into rather than the widget's outer box.
QStringList controlsThatClipContent(QWidget *area)
{
    QStringList problems;
    for (auto *combo : area->findChildren<QComboBox *>()) {
        // Editable fields scroll their text, and the serial port row deliberately elides long
        // device names: the popup and the tooltip carry the full label.
        if (!combo->isVisible() || combo->isEditable()
            || combo->objectName() == QLatin1String("serial.port"))
            continue;
        QStyleOptionComboBox option;
        option.initFrom(combo);
        option.currentText = combo->currentText();
        const QRect field = combo->style()->subControlRect(
            QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, combo);
        int widest = 0;
        QString widestItem;
        for (int i = 0; i < combo->count(); ++i) {
            const int width = combo->fontMetrics().horizontalAdvance(combo->itemText(i));
            if (width > widest) { widest = width; widestItem = combo->itemText(i); }
        }
        if (field.width() < widest)
            problems << QString("%1: field %2px cannot show \"%3\" (%4px)")
                            .arg(combo->objectName()).arg(field.width()).arg(widestItem).arg(widest);
    }
    for (auto *spin : area->findChildren<QAbstractSpinBox *>()) {
        if (!spin->isVisible()) continue;
        QStyleOptionSpinBox option;
        option.initFrom(spin);
        // 没有上下按钮的数字框，编辑区会占满内容区。不抄按钮样式的话会把空位算成按钮。
        option.buttonSymbols = spin->buttonSymbols();
        option.rect = spin->rect();
        const QRect field = spin->style()->subControlRect(
            QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxEditField, spin);
        // Every value the range allows has to be readable, not just the one showing right now.
        auto *number = qobject_cast<QSpinBox *>(spin);
        QStringList samples;
        // 波特率与端口框只保证当前文字放得下，数字变长时框再变宽。
        const bool hugsCurrentText = spin->objectName() == QLatin1String("client.port")
            || spin->objectName() == QLatin1String("server.port")
            || spin->objectName() == QLatin1String("udp.port")
            || spin->objectName() == QLatin1String("udp.targetPort");
        if (number && hugsCurrentText) {
            samples << number->text();
        } else if (number) {
            for (const int value : {number->minimum(), number->maximum()})
                samples << number->prefix() + QString::number(value) + number->suffix();
        } else {
            samples << spin->specialValueText();
        }
        for (const auto &sample : samples) {
            const int needed = spin->fontMetrics().horizontalAdvance(sample);
            if (field.width() < needed)
                problems << QString("%1: field %2px cannot show \"%3\" (%4px), widget %5 min %6 max %7 text \"%8\"")
                                .arg(spin->objectName()).arg(field.width()).arg(sample).arg(needed)
                                .arg(spin->width()).arg(spin->minimumWidth()).arg(spin->maximumWidth())
                                .arg(spin->text());
        }
    }
    return problems;
}

// 窗口不再有内容下限：边框可以拖到比顶栏更窄，窄了允许裁切。
// 顶栏"不重叠"只在放得下它的宽度上检查。

// A horizontal strip must stay one row at any window width: no wrapping, no scrollbar.
// Qt never wraps a QHBoxLayout, so the failure mode is overlap or a control pushed past the
// right edge. Anything in the same vertical band that overlaps horizontally is a failure;
// controls in different bands (the second row under the toolbar) are compared separately.
QString overlapInRow(QWidget *container)
{
    QList<QWidget *> items;
    for (auto *child : container->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        if (child->isVisibleTo(container) && child->width() > 1) items.append(child);
    std::sort(items.begin(), items.end(),
        [](QWidget *left, QWidget *right) { return left->x() < right->x(); });
    for (int i = 1; i < items.size(); ++i) {
        const auto *previous = items.at(i - 1);
        const auto *current = items.at(i);
        const bool sameBand = current->y() < previous->y() + previous->height()
            && previous->y() < current->y() + current->height();
        if (sameBand && current->x() < previous->x() + previous->width())
            return QStringLiteral("%1 压住 %2").arg(previous->objectName(), current->objectName());
    }
    for (auto *item : items)
        if (item->x() + item->width() > container->width())
            return QStringLiteral("%1 越过右缘（%2 > %3）")
                       .arg(item->objectName())
                       .arg(item->x() + item->width())
                       .arg(container->width());
    return QString();
}

// 取一个块最后一个分片的前景色。数据行的末片是正文色（HEX 对照行整行一个分片），
// 用来比对"哪一行更浅"。QColor::value() 是 HSV 明度，正好当"深浅"的判据。
QColor lastFragmentColor(const QTextBlock &block)
{
    QColor color;
    for (auto it = block.begin(); !it.atEnd(); ++it)
        color = it.fragment().charFormat().foreground().color();
    return color;
}

// 外观（编码格式 / 显示设置）改动会经防抖（trafficpanel.cpp 的 ReprojectDebounceMs = 50ms）
// 排队一次**设计上必然发生**的重投影：断言前必须显式等掉它，不能把用例建立在
// "它恰好还没触发"这种时序运气上（那正是这两条用例此前 flaky 的根因）。
// 90 = 50ms 防抖窗口 + 40ms 余量。
constexpr int AppearanceReprojectWaitMs = 90;

QString productCli()
{
    const auto override = qEnvironmentVariable("QQCOM_CLI_TEST_EXECUTABLE");
    if (!override.isEmpty()) return override;
    return QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../release/release/QQCom.exe");
}

struct CliReply {
    int code = -1;
    QJsonObject body;
    QByteArray diagnostic;
};

CliReply cliProcess(const QStringList &arguments, const QString &directory,
                    const QString &executable = productCli(), bool startupFixture = false)
{
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("QQCOM_CONTROL_DIR", directory);
    if (startupFixture) environment.insert("QQCOM_CLI_STARTUP_FIXTURE", "1");
    else environment.insert("QT_QPA_PLATFORM", "not-installed-cli-must-not-need-gui");
    process.setProcessEnvironment(environment);
    process.start(executable, QStringList{"--cli"} + arguments);
    if (!process.waitForStarted(3000)) return {-1, {}, process.errorString().toUtf8()};
    // Keep the test window's event loop running while the CLI uses its pipe.
    QSignalSpy finished(&process, &QProcess::finished);
    if (process.state() != QProcess::NotRunning && !finished.wait(15000)) {
        process.kill();
        process.waitForFinished(1000);
        return {-1, {}, "CLI process timed out"};
    }
    const auto bytes = process.readAllStandardOutput();
    return {process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1,
        QJsonDocument::fromJson(bytes).object(), bytes + process.readAllStandardError()};
}

} // namespace

class UiSmokeTest : public QObject
{
    Q_OBJECT
private slots:
    void cliCommandGroupsEditGuiAndSurviveReload()
    {
        if (!QFileInfo::exists(productCli())) QSKIP("Build QQCom first or set QQCOM_CLI_TEST_EXECUTABLE");
        QTemporaryDir directory;
        QString groupId, firstId, secondId;
        {
            MainWindow window;
            auto *control = window.findChild<LocalControlServer *>();
            QVERIFY(control);
            control->stop();
            QVERIFY(control->start(directory.path()));
            const auto call = [&](const QStringList &arguments) { return cliProcess(arguments, directory.path()); };
            const auto created = call({"commands", "create-group", "--name", QStringLiteral("设备调试"), "--notes", "CLI notes"});
            QVERIFY2(created.code == 0, created.diagnostic.constData());
            QVERIFY(created.body.value("persisted").toBool());
            groupId = created.body.value("groupId").toString();
            auto *selector = window.findChild<QComboBox *>("commandSetSelector");
            QVERIFY(selector);
            const int index = selector->findText(QStringLiteral("设备调试"));
            QVERIFY(index >= 0);
            selector->setCurrentIndex(index);
            auto *table = window.findChild<CommandTableWidget *>();
            QVERIFY(table);
            auto *panel = window.findChild<CommandPanel *>();
            QSignalSpy sends(panel, &CommandPanel::sendCommand);
            const auto first = call({"commands", "add", groupId, "--content", "AA 55", "--format", "hex",
                "--line-ending", "crlf", "--delay", "250", "--description", QStringLiteral("查询"), "--enabled", "false", "--packed", "true"});
            QVERIFY2(first.code == 0, first.diagnostic.constData());
            firstId = first.body.value("commandId").toString();
            QCOMPARE(table->rowCount(), 1);
            QCOMPARE(table->commandSet().commands.first().id, QUuid(firstId));
            QVERIFY(table->commandSet().commands.first().isHex);
            QCOMPARE(table->commandSet().commands.first().tailBytes(), QByteArray("\r\n"));
            QCOMPARE(table->commandSet().commands.first().packMode, QStringLiteral("组包"));
            const auto second = call({"commands", "add", groupId, "--content", "AT+VERSION?", "--position", "0"});
            QCOMPARE(second.code, 0);
            secondId = second.body.value("commandId").toString();
            QCOMPARE(table->commandSet().commands.first().id, QUuid(secondId));
            QCOMPARE(call({"commands", "update", groupId, firstId, "--description", QStringLiteral("查询版本"), "--position", "0"}).code, 0);
            QCOMPARE(table->commandSet().commands.first().content, QString("AA 55"));
            QCOMPARE(table->commandSet().commands.first().delayMs, 250);
            QCOMPARE(table->commandSet().commands.first().description, QStringLiteral("查询版本"));
            QVERIFY(!table->commandSet().commands.first().enabled);
            QCOMPARE(call({"commands", "update-group", groupId, "--name", "renamed"}).code, 0);
            QCOMPARE(selector->currentText(), QString("renamed"));
            const auto page = call({"commands", "get", groupId, "--limit", "1"});
            QCOMPARE(page.code, 0);
            QVERIFY(page.body.value("hasMore").toBool());
            QCOMPARE(page.body.value("commands").toArray().first().toObject().value("id").toString(), firstId);
            QCOMPARE(call({"commands", "get", groupId, "--offset", "1"}).body.value("commands").toArray().first()
                .toObject().value("id").toString(), secondId);
            const auto before = table->commandSet().toJson();
            QCOMPARE(call({"commands", "update", groupId, firstId, "--position", "50", "--content", "must not apply"}).code, 5);
            QCOMPARE(table->commandSet().toJson(), before);
            // A GUI edit after a CLI change must retain fields with no table editor.
            const auto delays = table->findChildren<QSpinBox *>();
            QVERIFY(!delays.isEmpty());
            delays.first()->setValue(300);
            QCOMPARE(table->commandSet().commands.first().packMode, QStringLiteral("组包"));
            QCOMPARE(table->commandSet().commands.last().description, QString());
            QCOMPARE(sends.size(), 0);
        }
        {
            MainWindow reloaded;
            const auto group = CommandManager::instance()->getSet(QUuid(groupId));
            QCOMPARE(group.name, QString("renamed"));
            QCOMPARE(group.notes, QString("CLI notes"));
            QCOMPARE(group.commands.size(), 2);
            QCOMPARE(group.commands.first().id, QUuid(firstId));
            QCOMPARE(group.commands.first().content, QString("AA 55"));
            QCOMPARE(group.commands.first().packMode, QStringLiteral("组包"));
            QCOMPARE(group.commands.last().id, QUuid(secondId));
            auto *control = reloaded.findChild<LocalControlServer *>();
            control->stop();
            QVERIFY(control->start(directory.path()));
            QCOMPARE(cliProcess({"commands", "delete", groupId, firstId}, directory.path()).code, 0);
            QCOMPARE(CommandManager::instance()->getSet(QUuid(groupId)).commands.size(), 1);
            QCOMPARE(cliProcess({"commands", "delete-group", groupId}, directory.path()).code, 0);
            QCOMPARE(reloaded.findChild<QComboBox *>("commandSetSelector")->findText("renamed"), -1);
        }
    }

    void commandEditingWaitsForPendingSendsAndGuiEditor()
    {
        MainWindow window;
        auto *panel = window.findChild<CommandPanel *>();
        auto *table = window.findChild<CommandTableWidget *>();
        auto *manager = CommandManager::instance();
        const auto groupId = manager->allSets().first().id.toString();
        const auto first = window.executeAutomationTool("qqcom_add_command", {{"groupId", groupId}, {"content", "draft"}, {"delayMs", 200}});
        QVERIFY(first.value("created").toBool());
        const auto before = manager->getSet(QUuid(groupId)).toJson();
        const QJsonObject edit{{"groupId", groupId}, {"name", "edited"}};
        CommandItem command;
        command.content = "pending";
        command.delayMs = 100;
        QSignalSpy sends(panel, &CommandPanel::sendCommand);
        QVERIFY(QMetaObject::invokeMethod(panel, "onSendCommand", Qt::DirectConnection, Q_ARG(CommandItem, command)));
        QCOMPARE(window.executeAutomationTool("qqcom_update_command_group", edit).value("error").toObject().value("code").toString(), QString("commands_busy"));
        QTRY_COMPARE(sends.size(), 1);
        QCOMPARE(manager->getSet(QUuid(groupId)).toJson(), before);
        QVERIFY(QMetaObject::invokeMethod(panel, "onBatchSendRequested", Qt::DirectConnection,
            Q_ARG(QList<int>, QList<int>{0}), Q_ARG(QList<CommandItem>, QList<CommandItem>{command}), Q_ARG(bool, true)));
        QVERIFY(panel->hasPendingSends());
        QVERIFY(window.executeAutomationTool("qqcom_update_command_group", edit).contains("error"));
        QVERIFY(QMetaObject::invokeMethod(panel, "onBatchSendStopRequested", Qt::DirectConnection));
        QVERIFY(!panel->hasPendingSends());
        // Opening a delegate editor must not let an external patch destroy its draft.
        panel->setExpanded(true);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        auto *grid = table->findChild<QTableWidget *>();
        QVERIFY(grid);
        grid->editItem(grid->item(0, 2));
        QVERIFY(table->hasActiveEditor());
        QCOMPARE(window.executeAutomationTool("qqcom_update_command_group", edit).value("error").toObject().value("code").toString(), QString("commands_busy"));
    }

    void cliOfflineDiscoveryAndInstanceSelection()
    {
        if (!QFileInfo::exists(productCli())) QSKIP("Build QQCom first or set QQCOM_CLI_TEST_EXECUTABLE");
        QTemporaryDir directory;
        const auto describe = cliProcess({"describe"}, directory.path());
        QCOMPARE(describe.code, 0);
        QCOMPARE(describe.body.value("tools").toArray().size(), CapabilityCatalog::tools().size());
        QVERIFY(!describe.body.value("cli").toObject().value("clientConfigurationRequired").toBool(true));
        QCOMPARE(describe.body.value("cli").toObject().value("transport").toString(), QString("local-pipe"));
        QCOMPARE(cliProcess({"instances"}, directory.path()).body.value("instances").toArray().size(), 0);
        QCOMPARE(cliProcess({"state", "--no-start"}, directory.path()).code, 3);
        QCOMPARE(cliProcess({"state", "--data", "invalid"}, directory.path()).code, 2);

        MainWindow window;
        auto *control = window.findChild<LocalControlServer *>();
        QVERIFY(control);
        auto *access = window.findChild<QToolButton *>("cliButton");
        QVERIFY(access && access->menu());
        QCOMPARE(access->menu()->actions().size(), 3); // copy entry, separator, help
        QCOMPARE(window.findChildren<QLocalServer *>().size(), 1);
        QVERIFY(window.findChildren<QTcpServer *>().isEmpty());
        control->stop();
        QVERIFY(control->start(directory.path()));
        const auto id = control->descriptor().value("instanceId").toString();
        const auto state = cliProcess({"state", "--no-start"}, directory.path());
        QVERIFY2(state.code == 0, state.diagnostic.constData());
        QCOMPARE(state.body.value("instanceId").toString(), id);
        QCOMPARE(state.body.value("endpoints").toArray().size(), 2);

        LocalControlServer second("test");
        second.setHandler([](const QString &, const QJsonObject &) { return QJsonObject{{"selected", true}}; });
        QVERIFY(second.start(directory.path()));
        QCOMPARE(cliProcess({"state"}, directory.path()).code, 4);
        const auto selected = cliProcess({"state", "--instance", second.descriptor().value("instanceId").toString()}, directory.path());
        QCOMPARE(selected.code, 0);
        QVERIFY(selected.body.value("selected").toBool());
        const auto stale = second.descriptor();
        second.stop();
        QFile staleFile(directory.filePath("control-stale.json"));
        QVERIFY(staleFile.open(QIODevice::WriteOnly));
        staleFile.write(QJsonDocument(stale).toJson());
        staleFile.close();
        QCOMPARE(cliProcess({"instances"}, directory.path()).body.value("instances").toArray().size(), 1);
        QCOMPARE(cliProcess({"state", "--instance", "not-a-live-instance"}, directory.path()).code, 3);
        control->stop();
        QVERIFY(!QFileInfo::exists(directory.filePath("control-" + id + ".json")));
    }

    void cliTcpAndUdpRoundTrip()
    {
        if (!QFileInfo::exists(productCli())) QSKIP("Build QQCom first or set QQCOM_CLI_TEST_EXECUTABLE");
        QTemporaryDir directory;
        MainWindow window;
        auto *control = window.findChild<LocalControlServer *>();
        QVERIFY(control);
        control->stop();
        QVERIFY(control->start(directory.path()));
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        const auto call = [&](const QStringList &arguments) { return cliProcess(arguments, directory.path()); };
        const auto endpoint = [&](int index) {
            return window.executeAutomationTool("qqcom_get_state", {}).value("endpoints").toArray().at(index).toObject();
        };
        QCOMPARE(call({"configure", "A", "--type", "tcp-client", "--param", "client.host=127.0.0.1",
            "--param", "client.port=" + QString::number(listener.serverPort()), "--auto-recover", "false"}).code, 0);
        QVERIFY(call({"open", "A"}).body.value("submitted").toBool());
        QTRY_COMPARE(endpoint(0).value("state").toString(), QString("open"));
        QTRY_VERIFY(listener.hasPendingConnections());
        auto *peer = listener.nextPendingConnection();
        QCOMPARE(call({"send", "A", "--format", "hex", "--data", "41 00 FF", "--line-ending", "crlf"}).code, 0);
        QTRY_VERIFY(peer->bytesAvailable() >= 5);
        QCOMPARE(peer->readAll(), QByteArray::fromHex("4100ff0d0a"));
        peer->write("CLI-device-response\r\n");
        QTRY_VERIFY(window.sessionRxBytes() >= 21);
        auto *traffic = window.findChild<TrafficPanel *>();
        QTRY_VERIFY(!traffic->queryTraffic({{"direction", "TX"}}).value("records").toArray().isEmpty());
        const auto tx = call({"traffic", "--endpoint", "A", "--direction", "TX"}).body.value("records").toArray();
        QVERIFY(!tx.isEmpty());
        QCOMPARE(tx.last().toObject().value("origin").toString(), QString("AI"));
        QCOMPARE(call({"ui-text", "--max-chars", "2048"}).code, 0);
        QVERIFY(!call({"ui-text"}).body.value("text").toString().isEmpty());
        QCOMPARE(call({"close", "A"}).code, 0);
        QTRY_COMPARE(endpoint(0).value("state").toString(), QString("closed"));
        QCOMPARE(call({"send", "A", "--format", "ascii", "--data", "closed"}).code, 5);

        QUdpSocket udp;
        QVERIFY(udp.bind(QHostAddress::LocalHost, 0));
        QCOMPARE(call({"configure", "B", "--type", "udp", "--param", "udp.address=127.0.0.1",
            "--param", "udp.port=0", "--param", "udp.target=127.0.0.1",
            "--param", "udp.targetPort=" + QString::number(udp.localPort()), "--auto-recover", "false"}).code, 0);
        QCOMPARE(call({"open", "B"}).code, 0);
        QTRY_COMPARE(endpoint(1).value("state").toString(), QString("open"));
        QCOMPARE(call({"send", "B", "--format", "ascii", "--data", "udp-cli"}).code, 0);
        QTRY_VERIFY(udp.hasPendingDatagrams());
        QByteArray data(64, '\0');
        QHostAddress sender;
        quint16 senderPort = 0;
        const auto length = udp.readDatagram(data.data(), data.size(), &sender, &senderPort);
        QCOMPARE(data.left(length), QByteArray("udp-cli"));
        const auto before = window.sessionRxBytes();
        QCOMPARE(udp.writeDatagram("udp-reply", sender, senderPort), qint64(9));
        QTRY_VERIFY(window.sessionRxBytes() >= before + 9);
        QCOMPARE(call({"traffic", "--endpoint", "B", "--direction", "RX"}).code, 0);
        QCOMPARE(call({"close", "B"}).code, 0);
        QTRY_COMPARE(endpoint(1).value("state").toString(), QString("closed"));
    }

    void cliStartsAbsentWindow()
    {
        QTemporaryDir directory;
        // Use the test executable as a bounded, isolated GUI fixture. Cli::run
        // launches its own executable, exactly as in the installed product.
        const auto state = cliProcess({"state"}, directory.path(), QCoreApplication::applicationFilePath(), true);
        QVERIFY2(state.code == 0, state.diagnostic.constData());
        QCOMPARE(state.body.value("application").toString(), QString("QQCom"));
        QVERIFY(state.body.value("pid").toInteger() != QCoreApplication::applicationPid());
        QTRY_VERIFY_WITH_TIMEOUT(LocalControl::instances(directory.path()).isEmpty(), 6000);
    }

    void automationTcpClientRoundTripAndUiState()
    {
        MainWindow window;
        QTcpServer peerServer;
        QVERIFY(peerServer.listen(QHostAddress::LocalHost, 0));
        const auto call = [&](const QString &name, const QJsonObject &arguments = {}) {
            return window.executeAutomationTool(name, arguments);
        };
        const auto state = [&] {
            return window.executeAutomationTool("qqcom_get_state", {}).value("endpoints").toArray().at(0).toObject();
        };
        QVERIFY(!call("qqcom_configure_endpoint", {{"endpoint", "A"}, {"type", "tcp-client"}, {"autoRecover", false},
            {"parameters", QJsonObject{{"client.host", "127.0.0.1"}, {"client.port", peerServer.serverPort()}}}}).contains("error"));
        const auto firstOpen = window.executeAutomationTool("qqcom_open_endpoint", {{"endpoint", "A"}});
        QVERIFY(firstOpen.value("submitted").toBool());
        QVERIFY(window.executeAutomationTool("qqcom_configure_endpoint", {{"endpoint", "A"}}).contains("error"));
        // A repeated command before the worker processes the first must never toggle the endpoint closed.
        window.executeAutomationTool("qqcom_open_endpoint", {{"endpoint", "A"}});
        QTRY_COMPARE(state().value("state").toString(), QString("open"));
        QTRY_VERIFY(!state().value("operationPending").toBool());
        QTRY_VERIFY(peerServer.hasPendingConnections());
        auto *peer = peerServer.nextPendingConnection();
        QVERIFY(peer);
        const auto selectors = window.findChildren<PortSelectorWidget *>();
        QCOMPARE(selectors.at(0)->draft().kind, int(PortDraft::TcpClient));
        const auto before = window.executeAutomationTool("qqcom_get_state", {}).value("send").toObject();
        const auto sent = call("qqcom_send", {{"endpoint", "A"}, {"data", "41 00 FF"}, {"format", "hex"}, {"lineEnding", "crlf"}});
        QVERIFY(!sent.contains("error"));
        QCOMPARE(sent.value("bytes").toInt(), 5);
        QTRY_VERIFY(peer->bytesAvailable() >= 5);
        QCOMPARE(peer->readAll(), QByteArray::fromHex("4100ff0d0a"));
        peer->write("device-response\r\n");
        QTRY_VERIFY(window.sessionRxBytes() >= 17);
        auto *panel = window.findChild<TrafficPanel *>();
        QVERIFY(panel);
        QTRY_VERIFY(panel->queryTraffic({{"direction", "RX"}}).value("records").toArray().size() > 0);
        const auto history = call("qqcom_query_traffic", {{"direction", "TX"}});
        const auto tx = history.value("records").toArray();
        QVERIFY(!tx.isEmpty());
        QCOMPARE(tx.last().toObject().value("origin").toString(), QString("AI"));
        QCOMPARE(window.executeAutomationTool("qqcom_get_state", {}).value("send").toObject(), before);
        QVERIFY(!call("qqcom_read_ui_text", {{"maxChars", 1024}}).value("text").toString().isEmpty());
        QVERIFY(!call("qqcom_close_endpoint", {{"endpoint", "A"}}).contains("error"));
        QTRY_COMPARE(state().value("state").toString(), QString("closed"));
        QTRY_VERIFY(!state().value("operationPending").toBool());
        QVERIFY(call("qqcom_send", {{"endpoint", "A"}, {"data", "hello"}, {"format", "utf8"}}).contains("error"));
    }

    void automationAllPortTypesAndAtomicValidation()
    {
        MainWindow window;
        const auto configure = [&](const QString &type, const QJsonObject &parameters) {
            return window.executeAutomationTool("qqcom_configure_endpoint", {{"endpoint", "B"}, {"type", type},
                {"autoRecover", false}, {"parameters", parameters}});
        };
        QVERIFY(!configure("serial", {{"serial.port", "COM999"}, {"serial.baud", 230400}}).contains("error"));
        QVERIFY(!configure("tcp-client", {{"client.host", "localhost"}, {"client.port", 8000}}).contains("error"));
        QVERIFY(!configure("tcp-server", {{"server.address", "127.0.0.1"}, {"server.port", 0}}).contains("error"));
        QVERIFY(!configure("udp", {{"udp.address", "127.0.0.1"}, {"udp.port", 0}, {"udp.target", "127.0.0.1"}}).contains("error"));
        const auto before = window.executeAutomationTool("qqcom_get_state", {}).value("endpoints");
        QVERIFY(configure("udp", {{"udp.targetPort", 65536}}).contains("error"));
        QVERIFY(configure("udp", {{"client.port", 9000}}).contains("error"));
        QVERIFY(configure("udp", {{"udp.target", "999.0.0.1"}}).contains("error"));
        QVERIFY(window.executeAutomationTool("qqcom_configure_endpoint", {{"endpoint", "B"},
            {"framing", QJsonObject{{"mode", "delimiter"}, {"delimiterHex", "F"}}}}).contains("error"));
        QCOMPARE(window.executeAutomationTool("qqcom_get_state", {}).value("endpoints"), before);
        const auto list = window.executeAutomationTool("qqcom_list_ports", {{"refresh", true}});
        QVERIFY(list.value("localIPv4").toArray().contains("127.0.0.1"));
    }

    void automationHistoryCursorTruncationAndDisplayFilters()
    {
        TrafficPanel panel;
        PortDraft a, b;
        panel.setPorts({a, b});
        auto *model = panel.findChild<TrafficModel *>();
        auto *text = panel.findChild<TrafficTextView *>();
        QVERIFY(model && text);
        const auto message = [a](int id, const QByteArray &bytes) {
            Traffic::Message result;
            result.id = id;
            result.source = Traffic::makeSource(a, 0);
            result.direction = Traffic::Direction::Receive;
            result.raw = bytes;
            result.rawLength = bytes.size();
            return result;
        };
        panel.appendMessages({message(1, "one"), message(2, "two"), message(3, QByteArray(5000, 'x'))});
        QTRY_COMPARE(model->rowCount(), 3);
        const auto first = panel.queryTraffic({{"limit", 2}});
        QCOMPARE(first.value("records").toArray().size(), 2);
        QVERIFY(first.value("hasMore").toBool());
        const auto second = panel.queryTraffic({{"cursor", first.value("nextCursor")}});
        QCOMPARE(second.value("records").toArray().size(), 1);
        const auto record = second.value("records").toArray().first().toObject();
        QVERIFY(record.value("truncated").toBool());
        QCOMPARE(record.value("rawLength").toInt(), 5000);
        QCOMPARE(record.value("previewLength").toInt(), 4096);
        const auto displayed = panel.readDisplayedText(7);
        QCOMPARE(displayed.value("text").toString(), text->toPlainText().right(7));
        QVERIFY(displayed.value("truncated").toBool());
        model->setBudget(100000, 1);
        QVERIFY(panel.queryTraffic({{"cursor", first.value("nextCursor").toString().section(':', 0, 0) + ":0"}}).value("gap").toBool());
        panel.clearTraffic();
        QVERIFY(panel.queryTraffic({{"cursor", second.value("nextCursor")}}).contains("error"));
        QVERIFY(panel.readDisplayedText(100).value("text").toString().isEmpty());
    }

    void historyProjectionRestartsImmediatelyWithLatestConditions()
    {
        TrafficPanel panel;
        PortDraft port;
        panel.setPorts({port});
        auto *model = panel.findChild<TrafficModel *>();
        auto *text = panel.findChild<TrafficTextView *>();
        auto *format = panel.findChild<QComboBox *>("trafficFormat");
        auto *keywords = panel.findChild<QLineEdit *>("filterKeywords");
        auto *apply = panel.findChild<QPushButton *>("filterRebuild");
        auto *timestamp = panel.findChild<QAction *>("prefixTimestamp");
        QVERIFY(model && text && format && keywords && apply && timestamp);
        format->setCurrentText(QStringLiteral("UTF-8"));
        const auto message = [port](int id) {
            Traffic::Message m;
            m.id = id;
            m.timestamp = QDateTime::currentDateTime();
            m.source.endpointId = port.id;
            m.source.endpointIndex = 0;
            m.direction = Traffic::Direction::Receive;
            m.raw = id % 2 ? "alpha" : "beta";
            m.rawLength = m.raw.size();
            return m;
        };
        QList<Traffic::Message> history;
        for (int i = 1; i <= 12000; ++i) history.append(message(i));
        model->appendRecords(history);
        apply->setEnabled(true);
        QSignalSpy summaries(&panel, &TrafficPanel::summaryChanged);
        apply->click();
        QVERIFY(panel.isReprojecting());
        QCOMPARE(apply->text(), QStringLiteral("重建中…"));
        keywords->setText(QStringLiteral("beta")); // 不用再点应用，正在运行的任务立即替换。
        QVERIFY(panel.isReprojecting());
        QVERIFY(text->toPlainText().contains(QStringLiteral("beta")));
        QVERIFY(!text->toPlainText().contains(QStringLiteral("alpha")));
        format->setCurrentText(QStringLiteral("HEX"));
        timestamp->setChecked(false);
        keywords->setText(QStringLiteral("alpha"));
        QVERIFY(panel.isReprojecting());
        QVERIFY(text->toPlainText().contains(QStringLiteral("61 6C 70 68 61")));
        QVERIFY(!text->toPlainText().contains(QStringLiteral("62 65 74 61")));
        for (int i = 12001; i <= 12100; ++i) panel.appendMessage(message(i));
        QTRY_COMPARE(model->rowCount(), 12100);
        QTRY_VERIFY_WITH_TIMEOUT(!panel.isReprojecting(), 10000);
        QTest::qWait(AppearanceReprojectWaitMs);
        QCOMPARE(text->lineCount(), 6050);
        const auto visible = text->toPlainText();
        QVERIFY(visible.contains(QStringLiteral("61 6C 70 68 61")));
        QVERIFY(!visible.contains(QStringLiteral("62 65 74 61")));
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
        int completions = 0;
        for (const auto &args : summaries)
            if (args.first().toString().contains(QStringLiteral("历史重建完成"))) ++completions;
        QCOMPARE(completions, 1); // 取消的任务不能继续写入或播报完成。
    }

    void historyProjectionRefiltersArrivalsWithLatestConditions()
    {
        TrafficPanel panel;
        PortDraft port;
        panel.setPorts({port});
        auto *model = panel.findChild<TrafficModel *>();
        auto *text = panel.findChild<TrafficTextView *>();
        auto *apply = panel.findChild<QPushButton *>("filterRebuild");
        auto *keywords = panel.findChild<QLineEdit *>("filterKeywords");
        auto *format = panel.findChild<QComboBox *>("trafficFormat");
        format->setCurrentText(QStringLiteral("UTF-8"));
        QList<Traffic::Message> history;
        Traffic::Message message;
        message.source.endpointId = port.id;
        message.direction = Traffic::Direction::Receive;
        message.raw = "history-beta";
        for (int i = 0; i < 60000; ++i) history.append(message);
        model->appendRecords(history);
        apply->setEnabled(true);
        apply->click();
        QVERIFY(panel.isReprojecting());
        keywords->setText(QStringLiteral("beta"));
        message.raw = "arrival-beta";
        panel.appendMessage(message);
        message.raw = "arrival-alpha";
        panel.appendMessage(message);
        QTimer *flush = nullptr;
        for (auto *timer : panel.findChildren<QTimer *>(QString(), Qt::FindDirectChildrenOnly))
            if (timer->interval() == 16) flush = timer;
        QVERIFY(flush);
        flush->stop();
        QVERIFY(QMetaObject::invokeMethod(flush, "timeout", Qt::DirectConnection));
        QCOMPARE(model->rowCount(), 60002);
        QVERIFY(panel.isReprojecting());
        keywords->setText(QStringLiteral("alpha"));
        QTRY_VERIFY_WITH_TIMEOUT(!panel.isReprojecting(), 10000);
        QCOMPARE(text->lineCount(), 1);
        QVERIFY(text->toPlainText().contains(QStringLiteral("arrival-alpha")));
        QVERIFY(!text->toPlainText().contains(QStringLiteral("beta")));
        QCOMPARE(panel.totalAccepted(), quint64(1));
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
    }

    void repeatedIdenticalProjectionDoesNotRestart()
    {
        TrafficPanel panel;
        PortDraft port;
        panel.setPorts({port});
        auto *model = panel.findChild<TrafficModel *>();
        auto *text = panel.findChild<TrafficTextView *>();
        auto *apply = panel.findChild<QPushButton *>("filterRebuild");
        Traffic::Message message;
        message.source.endpointId = port.id;
        message.direction = Traffic::Direction::Receive;
        message.raw = "data";
        QList<Traffic::Message> history;
        for (int i = 0; i < 12000; ++i) history.append(message);
        model->appendRecords(history);
        apply->setEnabled(true);
        apply->click();
        QVERIFY(panel.isReprojecting());
        const int written = text->lineCount();
        QSignalSpy blocks(text, &QPlainTextEdit::blockCountChanged);
        apply->click();
        apply->click();
        QCOMPARE(text->lineCount(), written);
        QCOMPARE(blocks.size(), 0); // 同一请求不能清空已经写入的批次。
        QTRY_VERIFY_WITH_TIMEOUT(!panel.isReprojecting(), 10000);
        for (const auto &args : blocks) QVERIFY(args.first().toInt() != 1);
        QCOMPARE(text->lineCount(), 12000);

        blocks.clear();
        apply->click(); // 完成后相同请求也应复用结果。
        QVERIFY(!panel.isReprojecting());
        QCOMPARE(blocks.size(), 0);
        QCOMPARE(text->lineCount(), 12000);

        panel.appendMessage(message); // 同条件的新数据已增量显示，无需再扫描历史。
        QTRY_COMPARE(model->rowCount(), 12001);
        QTRY_COMPARE(text->lineCount(), 12001);
        blocks.clear();
        apply->click();
        QVERIFY(!panel.isReprojecting());
        QCOMPARE(blocks.size(), 0);
        QCOMPARE(text->lineCount(), 12001);
    }

    void identicalProjectionRefiltersTemporaryConditionArrivals_data()
    {
        QTest::addColumn<bool>("acceptedDuringChange");
        QTest::newRow("temporarily-rejected") << false;
        QTest::newRow("temporarily-accepted") << true;
    }

    void identicalProjectionRefiltersTemporaryConditionArrivals()
    {
        QFETCH(bool, acceptedDuringChange);
        TrafficPanel panel;
        PortDraft port;
        panel.setPorts({port});
        auto *model = panel.findChild<TrafficModel *>();
        auto *text = panel.findChild<TrafficTextView *>();
        auto *apply = panel.findChild<QPushButton *>("filterRebuild");
        auto *keywords = panel.findChild<QLineEdit *>("filterKeywords");
        auto *format = panel.findChild<QComboBox *>("trafficFormat");
        QVERIFY(model && text && apply && keywords && format);
        format->setCurrentText(QStringLiteral("UTF-8"));
        Traffic::Message message;
        message.source.endpointId = port.id;
        message.direction = Traffic::Direction::Receive;
        message.raw = "alpha-history";
        panel.appendMessage(message);
        QTRY_COMPARE(model->rowCount(), 1);
        keywords->setText(QStringLiteral("alpha"));
        apply->click();
        QCOMPARE(text->lineCount(), 1);

        keywords->setText(QStringLiteral("beta"));
        message.raw = acceptedDuringChange ? "beta-arrival" : "alpha-arrival";
        panel.appendMessage(message);
        QTRY_COMPARE(model->rowCount(), 2);
        QCOMPARE(text->lineCount(), acceptedDuringChange ? 2 : 1);
        keywords->setText(QStringLiteral("alpha")); // 回到已完成快照，但历史已不一致。
        QCOMPARE(apply->text(), QStringLiteral("应用过滤 •"));
        apply->click();
        QCOMPARE(text->lineCount(), acceptedDuringChange ? 1 : 2);
        QVERIFY(!text->toPlainText().contains(QStringLiteral("beta-arrival")));
        QCOMPARE(text->toPlainText().contains(QStringLiteral("alpha-arrival")), !acceptedDuringChange);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
    }

    void reloadExampleRebuildsWithUnchangedConditions()
    {
        TrafficPanel panel;
        PortDraft port;
        panel.setPorts({port});
        auto *text = panel.findChild<TrafficTextView *>();
        auto *model = panel.findChild<TrafficModel *>();
        QVERIFY(text && model);
        panel.loadExample({port});
        const int exampleLines = text->lineCount();
        Traffic::Message message;
        message.source.endpointId = port.id;
        message.direction = Traffic::Direction::Receive;
        message.raw = "extra-record";
        panel.appendMessage(message);
        QTRY_COMPARE(model->rowCount(), 13);
        QTRY_COMPARE(text->lineCount(), exampleLines + 1);
        panel.loadExample({port}); // 数据源整体替换，不受相同过滤快照的去重影响。
        QCOMPARE(model->rowCount(), 12);
        QCOMPARE(text->lineCount(), exampleLines);
    }

    void completedProjectionTracksThemeChanges()
    {
        AppTheme::setMode(AppTheme::Mode::Light);
        TrafficPanel panel;
        PortDraft port;
        panel.setPorts({port});
        auto *text = panel.findChild<TrafficTextView *>();
        auto *model = panel.findChild<TrafficModel *>();
        auto *apply = panel.findChild<QPushButton *>("filterRebuild");
        QVERIFY(text && model && apply);
        Traffic::Message message;
        message.source.endpointId = port.id;
        message.direction = Traffic::Direction::Receive;
        message.raw = "data";
        panel.appendMessage(message);
        QTRY_COMPARE(model->rowCount(), 1);
        apply->click();
        const QColor light = lastFragmentColor(text->document()->firstBlock());
        AppTheme::setMode(AppTheme::Mode::Dark);
        const QColor dark = lastFragmentColor(text->document()->firstBlock());
        AppTheme::setMode(AppTheme::Mode::Light);
        QVERIFY(dark != light);
        QCOMPARE(lastFragmentColor(text->document()->firstBlock()), light);
        QSignalSpy blocks(text, &QPlainTextEdit::blockCountChanged);
        apply->click();
        QCOMPARE(blocks.size(), 0);
    }

    void receiveBatchingRestoresIntervalAfterBacklog()
    {
        TrafficPanel panel;
        PortDraft port;
        panel.setPorts({port});
        auto *model = panel.findChild<TrafficModel *>();
        QTimer *flush = nullptr;
        for (auto *timer : panel.findChildren<QTimer *>(QString(), Qt::FindDirectChildrenOnly))
            if (timer->interval() == 16) flush = timer;
        QVERIFY(flush);
        Traffic::Message message;
        message.source.endpointId = port.id;
        message.direction = Traffic::Direction::Receive;
        message.raw = "data";
        QList<Traffic::Message> batch;
        for (int i = 0; i < 600; ++i) batch.append(message);
        panel.appendMessages(batch);
        QTRY_COMPARE(model->rowCount(), 600);
        panel.appendMessage(message);
        QCOMPARE(flush->interval(), 16);
        QVERIFY(flush->remainingTime() > 0);
        QCOMPARE(model->rowCount(), 600); // 新到达先攒批，不能被上一轮的 0 ms 续批永久改变。
        QTRY_COMPARE(model->rowCount(), 601);
    }

    void clearCancelsHistoryProjection()
    {
        TrafficPanel panel;
        PortDraft port;
        panel.setPorts({port});
        auto *model = panel.findChild<TrafficModel *>();
        auto *text = panel.findChild<TrafficTextView *>();
        auto *apply = panel.findChild<QPushButton *>("filterRebuild");
        auto *clear = panel.findChild<QPushButton *>("clearTraffic");
        QList<Traffic::Message> history;
        Traffic::Message old;
        old.source.endpointId = port.id;
        old.direction = Traffic::Direction::Receive;
        old.raw = "old-session";
        for (int i = 0; i < 12000; ++i) history.append(old);
        model->appendRecords(history);
        apply->setEnabled(true);
        apply->click();
        QVERIFY(panel.isReprojecting());
        apply->click();
        clear->click();
        QVERIFY(!panel.isReprojecting());
        Traffic::Message fresh = old;
        fresh.raw = "new-session";
        panel.appendMessage(fresh);
        QTRY_COMPARE(model->rowCount(), 1);
        QTest::qWait(AppearanceReprojectWaitMs);
        QCOMPARE(text->lineCount(), 1);
        QVERIFY(!text->toPlainText().contains(QStringLiteral("old-session")));
        QVERIFY(text->toPlainText().contains(QStringLiteral("new-session")));
    }

    void alertAcknowledgementAndRecoveryHaveSeparateLifetimes()
    {
        EndpointAlertState alerts;
        QCOMPARE(alerts.level(), EndpointAlertState::Normal);
        alerts.setConnectionOpen(true);
        QCOMPARE(alerts.level(), EndpointAlertState::Normal);
        alerts.report(Traffic::Issue::EndpointFailure, QStringLiteral("连接断开"));
        alerts.setConnectionOpen(false);
        QCOMPARE(alerts.level(), EndpointAlertState::Error);
        alerts.acknowledge();
        QCOMPARE(alerts.level(), EndpointAlertState::Normal);
        QVERIFY(alerts.hasFault()); // 已查看只消除提醒，连接仍未恢复。
        QVERIFY(!alerts.blinking());
        alerts.report(Traffic::Issue::EndpointFailure, QStringLiteral("连接断开"));
        QCOMPARE(alerts.entries().last().count, quint64(2));
        QCOMPARE(alerts.level(), EndpointAlertState::Normal);
        QVERIFY(!alerts.blinking());
        QSignalSpy attention(&alerts, &EndpointAlertState::attentionRequested);
        alerts.report(Traffic::Issue::EndpointFailure, QStringLiteral("重试时连接被拒绝"));
        QCOMPARE(alerts.level(), EndpointAlertState::Normal);
        QVERIFY(!alerts.entries().last().unread);
        QVERIFY(!alerts.blinking());
        QCOMPARE(attention.size(), 0);
        alerts.setConnectionOpen(true);
        QTest::qWait(1100);
        alerts.setConnectionOpen(false); // 短暂连上不足以消除故障。
        QTest::qWait(1100);
        QCOMPARE(alerts.level(), EndpointAlertState::Normal);
        QVERIFY(alerts.hasFault());
        alerts.setConnectionOpen(true);
        QTRY_VERIFY_WITH_TIMEOUT(!alerts.hasFault(), 3500);
        QCOMPARE(alerts.level(), EndpointAlertState::Normal);
        QVERIFY(alerts.entries().last().resolved);

        alerts.report(Traffic::Issue::EndpointFailure, QStringLiteral("再次断开"));
        alerts.setConnectionOpen(false);
        alerts.setConnectionOpen(true);
        QTRY_COMPARE_WITH_TIMEOUT(alerts.level(), EndpointAlertState::Warning, 3500);
        QVERIFY(alerts.summary().contains(QStringLiteral("已恢复")));
        QVERIFY(!alerts.blinking());
        alerts.stopByUser();
        QCOMPARE(alerts.level(), EndpointAlertState::Normal);
    }

    void repeatedAlertsDoNotExtendBlinkingOrDowngradeFaults()
    {
        EndpointAlertState alerts;
        QSignalSpy attention(&alerts, &EndpointAlertState::attentionRequested);
        alerts.report(Traffic::Issue::EndpointFailure, QStringLiteral("打开失败"));
        QVERIFY(alerts.blinking());
        QTest::qWait(700);
        QVERIFY(!alerts.bright());
        alerts.report(Traffic::Issue::DataLoss, QStringLiteral("丢弃数据"));
        QCOMPARE(alerts.level(), EndpointAlertState::Error);
        const int started = attention.size();
        for (int n = 0; n < 6; ++n) {
            QTest::qWait(900);
            alerts.report(Traffic::Issue::EndpointFailure, QStringLiteral("打开失败"));
        }
        QCOMPARE(attention.size(), started);
        QVERIFY(!alerts.blinking());
        QVERIFY(alerts.bright());
        QCOMPARE(alerts.entries().last().count, quint64(7));
        alerts.stopByUser();
        QCOMPARE(alerts.level(), EndpointAlertState::Normal);
        QVERIFY(!alerts.blinking());
    }

    void endpointIconsRecoverAndManualStopRestoresOriginal()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        const auto original = window.windowIcon().cacheKey();
        const auto saveShot = [&](const QString &name) {
            const auto dir = qEnvironmentVariable("QQCOM_UI_ARTIFACT_DIR");
            if (dir.isEmpty()) return true;
            return QDir().mkpath(dir) && window.grab().save(dir + QLatin1Char('/') + name);
        };
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(b);
        b->findChild<QComboBox *>(QStringLiteral("portType"))->setCurrentIndex(PortDraft::TcpClient);
        b->findChild<QComboBox *>(QStringLiteral("client.host"))->setCurrentText(QStringLiteral("127.0.0.1"));
        b->findChild<QCheckBox *>(QStringLiteral("autoRecover"))->setChecked(true);
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        b->findChild<QSpinBox *>(QStringLiteral("client.port"))->setValue(listener.serverPort());
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        auto *alerts = window.findChild<EndpointAlertState *>(QStringLiteral("endpointAlertsB"));
        QVERIFY(action && alerts);
        QVERIFY(!b->findChild<QToolButton *>(QStringLiteral("endpointAlert")));
        QCOMPARE(window.windowIcon().cacheKey(), original);
        action->click();
        QTRY_VERIFY(listener.hasPendingConnections());
        auto *peer = listener.nextPendingConnection();
        QVERIFY(peer);
        QTRY_COMPARE(action->text(), QStringLiteral("断  开"));
        QCOMPARE(window.windowIcon().cacheKey(), original);
        peer->abort();
        QTRY_COMPARE(alerts->level(), EndpointAlertState::Error);
        QVERIFY(window.windowIcon().cacheKey() != original);
        QVERIFY(saveShot(QStringLiteral("alert-fault.png")));
        AppTheme::setMode(AppTheme::Mode::Dark);
        QVERIFY(saveShot(QStringLiteral("alert-fault-dark.png")));
        AppTheme::setMode(AppTheme::Mode::Light);
        QTRY_VERIFY(listener.hasPendingConnections());
        QTRY_COMPARE_WITH_TIMEOUT(alerts->level(), EndpointAlertState::Warning, 3500);
        auto *status = window.findChildren<QLabel *>(QStringLiteral("EndpointState")).value(1);
        QVERIFY(status && status->toolTip().contains(QStringLiteral("已恢复")));
        QVERIFY(saveShot(QStringLiteral("alert-recovered.png")));
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(text);
        QTest::mouseClick(text->viewport(), Qt::LeftButton);
        QTRY_COMPARE(alerts->level(), EndpointAlertState::Normal);
        QCOMPARE(window.windowIcon().cacheKey(), original);
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("连  接"));
        QCOMPARE(window.windowIcon().cacheKey(), original);
        QVERIFY(saveShot(QStringLiteral("alert-normal.png")));
    }

    void clientLeavingWarnsWithoutMarkingListenerFaulty()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(b);
        b->findChild<QComboBox *>(QStringLiteral("portType"))->setCurrentIndex(PortDraft::TcpServer);
        auto *address = b->findChild<QComboBox *>(QStringLiteral("server.address"));
        address->setCurrentIndex(address->findData(QStringLiteral("0.0.0.0")));
        QTcpServer reservation;
        QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        const auto port = reservation.serverPort();
        reservation.close();
        b->findChild<QSpinBox *>(QStringLiteral("server.port"))->setValue(port);
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        auto *alerts = window.findChild<EndpointAlertState *>(QStringLiteral("endpointAlertsB"));
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("停  止"));
        QCOMPARE(alerts->level(), EndpointAlertState::Normal); // 无客户端不是故障。
        QTcpSocket peer;
        peer.connectToHost(QHostAddress::LocalHost, port);
        QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);
        QTest::qWait(50);
        peer.abort();
        QTRY_COMPARE(alerts->level(), EndpointAlertState::Warning);
        QVERIFY(!alerts->hasFault());
        QCOMPARE(alerts->entries().last().issue, Traffic::Issue::PeerDisconnected);
        QCOMPARE(action->text(), QStringLiteral("停  止"));
        action->click();
        QTRY_COMPARE(alerts->level(), EndpointAlertState::Normal);
    }

    void endpointAlertsKeepIdentityAndAggregateIndependently()
    {
        clearEndpointDrafts();
        MainWindow window;
        const auto original = window.windowIcon().cacheKey();
        auto *a = window.findChild<EndpointAlertState *>(QStringLiteral("endpointAlertsA"));
        auto *b = window.findChild<EndpointAlertState *>(QStringLiteral("endpointAlertsB"));
        QVERIFY(a && b);
        const QIcon normalIcon = window.windowIcon();
        b->report(Traffic::Issue::DataLoss, QStringLiteral("B 单次发送失败"));
        QCOMPARE(a->level(), EndpointAlertState::Normal);
        const auto warning = window.windowIcon().cacheKey();
        const QIcon warningIcon = window.windowIcon();
        QVERIFY(warning != original);
        a->report(Traffic::Issue::EndpointFailure, QStringLiteral("A 串口拔出"));
        const auto error = window.windowIcon().cacheKey();
        QVERIFY(error != warning);
        const QIcon errorIcon = window.windowIcon();
        const auto artifactDir = qEnvironmentVariable("QQCOM_UI_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QVERIFY(QDir().mkpath(artifactDir));
            QVERIFY(normalIcon.pixmap(128, 128).save(artifactDir + QStringLiteral("/icon-normal.png")));
            QVERIFY(warningIcon.pixmap(128, 128).save(artifactDir + QStringLiteral("/icon-warning.png")));
            QVERIFY(errorIcon.pixmap(128, 128).save(artifactDir + QStringLiteral("/icon-error.png")));
        }
        for (const int size : {16, 32, 64, 256}) {
            const auto normalImage = normalIcon.pixmap(QSize(size, size), 1.0).toImage();
            const auto errorImage = errorIcon.pixmap(QSize(size, size), 1.0).toImage();
            const auto warningImage = warningIcon.pixmap(QSize(size, size), 1.0).toImage();
            QCOMPARE(errorImage.size(), normalImage.size());
            QCOMPARE(warningImage.size(), normalImage.size());
            int recolored = 0;
            for (int y = 0; y < normalImage.height(); ++y)
                for (int x = 0; x < normalImage.width(); ++x) {
                    QCOMPARE(errorImage.pixelColor(x, y).alpha(), normalImage.pixelColor(x, y).alpha());
                    const auto pixel = normalImage.pixelColor(x, y);
                    if (pixel.red() > 245 && pixel.red() == pixel.green() && pixel.green() == pixel.blue())
                        QCOMPARE(errorImage.pixelColor(x, y), pixel); // 白色图案保持可辨。
                    if (pixel.alpha() > 128 && pixel.hsvSaturation() > 120) {
                        // 原图全部有色区域都必须换色，避免只在局部叠一块告警色。
                        const auto red = errorImage.pixelColor(x, y);
                        QVERIFY(red.red() > red.green() && red.red() > red.blue());
                        const auto yellow = warningImage.pixelColor(x, y);
                        QVERIFY(yellow.red() > yellow.green() && yellow.green() > yellow.blue());
                        ++recolored;
                    }
                }
            QVERIFY(recolored > size * size / 5);
        }
        b->acknowledge();
        QCOMPARE(window.windowIcon().cacheKey(), error);
        a->acknowledge();
        QCOMPARE(a->level(), EndpointAlertState::Normal);
        QCOMPARE(window.windowIcon().cacheKey(), original);
        for (const auto *badge : window.findChildren<QLabel *>(QStringLiteral("EndpointBadge")))
            QCOMPARE(badge->text(), badge->property("port").toString());
        a->stopByUser();
        QCOMPARE(window.windowIcon().cacheKey(), original);
    }

    void endpointEditsAcknowledgeOnlyTheirEndpoint()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        const auto original = window.windowIcon().cacheKey();
        auto *a = window.findChild<EndpointAlertState *>(QStringLiteral("endpointAlertsA"));
        auto *b = window.findChild<EndpointAlertState *>(QStringLiteral("endpointAlertsB"));
        auto *selectorA = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointA"));
        auto *selectorB = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(a && b && selectorA && selectorB);
        auto *port = selectorB->findChild<QSpinBox *>(QStringLiteral("server.port"));
        auto *action = selectorB->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(port && action);
        QTcpServer blocker;
        QVERIFY(blocker.listen(QHostAddress::AnyIPv4, 0));
        port->setValue(blocker.serverPort());
        action->click();
        QTRY_COMPARE(b->level(), EndpointAlertState::Error);
        a->report(Traffic::Issue::EndpointFailure, QStringLiteral("A 设备拔出"));

        // 自动扫描以及草稿载入都不表示用户看过了，不能消掉提醒。
        selectorA->setSerialPorts({});
        selectorB->setSerialPorts({});
        selectorB->setDraft(selectorB->draft());
        QCOMPARE(a->level(), EndpointAlertState::Error);
        QCOMPARE(b->level(), EndpointAlertState::Error);
        auto *baud = selectorA->findChild<QComboBox *>(QStringLiteral("serial.baud"));
        QVERIFY(baud);
        baud->setCurrentText(baud->currentText() == QStringLiteral("9600")
            ? QStringLiteral("115200") : QStringLiteral("9600"));
        QCOMPARE(a->level(), EndpointAlertState::Normal);
        QCOMPARE(b->level(), EndpointAlertState::Error);
        port->setValue(port->value() == 65535 ? 65534 : port->value() + 1);
        QCOMPARE(b->level(), EndpointAlertState::Normal);
        QVERIFY(b->hasFault());
        QCOMPARE(window.windowIcon().cacheKey(), original);
        port->setValue(blocker.serverPort());

        // 用户手动再试一次：先消旧提醒，再次失败才重新提醒。
        QSignalSpy attention(b, &EndpointAlertState::attentionRequested);
        action->click();
        QCOMPARE(b->level(), EndpointAlertState::Normal);
        QTRY_COMPARE(b->level(), EndpointAlertState::Error);
        QCOMPARE(attention.size(), 1);
        a->beginUserAttempt();
        a->report(Traffic::Issue::EndpointFailure, QStringLiteral("A 仍未打开"));
        port->setValue(port->value() == 65535 ? 65534 : port->value() + 1);
        QCOMPARE(b->level(), EndpointAlertState::Normal);
        QCOMPARE(a->level(), EndpointAlertState::Error);
    }

    void windowInteractionAcknowledgesAlertsWithoutChangingConnection_data()
    {
        QTest::addColumn<QString>("interaction");
        QTest::newRow("click") << QStringLiteral("click");
        QTest::newRow("activate") << QStringLiteral("activate");
        QTest::newRow("key") << QStringLiteral("key");
    }

    void windowInteractionAcknowledgesAlertsWithoutChangingConnection()
    {
        QFETCH(QString, interaction);
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        QTest::qWait(30);
        const auto original = window.windowIcon().cacheKey();
        auto *a = window.findChild<EndpointAlertState *>(QStringLiteral("endpointAlertsA"));
        auto *b = window.findChild<EndpointAlertState *>(QStringLiteral("endpointAlertsB"));
        auto *selector = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(a && b && selector && model);
        QVERIFY(!window.findChild<QToolButton *>(QStringLiteral("endpointAlert")));
        selector->findChild<QComboBox *>(QStringLiteral("portType"))->setCurrentIndex(PortDraft::TcpClient);
        selector->findChild<QCheckBox *>(QStringLiteral("autoRecover"))->setChecked(false);
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        selector->findChild<QComboBox *>(QStringLiteral("client.host"))->setCurrentText(QStringLiteral("127.0.0.1"));
        selector->findChild<QSpinBox *>(QStringLiteral("client.port"))->setValue(listener.serverPort());
        auto *action = selector->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        action->click();
        QTRY_VERIFY(listener.hasPendingConnections());
        auto *peer = listener.nextPendingConnection();
        QVERIFY(peer);
        QTRY_COMPARE(action->text(), QStringLiteral("断  开"));
        peer->abort();
        QTRY_COMPARE(b->level(), EndpointAlertState::Error);
        QTRY_COMPARE(action->text(), QStringLiteral("连  接"));
        QTRY_VERIFY(model->rowCount() > 0
            && model->record(model->rowCount() - 1).direction == Traffic::Direction::Error);
        a->report(Traffic::Issue::DataLoss, QStringLiteral("A 发送失败"));
        QVERIFY(window.windowIcon().cacheKey() != original);

        // 刷新、悬停和其他窗口的输入都不确认本窗口的告警。
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(text);
        QTest::mouseMove(text->viewport(), QPoint(10, 10));
        QLineEdit unrelated;
        QTest::keyClicks(&unrelated, QStringLiteral("x"));
        QCOMPARE(unrelated.text(), QStringLiteral("x"));
        QCOMPARE(b->level(), EndpointAlertState::Error);
        QCOMPARE(a->level(), EndpointAlertState::Warning);
        const int rows = model->rowCount();
        const QString reason = model->record(rows - 1).statusText;
        auto *note = window.findChild<QLabel *>(QStringLiteral("StatusNote"));
        QVERIFY(note && note->text().contains(QStringLiteral("断开")));
        const QString noteText = note->text();

        if (interaction == QLatin1String("click")) {
            auto *wrap = window.findChild<QPushButton *>(QStringLiteral("viewAutoWrap"));
            QVERIFY(wrap);
            const bool before = wrap->isChecked();
            QTest::mouseClick(wrap, Qt::LeftButton);
            QCOMPARE(wrap->isChecked(), !before); // 确认提醒不吞掉原有点击。
        } else if (interaction == QLatin1String("activate")) {
            QEvent activate(QEvent::WindowActivate);
            QCoreApplication::sendEvent(&window, &activate);
        } else {
            auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("sendEditor"));
            QVERIFY(editor);
            editor->clear();
            QTest::keyClicks(editor, QStringLiteral("x"));
            QCOMPARE(editor->toPlainText(), QStringLiteral("x"));
        }
        QCOMPARE(a->level(), EndpointAlertState::Normal);
        QCOMPARE(b->level(), EndpointAlertState::Normal);
        QVERIFY(!a->blinking() && !b->blinking());
        QCOMPARE(window.windowIcon().cacheKey(), original);
        QVERIFY(b->hasFault()); // 消色不冒充重连成功，也不启动自动恢复。
        QCOMPARE(action->text(), QStringLiteral("连  接"));
        QVERIFY(!action->property("recovering").toBool());
        QCOMPARE(model->rowCount(), rows);
        QCOMPARE(model->record(rows - 1).statusText, reason);
        QCOMPARE(note->text(), noteText);
        b->report(Traffic::Issue::EndpointFailure, QStringLiteral("同次故障的后续结果"));
        QCOMPARE(b->level(), EndpointAlertState::Normal);
        QVERIFY(!b->blinking());
        b->beginUserAttempt();
        b->report(Traffic::Issue::EndpointFailure, QStringLiteral("新的手动连接失败"));
        QCOMPARE(b->level(), EndpointAlertState::Error); // 新故障仍可重新提醒。
    }

    void eventOptionsFollowPrefixAndRememberChoices()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PreferencesStore store(nullptr, dir.filePath(QStringLiteral("preferences.ini")));
        const QStringList names{QStringLiteral("showInfo"), QStringLiteral("showWarning"),
            QStringLiteral("showError"), QStringLiteral("showState")};
        const QStringList keys{QStringLiteral("filter/info"), QStringLiteral("filter/warning"),
            QStringLiteral("filter/error"), QStringLiteral("filter/state")};
        const QList<bool> choices{false, true, true, false};
        {
            TrafficPanel panel;
            panel.setPreferences(&store);
            auto *prefix = panel.findChild<QAction *>(QStringLiteral("prefix"));
            QVERIFY(prefix && prefix->isChecked());
            for (int i = 0; i < names.size(); ++i) {
                auto *event = panel.findChild<QAction *>(names.at(i));
                QVERIFY(event && event->isEnabled());
                event->setChecked(choices.at(i));
            }
            for (int n = 0; n < 2; ++n) {
                prefix->setChecked(false);
                for (int i = 0; i < names.size(); ++i) {
                    auto *event = panel.findChild<QAction *>(names.at(i));
                    QVERIFY(!event->isChecked());
                    QVERIFY(!event->isEnabled());
                    QCOMPARE(store.value(keys.at(i)).toBool(), choices.at(i));
                }
                prefix->setChecked(true);
                for (int i = 0; i < names.size(); ++i) {
                    auto *event = panel.findChild<QAction *>(names.at(i));
                    QVERIFY(event->isEnabled());
                    QCOMPARE(event->isChecked(), choices.at(i));
                }
            }
            prefix->setChecked(false);
        }
        // 纯数据下退出重进，仍置灰但不能丢掉原来的分类选择。
        TrafficPanel restored;
        restored.setPreferences(&store);
        restored.setPreferences(&store); // 已是纯数据时再次载入同样有效。
        auto *prefix = restored.findChild<QAction *>(QStringLiteral("prefix"));
        QVERIFY(prefix && !prefix->isChecked());
        for (const auto &name : names) {
            auto *event = restored.findChild<QAction *>(name);
            QVERIFY(event && !event->isEnabled() && !event->isChecked());
        }
        prefix->setChecked(true);
        for (int i = 0; i < names.size(); ++i)
            QCOMPARE(restored.findChild<QAction *>(names.at(i))->isChecked(), choices.at(i));
        prefix->setChecked(false);
        auto *reset = restored.findChild<QPushButton *>(QStringLiteral("filterReset"));
        QVERIFY(reset && reset->isEnabled());
        reset->click();
        for (int i = 0; i < names.size(); ++i) {
            auto *event = restored.findChild<QAction *>(names.at(i));
            QVERIFY(!event->isEnabled() && !event->isChecked());
            QVERIFY(store.value(keys.at(i)).toBool());
        }
        prefix->setChecked(true);
        for (const auto &name : names) {
            auto *event = restored.findChild<QAction *>(name);
            QVERIFY(event->isEnabled() && event->isChecked());
        }
    }

    void initTestCase()
    {
        QApplication::setStyle("Fusion");
        // 显示选项现在会经 PreferencesStore 落盘（测试模式重定向到 qttest 目录），
        // 但该文件在两次运行之间残留：上一次的格式切换会改变这一次的默认值。
        // 每次从干净配置开始，用例的"默认状态"假设才稳定成立。
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                      + QStringLiteral("/preferences.ini"));
        // 用例按浅色样式表里的具体颜色断言。进程钉在亮色上，不跟本机的深色系统走。
        // 外观用例会切走，cleanup 再钉回来。
        AppTheme::setMode(AppTheme::Mode::Light);
    }

    void payload_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<QString>("input");
        QTest::addColumn<QByteArray>("lineEnding");
        QTest::addColumn<QByteArray>("expected");
        using F = PayloadCodec::Format;
        QTest::newRow("hex") << int(F::Hex) << "00 7F 80 FF" << QByteArray() << QByteArray::fromHex("007f80ff");
        QTest::newRow("compact") << int(F::Hex) << "007F80FF" << QByteArray() << QByteArray::fromHex("007f80ff");
        QTest::newRow("prefix") << int(F::Hex) << "0x01 0xA0" << QByteArray() << QByteArray::fromHex("01a0");
        QTest::newRow("escape") << int(F::Escape) << "A\\r\\n\\0\\xFF" << QByteArray() << QByteArray::fromHex("410d0a00ff");
        QTest::newRow("utf8") << int(F::Utf8) << QStringLiteral("中") << QByteArray() << QByteArray::fromHex("e4b8ad");
        QTest::newRow("gbk") << int(F::Gbk) << QStringLiteral("你好") << QByteArray() << QByteArray::fromHex("c4e3bac3");
        QTest::newRow("dec") << int(F::Dec) << "1 10 255" << QByteArray() << QByteArray::fromHex("010aff");
        QTest::newRow("dec-comma") << int(F::Dec) << "1, 10;255" << QByteArray() << QByteArray::fromHex("010aff");
        QTest::newRow("base64") << int(F::Base64) << "AUE=" << QByteArray() << QByteArray::fromHex("0141");
        QTest::newRow("base64-ws") << int(F::Base64) << "AU E=\r\n" << QByteArray() << QByteArray::fromHex("0141");
        QTest::newRow("base64-nopad") << int(F::Base64) << "AUE" << QByteArray() << QByteArray::fromHex("0141");
        QTest::newRow("suffix") << int(F::Utf8) << "AT" << QByteArray("\r\n") << QByteArray("AT\r\n");
        QTest::newRow("backslash") << int(F::Escape) << "a\\\\b" << QByteArray() << QByteArray("a\\b");
    }

    void payload()
    {
        QFETCH(int, format);
        QFETCH(QString, input);
        QFETCH(QByteArray, lineEnding);
        QFETCH(QByteArray, expected);
        const auto result = PayloadCodec::parse(input, PayloadCodec::Format(format), lineEnding);
        QVERIFY2(result.valid(), qPrintable(result.error));
        QCOMPARE(result.bytes, expected);
    }

    void invalidPayload_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<QString>("input");
        using F = PayloadCodec::Format;
        QTest::newRow("odd") << int(F::Hex) << "A";
        QTest::newRow("illegal") << int(F::Hex) << "01 GG";
        QTest::newRow("prefix-short") << int(F::Hex) << "0x1";
        QTest::newRow("comma") << int(F::Hex) << "01,02";
        QTest::newRow("dec-range") << int(F::Dec) << "256";
        QTest::newRow("dec-float") << int(F::Dec) << "1.5";
        QTest::newRow("dec-negative") << int(F::Dec) << "-1";
        QTest::newRow("base64-short") << int(F::Base64) << "A";
        QTest::newRow("base64-char") << int(F::Base64) << "AUE*";
        QTest::newRow("base64-extra-pad") << int(F::Base64) << "AUE==";
        QTest::newRow("short-escape") << int(F::Escape) << "\\xF";
        QTest::newRow("unknown-escape") << int(F::Escape) << "\\q";
        QTest::newRow("trailing-escape") << int(F::Escape) << "a\\";
        QTest::newRow("unrepresentable") << int(F::Ascii) << QStringLiteral("中");
    }

    void invalidPayload()
    {
        QFETCH(int, format);
        QFETCH(QString, input);
        const auto result = PayloadCodec::parse(input, PayloadCodec::Format(format));
        QVERIFY(!result.valid());
        QVERIFY(result.bytes.isEmpty());
    }

    void payloadSizeIncludesSuffix()
    {
        QVERIFY(PayloadCodec::parse(QString(65536, 'a'), PayloadCodec::Format::Utf8).valid());
        QVERIFY(!PayloadCodec::parse(QString(65536, 'a'), PayloadCodec::Format::Utf8, "\r\n").valid());
    }

    void renderFormats()
    {
        using F = PayloadCodec::Format;
        // "AB\r\n\t" + UTF-8 的“中”（E4 B8 AD） + 独立的 0xFF：覆盖可打印、控制、多字节与非法序列。
        const auto payload = QByteArray::fromHex("41420d0a09e4b8adff");
        QCOMPARE(PayloadCodec::render(payload, F::Hex), QStringLiteral("41 42 0D 0A 09 E4 B8 AD FF"));
        QCOMPARE(PayloadCodec::render(payload, F::Dec), QStringLiteral("65 66 13 10 9 228 184 173 255"));
        QCOMPARE(PayloadCodec::render(QByteArray("AE"), F::Base64), QStringLiteral("QUU="));
        QCOMPARE(PayloadCodec::render(payload, F::Utf8),
                 QStringLiteral("AB\\r\\n\\t中") + QString(QChar(0xFFFD)));
        QCOMPARE(PayloadCodec::render(QByteArray::fromHex("c4e3bac3"), F::Gbk), QStringLiteral("你好"));
        QCOMPARE(PayloadCodec::render(payload, F::Ascii),
                 QStringLiteral("AB\\r\\n\\t\\xE4\\xB8\\xAD\\xFF"));
        QCOMPARE(PayloadCodec::render(payload, F::Escape),
                 QStringLiteral("AB\\r\\n\\t\\xE4\\xB8\\xAD\\xFF"));
        // \0 与 \\ 的渲染分支：任何字节序列经转义字节渲染后必须能无损还原。
        const auto nulBackslash = QByteArray::fromHex("41005c4200");
        QCOMPARE(PayloadCodec::render(nulBackslash, F::Escape), QStringLiteral("A\\0\\\\B\\0"));
        QCOMPARE(PayloadCodec::parse(PayloadCodec::render(nulBackslash, F::Escape), F::Escape).bytes,
                 nulBackslash);
        // 渲染与解析互为逆变换：字节序列经 HEX/DEC/BASE64/转义字节往返保持不变。
        for (const auto format : {F::Hex, F::Dec, F::Base64, F::Escape}) {
            const auto round = PayloadCodec::parse(PayloadCodec::render(payload, format), format);
            QVERIFY2(round.valid(), qPrintable(round.error));
            QCOMPARE(round.bytes, payload);
        }
        // ASCII 是纯文本编码：可打印字符往返不变；控制字符交给“转义字节”格式表达。
        const QByteArray asciiBytes("AB");
        QCOMPARE(PayloadCodec::parse(PayloadCodec::render(asciiBytes, F::Ascii), F::Ascii).bytes, asciiBytes);
        QCOMPARE(PayloadCodec::parse(QStringLiteral("你好"), F::Gbk).bytes, QByteArray::fromHex("c4e3bac3"));
        // GBK 渲染是容错的：非法序列不报错、不影响后续字节（替换符呈现）。
        const QString gbkMixed = PayloadCodec::render(QByteArray::fromHex("c4e3ff41"), F::Gbk);
        QVERIFY(gbkMixed.startsWith(QStringLiteral("你")));
        QVERIFY(gbkMixed.endsWith(QStringLiteral("A")));
        // 空输入 + 行尾：字节数恰为行尾本身。
        QCOMPARE(PayloadCodec::parse(QString(), F::Utf8, "\r\n").bytes, QByteArray("\r\n"));
        // 输入侧上限（256 KiB 字符）先于输出侧上限（64 KiB 字节）生效。
        QVERIFY(PayloadCodec::parse(QString(256 * 1024 + 1, 'a'), F::Utf8)
                    .error.contains(QStringLiteral("输入过长")));
        // 组合框顺序与名称来自同一份 formats()，两个面板不会各写一份。
        const auto formats = PayloadCodec::formats();
        QCOMPARE(formats.size(), 7);
        QCOMPARE(PayloadCodec::formatName(formats.first()), QStringLiteral("HEX"));
        QCOMPARE(PayloadCodec::formatName(formats.at(3)), QStringLiteral("UTF-8"));
        QCOMPARE(PayloadCodec::formatName(formats.last()), QStringLiteral("转义字节"));
    }

    void filtersPreserveRawData()
    {
        TrafficModel model;
        const auto a = QUuid::createUuid(), b = QUuid::createUuid();
        PortDraft portA, portB;
        portA.id = a;
        portA.kind = PortDraft::Serial;
        portB.id = b;
        portB.kind = PortDraft::TcpServer;

        const auto sample = [&](quint64 id, const PortDraft &port, int index, Traffic::Direction direction,
                                const QByteArray &raw, Traffic::Framing framing = Traffic::Framing::Message) {
            return Traffic::makeSampleMessage(id, QDateTime(), Traffic::makeSource(port, index), direction, raw, framing);
        };

        model.replaceRecords({sample(1, portA, 0, Traffic::Direction::Receive, QByteArray::fromHex("007fff")),
            sample(2, portB, 1, Traffic::Direction::Transmit, "hello")});
        // 用例统一按 HEX 格式判定，断言走静态 accepts。
        const auto accepted = [&model](const TrafficFilter::Options &options) {
            int count = 0;
            for (int i = 0; i < model.rowCount(); ++i)
                if (TrafficFilter::accepts(model.record(i), options, PayloadCodec::Format::Hex)) ++count;
            return count;
        };
        // 端点为独立开关：勾选集合内的才显示，一个都不勾则不显示。
        TrafficFilter::Options options;
        options.endpoints = {a, b};
        options.keywords = {QStringLiteral("7F FF")};
        QCOMPARE(accepted(options), 1);
        // 原始字节是事实依据，任何过滤与格式设置都改不了模型里的记录。
        QCOMPARE(model.record(0).raw, QByteArray::fromHex("007fff"));
        options.keywords.clear();
        options.endpoints = {a};
        options.tx = false;
        QCOMPARE(accepted(options), 1);
        options.endpoints = {b};
        options.rx = false;
        options.tx = true;
        options.keywords = {QStringLiteral("HELLO")};
        QCOMPARE(accepted(options), 1);
        options.keywords.clear();
        options.tx = false;
        QCOMPARE(accepted(options), 0);
        options.endpoints.clear();
        QCOMPARE(accepted(options), 0);

        // 桥接状态没有端点身份。A、B 有一端可见就留下，两端都关掉才隐藏。
        auto bridge = sample(3, portA, 0, Traffic::Direction::State, {});
        bridge.source = {};
        bridge.statusText = QStringLiteral("桥接已建立 · A · COM3 → B · 127.0.0.1:9000");
        model.replaceRecords({model.record(0), model.record(1), bridge});
        options.tx = true;
        options.rx = true;
        options.endpoints = {a};
        QCOMPARE(accepted(options), 2);
        options.endpoints = {b};
        options.rx = true;
        QCOMPARE(accepted(options), 2);
        options.endpoints = {a, b};
        QCOMPARE(accepted(options), 3);
        options.endpoints.clear();
        QCOMPARE(accepted(options), 0);
    }

    // 内容条件的分组语义：关键词组内 OR、排除词压过关键词、大小写开关、
    // 非法正则**不参与判定**（退化为不过滤，绝不静默吞掉整屏数据）。
    void contentConditionsCombineAsGroups()
    {
        PortDraft port;
        port.kind = PortDraft::Serial;
        const auto source = Traffic::makeSource(port, 0);
        const auto make = [&source](const QByteArray &raw) {
            return Traffic::makeSampleMessage(1, QDateTime(), source, Traffic::Direction::Receive, raw);
        };
        const auto heartbeat = make(QByteArray("heartbeat ok"));
        const auto temperature = make(QStringLiteral("温度 25.0").toUtf8());
        const auto upper = make(QByteArray("HEARTBEAT"));

        const auto hits = [&](const TrafficFilter::Options &options) {
            int count = 0;
            for (const auto &record : {heartbeat, temperature, upper})
                if (TrafficFilter::accepts(record, options, PayloadCodec::Format::Utf8)) ++count;
            return count;
        };

        TrafficFilter::Options options;
        options.endpoints = {source.endpointId};
        // 组内 OR：两个词命中任一即显示，大小写默认不敏感。
        options.keywords = {QStringLiteral("heartbeat"), QStringLiteral("温度")};
        QCOMPARE(hits(options), 3);
        // 排除词优先于关键词：命中即隐藏，压过已满足的关键词。
        options.excludes = {QStringLiteral("温度")};
        QCOMPARE(hits(options), 2);
        options.excludes.clear();
        options.caseSensitive = true;
        QCOMPARE(hits(options), 2); // 区分大小写后 HEARTBEAT 不再命中

        // 正则支持已移除（条件一律按纯文本子串匹配）：`heart.*` 里的 `.*` 不再是通配，
        // 整串原样去找，因此一条也命中不了——这条断言就是"没人悄悄把它加回来"的看门人。
        options.caseSensitive = false;
        options.keywords = {QStringLiteral("heart.*")};
        QCOMPARE(hits(options), 0);
    }

    // 形态铁律：条件条恒为一行（不因条件增多或窗口变窄而换行）、每个按钮都带字形、
    // 匹配选项就摆在条上而不是收进下拉、折叠只隐藏第二行而不清条件。
    void filterBarKeepsToASingleRow()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *bar = window.findChild<QWidget *>(QStringLiteral("filterBar"));
        auto *toggle = window.findChild<QPushButton *>(QStringLiteral("filterToggle"));
        QVERIFY(bar && toggle);
        // 默认展开，且条子高度就是一行控件——"两行头"的全部意义都在这里。
        QVERIFY(toggle->isChecked());
        QVERIFY(bar->isVisibleTo(&window));
        const int rowHeight = window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))->height();
        QVERIFY2(bar->height() <= rowHeight + 12,
            qPrintable(QStringLiteral("过滤条高 %1 超过一行 %2").arg(bar->height()).arg(rowHeight)));

        // 条子上的每个按钮都必须带 Material 字形：只有文字说明的按钮在窄宽度下会先被牺牲。
        // 正则开关已移除（条件一律按纯文本匹配），"区分大小写"是条上唯一的匹配选项。
        const QStringList named{QStringLiteral("filterCaseSensitive"),
            QStringLiteral("filterRebuild"), QStringLiteral("filterReset")};
        for (const auto &name : named) {
            auto *button = bar->findChild<QPushButton *>(name);
            QVERIFY2(button, qPrintable(QStringLiteral("缺按钮：") + name));
            QVERIFY2(!button->icon().isNull(), qPrintable(QStringLiteral("缺字形：") + name));
        }
        // 匹配选项是条上的按钮，不是下拉列表：既没有菜单，也没有失去"一眼可见"的入口。
        auto *caseButton = bar->findChild<QPushButton *>(QStringLiteral("filterCaseSensitive"));
        QVERIFY(caseButton && caseButton->isCheckable());
        QVERIFY(!caseButton->menu());
        QVERIFY(!bar->findChild<QPushButton *>(QStringLiteral("filterMore")));
        // 正则开关已整体移除：控件、菜单项、动作都不该以任何形式回来。
        QVERIFY(!bar->findChild<QPushButton *>(QStringLiteral("filterRegex")));
        QVERIFY(!window.findChild<QAction *>(QStringLiteral("filterRegex")));

        // 关键词不做 chip：输入框本身就是条件唯一的样子，不产生第二份视图；
        // 输入框也不带清除按钮（✕），"全选重输"比瞄准小叉更快。
        auto *keywords = bar->findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        auto *excludes = bar->findChild<QLineEdit *>(QStringLiteral("filterExcludes"));
        QVERIFY(keywords && excludes);
        QVERIFY(!keywords->isClearButtonEnabled() && !excludes->isClearButtonEnabled());
        keywords->setText(QStringLiteral("心跳, 0D0A ,,心跳"));
        QVERIFY(bar->findChildren<QPushButton *>(QStringLiteral("filterChip")).isEmpty());

        // 条件是逗号分隔的，且 trim / 去空 / 去重：HEX 条件里的空格因此能原样保留。
        const auto conditions = FilterBar::splitConditions(QStringLiteral("心跳, AA 55 ,,心跳,error"));
        QCOMPARE(conditions, QStringList({QStringLiteral("心跳"), QStringLiteral("AA 55"),
                                   QStringLiteral("error")}));
        QVERIFY(FilterBar::splitConditions(QString()).isEmpty());

        // 折叠只隐藏第二行：条件、判定器与记忆状态一律不动。
        toggle->setChecked(false);
        QVERIFY(!bar->isVisibleTo(&window));
        QVERIFY2(toggle->text().contains(QStringLiteral("▸")), qPrintable(toggle->text()));
        QCOMPARE(keywords->text(), QStringLiteral("心跳, 0D0A ,,心跳"));
        toggle->setChecked(true);
        QVERIFY(bar->isVisibleTo(&window));

        // 条上加过好几个控件了：最小窗口宽下第二行仍必须一行排得下（不重叠、不越右缘）。
        window.resize(1120, 660);
        QTest::qWait(30);
        const auto problem = overlapInRow(bar);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
    }

    // "重新过滤"：把当前条件应用到**已经收到**的记录上（显式回溯）。
    // 到达时判定是控制台语义，改条件不会动旧行；这个按钮是唯一的显式回溯入口，
    // 因此它必须真的重放、必须重排行号、必须自己说清"只回溯模型保留范围"。
    void rebuildAppliesConditionsToExistingRecords()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr); // 条件不落盘，避免污染共享配置
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *bar = window.findChild<QWidget *>(QStringLiteral("filterBar"));
        auto *rebuild = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        QVERIFY(text && model && bar && rebuild);
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        // 始终显示 HEX 缺省关闭。这里再关一次，保证一条消息只占一块。
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(false);
        // 还没收到任何记录时无从回溯，按钮应该是灰的。
        QVERIFY(!rebuild->isEnabled());

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        QList<Traffic::Message> batch;
        for (int i = 0; i < 6; ++i) {
            Traffic::Message message;
            message.id = i + 1;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = (i % 2 == 0) ? QByteArray("心跳 ok") : QByteArray("temp 25");
            message.rawLength = message.raw.size();
            batch.append(message);
        }
        // 先按"只收心跳"到达：半数被挡在屏幕之外。
        auto *keywords = bar->findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        keywords->setText(QStringLiteral("心跳"));
        panel->appendMessages(batch);
        QTRY_COMPARE(model->rowCount(), 6);
        QTRY_COMPARE(text->document()->blockCount(), 3); // 三条心跳
        QVERIFY(rebuild->isEnabled());

        // 放宽条件后旧行不会自己回来（控制台语义），屏幕上仍是 3 条。
        keywords->setText(QStringLiteral("心跳,temp"));
        QCOMPARE(text->document()->blockCount(), 3);

        // 点"重新过滤"才把新条件应用到已有记录：六条全中，行号从 1 连续重排。
        rebuild->click();
        QTRY_COMPARE(text->document()->blockCount(), 6);
        QCOMPARE(text->lineNumberAt(0), quint64(1));
        QCOMPARE(text->lineNumberAt(5), quint64(6));

        // 反向也成立：条件收窄后旧行同样要等这个按钮才消失。
        keywords->setText(QStringLiteral("temp"));
        QCOMPARE(text->document()->blockCount(), 6);
        rebuild->click();
        QTRY_COMPARE(text->document()->blockCount(), 3);
        for (const auto &block : text->document()->toPlainText().split(QLatin1Char('\n')))
            QVERIFY(!block.isEmpty());
    }

    // 顶栏第一行的开关组（用户钦定）：方向开关（RX / TX）与显示开关（纯数据模式 /
    // 同时显示 HEX / 自动换行）常驻，**不在**可折叠的条件条里——条件条收起来时它们
    // 仍在原位，而且每个都是"图标 + 文案"，不必悬停才知道自己在点的是什么。
    // 事件开关（状态 / 警告 / 错误）让出顶栏位置，收进显示设置菜单，但仍必须找得到：
    // 它们是"只看错误日志"的唯一入口。
    // 代价压在顶栏这一行的宽度上，所以末尾还要把窗口压到最小宽度量一次。
    void topRowSwitchGroupsStayPut()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *flags = window.findChild<QWidget *>(QStringLiteral("filterFlags"));
        auto *views = window.findChild<QWidget *>(QStringLiteral("viewToggles"));
        auto *bar = window.findChild<QWidget *>(QStringLiteral("filterBar"));
        auto *toggle = window.findChild<QPushButton *>(QStringLiteral("filterToggle"));
        QVERIFY(flags && views && bar && toggle);
        // 两组开关同住第一行：并排出现才叫"常驻顶栏"，谁被挪进第二行都会随条件条一起消失。
        QCOMPARE(views->parentWidget(), flags->parentWidget());

        const QStringList directions{QStringLiteral("filterRx"), QStringLiteral("filterTx"),
            QStringLiteral("filterForward")};
        const QStringList viewSwitches{QStringLiteral("viewPrefix"), QStringLiteral("viewAnsiColor"),
            QStringLiteral("viewAlwaysShowHex"), QStringLiteral("viewAutoWrap")};
        for (const auto &name : directions) {
            auto *button = flags->findChild<QPushButton *>(name);
            QVERIFY2(button, qPrintable(QStringLiteral("缺方向开关：") + name));
            QVERIFY2(button->isCheckable(), qPrintable(QStringLiteral("方向开关必须是开/关：") + name));
        }
        for (const auto &name : viewSwitches) {
            auto *button = views->findChild<QPushButton *>(name);
            QVERIFY2(button, qPrintable(QStringLiteral("缺显示开关：") + name));
            QVERIFY2(button->isCheckable(), qPrintable(QStringLiteral("显示开关必须是开/关：") + name));
            QVERIFY2(!button->icon().isNull(), qPrintable(QStringLiteral("缺字形：") + name));
            QVERIFY2(!button->text().isEmpty(), qPrintable(QStringLiteral("缺文案：") + name));
        }
        for (const auto &name : directions + viewSwitches) {
            // 同一个开关不能既在顶栏又在条件条里：条件分散在两个控件之间，
            // "重置一次清干净"全靠这条边界清楚。
            QVERIFY2(!bar->findChild<QPushButton *>(name),
                qPrintable(QStringLiteral("开关不应出现在条件条里：") + name));
            // 也不该再有第二个同名按钮藏在别处（顶栏只有一份）。
            QCOMPARE(window.findChildren<QPushButton *>(name).size(), 1);
        }

        // 事件开关：顶栏上已经让位，但菜单里必须还在，且是勾选型的 QAction。
        for (const auto &name : {QStringLiteral("filterState"), QStringLiteral("filterWarning"),
                 QStringLiteral("filterError")}) {
            QVERIFY2(!flags->findChild<QPushButton *>(name),
                qPrintable(QStringLiteral("事件开关不该留在顶栏：") + name));
            QVERIFY2(!views->findChild<QPushButton *>(name),
                qPrintable(QStringLiteral("事件开关不该挪进显示开关组：") + name));
        }
        for (const auto &name : {QStringLiteral("showInfo"), QStringLiteral("showWarning"),
                 QStringLiteral("showError"), QStringLiteral("showState")}) {
            auto *action = window.findChild<QAction *>(name);
            QVERIFY2(action && action->isCheckable(),
                qPrintable(QStringLiteral("缺菜单里的事件开关：") + name));
        }

        // 收起条件条后这排开关照旧可见——它们管的是"哪些行进来"与"行长什么样"，
        // 不属于被折叠的内容条件。
        toggle->setChecked(false);
        QVERIFY(!bar->isVisibleTo(&window));
        for (const auto &name : directions)
            QVERIFY2(flags->findChild<QPushButton *>(name)->isVisibleTo(&window),
                qPrintable(QStringLiteral("折叠后方向开关应仍可见：") + name));
        for (const auto &name : viewSwitches)
            QVERIFY2(views->findChild<QPushButton *>(name)->isVisibleTo(&window),
                qPrintable(QStringLiteral("折叠后显示开关应仍可见：") + name));
        toggle->setChecked(true);

        // 顶栏按钮与菜单项是同一个命令的两处门面（ViewToggles::bind）：任一侧改动、
        // 另一侧立刻跟上。两侧都不许"各记一份"，否则用户在菜单里看到的与顶栏按下的
        // 会是两个状态。
        auto *pure = views->findChild<QPushButton *>(QStringLiteral("viewPrefix"));
        auto *ansiColor = views->findChild<QPushButton *>(QStringLiteral("viewAnsiColor"));
        auto *hexEcho = views->findChild<QPushButton *>(QStringLiteral("viewAlwaysShowHex"));
        auto *lineWrap = views->findChild<QPushButton *>(QStringLiteral("viewAutoWrap"));
        auto *pureAction = window.findChild<QAction *>(QStringLiteral("prefix"));
        auto *ansiAction = window.findChild<QAction *>(QStringLiteral("showAnsi"));
        auto *hexAction = window.findChild<QAction *>(QStringLiteral("alwaysShowHex"));
        auto *wrapAction = window.findChild<QAction *>(QStringLiteral("autoWrap"));
        QVERIFY(pure && ansiColor && hexEcho && lineWrap && pureAction && ansiAction && hexAction
            && wrapAction);
        const QList<QPair<QPushButton *, QAction *>> mirrored{
            {pure, pureAction}, {ansiColor, ansiAction}, {hexEcho, hexAction}, {lineWrap, wrapAction}};
        for (const auto &pair : mirrored) {
            // 顶栏用短名，菜单写完整说明。两边勾选必须一致，文案不必相同。
            QVERIFY2(!pair.first->text().isEmpty() && !pair.second->text().isEmpty(),
                qPrintable(pair.first->objectName()));
            QVERIFY2(pair.first->text().size() < pair.second->text().size(),
                qPrintable(pair.first->text() + QStringLiteral(" / ") + pair.second->text()));
            QCOMPARE(pair.first->isChecked(), pair.second->isChecked());
            // 按钮 → 命令。
            const bool flipped = !pair.second->isChecked();
            pair.first->click();
            QCOMPARE(pair.second->isChecked(), flipped);
            // 命令 → 按钮（程序侧：恢复偏好、联动置灰走这条路）。
            pair.second->setChecked(!flipped);
            QCOMPARE(pair.first->isChecked(), !flipped);
        }

        // 关掉「显示前缀」后，时间戳、数据长度和 HEX 置灰：顶栏按钮必须同步置灰，
        // 否则留下一个"看着能点、点了一下没反应"的死开关。颜色和换行不跟前缀走。
        // 行号改画在左侧槽，菜单里那一项不再出现。
        if (!pure->isChecked()) pure->click();
        QVERIFY(pure->isChecked());
        auto *number = window.findChild<QAction *>(QStringLiteral("prefixLineNumber"));
        auto *stamp = window.findChild<QAction *>(QStringLiteral("prefixTimestamp"));
        auto *length = window.findChild<QAction *>(QStringLiteral("prefixLength"));
        QVERIFY(number && stamp && length && hexEcho->isEnabled());
        QVERIFY(!number->isVisible());
        pure->click();
        QVERIFY(!pure->isChecked());
        QCOMPARE(stamp->isEnabled(), false);
        QCOMPARE(length->isEnabled(), false);
        QVERIFY(!hexEcho->isEnabled());
        QVERIFY(!hexAction->isEnabled());
        QVERIFY(wrapAction->isEnabled());
        QVERIFY(ansiColor->isEnabled());
        QVERIFY(ansiAction->isEnabled());
        pure->click();
        QVERIFY(pure->isChecked());
        QVERIFY(stamp->isEnabled());
        QVERIFY(length->isEnabled());
        QVERIFY(hexEcho->isEnabled());

        // '过滤 · N' 仍是两处条件的合计：只关掉 RX（条件条里一条内容条件都没有）也必须算进去。
        auto *rx = flags->findChild<QPushButton *>(QStringLiteral("filterRx"));
        auto *forward = flags->findChild<QPushButton *>(QStringLiteral("filterForward"));
        QVERIFY(forward && forward->isChecked());
        // 只关掉 RX 时重置按钮必须是亮的，否则那半套条件没有回到默认的入口。
        auto *reset = bar->findChild<QPushButton *>(QStringLiteral("filterReset"));
        QVERIFY(reset && !reset->isEnabled());
        rx->setChecked(false);
        QVERIFY2(toggle->text().contains(QStringLiteral("· 1")), qPrintable(toggle->text()));
        QVERIFY(reset->isEnabled());
        // 菜单里的事件开关同样是条件：只关掉"警告"也要计数。
        auto *warnings = window.findChild<QAction *>(QStringLiteral("showWarning"));
        warnings->setChecked(false);
        QVERIFY2(toggle->text().contains(QStringLiteral("· 2")), qPrintable(toggle->text()));
        // 重置一次清干净：顶栏方向开关、菜单事件开关、条件条内容条件一起回到不过滤。
        bar->findChild<QLineEdit *>(QStringLiteral("filterKeywords"))->setText(QStringLiteral("心跳"));
        QVERIFY(toggle->text().contains(QStringLiteral("· 3")));
        forward->setChecked(false);
        QVERIFY(toggle->text().contains(QStringLiteral("· 4")));
        reset->click();
        QVERIFY(rx->isChecked());
        QVERIFY(forward->isChecked());
        QVERIFY(warnings->isChecked());
        // 「清除过滤」改为**保留输入框内容、只停用过滤**（用户反馈"又得重新输入，很麻烦"）：
        // 复位的是方向/事件开关与"生效的过滤"；条件留在框里，点「应用过滤」即可恢复。
        QCOMPARE(bar->findChild<QLineEdit *>(QStringLiteral("filterKeywords"))->text(),
                 QStringLiteral("心跳"));
        QVERIFY2(toggle->text().contains(QStringLiteral("过滤 ")), qPrintable(toggle->text()));

        // 窗口没有内容下限：布局激活之后仍然是 0，并且能停在比内容更小的尺寸上。
        auto *panel = window.findChild<TrafficPanel *>();
        QVERIFY(panel);
        qInfo() << "window minimum width" << window.minimumWidth() << "traffic minimum"
                << panel->minimumSizeHint().width();
        QCOMPARE(window.minimumWidth(), 0);
        QCOMPARE(window.minimumHeight(), 0);
        window.resize(400, 280);
        QApplication::processEvents();
        QTest::qWait(50);
        QVERIFY2(window.width() < 700,
            qPrintable(QStringLiteral("窗口宽度被弹回 %1，最小宽度 %2")
                           .arg(window.width()).arg(window.minimumWidth())));
        QVERIFY2(window.height() < 500,
            qPrintable(QStringLiteral("窗口高度被弹回 %1，最小高度 %2")
                           .arg(window.height()).arg(window.minimumHeight())));
        QCOMPARE(window.minimumWidth(), 0);
        QCOMPARE(window.minimumHeight(), 0);
        // 拉回放得下顶栏的宽度，这一行仍不重叠、不越右缘。
        window.resize(1280, 660);
        QTest::qWait(30);
        QWidget *row = flags->parentWidget();
        QVERIFY(row);
        for (const auto &name : {QStringLiteral("showPortA"), QStringLiteral("showPortB"),
                 QStringLiteral("trafficFormat"), QStringLiteral("filterToggle"),
                 QStringLiteral("filterFlags"), QStringLiteral("viewToggles"),
                 QStringLiteral("displaySettings"), QStringLiteral("exportTraffic"),
                 QStringLiteral("followTraffic"), QStringLiteral("clearTraffic"),
                 QStringLiteral("commandPanelToggle")})
            QVERIFY2(row->findChild<QWidget *>(name), qPrintable(QStringLiteral("缺顶栏元素：") + name));
        QCOMPARE(window.findChild<QPushButton *>(QStringLiteral("displaySettings"))->text(),
                 QStringLiteral("设置"));
        QVERIFY(window.findChild<QPushButton *>(QStringLiteral("followTraffic"))->isChecked());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("commandPanelToggle"))->isChecked());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("exportTraffic"))->icon().isNull());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("clearTraffic"))->icon().isNull());
        // 过滤旁的 A/B 是单字母开关，宽度应贴着字，不能被通用按钮的左右内边距撑成方块。
        auto *portA = window.findChild<QPushButton *>(QStringLiteral("showPortA"));
        auto *portB = window.findChild<QPushButton *>(QStringLiteral("showPortB"));
        auto *filterToggle = window.findChild<QPushButton *>(QStringLiteral("filterToggle"));
        QVERIFY(portA && portB && filterToggle);
        // 样式宽度不含边框：16px 内容区 + 两侧 1px 边框 = 18。比按钮高度窄，才不像方块。
        QCOMPARE(portA->width(), 18);
        QCOMPARE(portB->width(), 18);
        QVERIFY(portA->width() < filterToggle->height());
        QCOMPARE(portA->height(), filterToggle->height());
        QCOMPARE(portB->height(), filterToggle->height());
        const auto crowded = overlapInRow(row);
        QVERIFY2(crowded.isEmpty(), qPrintable(crowded));
    }

    // ★ 编辑一行**不得重建整表**——这是"拖拽崩 / 选结尾符崩"和"列表越用越卡"
    // 两个用户可见问题的共同病根。
    //
    // 病根链条：行内控件改一下 → commandsChanged → 面板 saveCurrentSet() → 落库；
    // 而落库用的 CommandManager::syncSet() 曾经转手调用 updateSet()，后者无条件
    // emit setsChanged()，于是 setsChanged → 面板重建下拉框 → 表格 setCommandSet()
    // → refreshTable() 把**正在派发信号的那个控件**一起销毁 → 回到它内部就崩。
    // 同一件事也让每次勾选/改延时都付一次全表重建的代价。
    //
    // 这条用例同时钉两件事：① 编辑后行内控件必须**还是原来那些对象**；
    // ② 编辑不得广播 setsChanged（"静默回写"必须真的静默）。
    void editingARowKeepsItsWidgetsAndStaysSilent()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 800);
        window.show();
        QTest::qWait(30);

        auto *table = window.findChild<CommandTableWidget *>();
        QVERIFY(table);

        CommandSet set;
        for (int i = 0; i < 3; ++i) {
            CommandItem item;
            item.content = QStringLiteral("+CR,%1;").arg(i);
            item.description = QStringLiteral("指令%1").arg(i);
            set.commands.append(item);
        }
        table->setCommandSet(set);
        QTRY_COMPARE(table->rowCount(), 3);
        QTest::qWait(20);

        // 行内控件三族：结尾符下拉（每行 1）、延时框（每行 1）、勾选框（每行 2——启用 + Hex）。
        // 它们的**对象身份**就是判据，所以下面的断言全部写成与遍历顺序无关的形式。
        auto *grid = table->findChild<QTableWidget *>();
        QVERIFY(grid);
        const auto tailsBefore = grid->findChildren<QComboBox *>();
        const auto checksBefore = grid->findChildren<QCheckBox *>();
        const auto delaysBefore = grid->findChildren<QSpinBox *>();
        QCOMPARE(tailsBefore.size(), 3);
        QCOMPARE(checksBefore.size(), 6);
        QCOMPARE(delaysBefore.size(), 3);

        QSignalSpy broadcast(CommandManager::instance(), &CommandManager::setsChanged);

        // 逐个编辑三类控件，覆盖三条不同的信号路径：toggled / valueChanged / activated 之外
        // 的 index 变更。全部勾选框都点一遍：这样"哪一行"就不必推断，断言可以按整体写。
        for (auto *box : checksBefore) box->click();
        for (auto *spin : delaysBefore) spin->setValue(123);
        for (auto *combo : tailsBefore) combo->setCurrentIndex(3);
        QCoreApplication::processEvents();

        // ① 行内控件必须还是原来那些对象——重建过一次就说明"编辑 → 落库 → 重建"这条
        //    use-after-free 链条又回来了。
        QCOMPARE(grid->findChildren<QComboBox *>(), tailsBefore);
        QCOMPARE(grid->findChildren<QCheckBox *>(), checksBefore);
        QCOMPARE(grid->findChildren<QSpinBox *>(), delaysBefore);

        // ② 编辑必须落到模型（"静默"不等于"不生效"）。默认 enabled=true / isHex=false，
        //    全部点一遍之后每一行都该反过来。
        const CommandSet stored = table->commandSet();
        QCOMPARE(stored.commands.size(), 3);
        for (const auto &command : stored.commands) {
            QCOMPARE(command.enabled, false);
            QCOMPARE(command.isHex, true);
            QCOMPARE(command.delayMs, 123);
            QCOMPARE(command.tail, QStringLiteral("CR+LF"));
        }

        // ③ 整段编辑不得广播一次 setsChanged——正是它把表格重建推给了"正在发信号的控件"。
        QCOMPARE(broadcast.count(), 0);
    }

    void renamingACommandSetKeepsItsContents()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 800);
        window.show();
        QTest::qWait(30);

        auto *panel = window.findChild<CommandPanel *>();
        auto *selector = panel ? panel->findChild<QComboBox *>() : nullptr;
        auto *rename = panel ? panel->findChild<QPushButton *>(QStringLiteral("renameCommandSet"))
                             : nullptr;
        QVERIFY(panel && selector && rename);
        QCOMPARE(rename->text(), QStringLiteral("重命名"));

        const QUuid setId = selector->currentData().toUuid();
        QVERIFY(!setId.isNull());
        CommandSet before = CommandManager::instance()->getSet(setId);
        before.name = QStringLiteral("原分组名称");
        before.notes = QStringLiteral("保留的备注");
        CommandItem command;
        command.content = QStringLiteral("AT+RST");
        command.description = QStringLiteral("重启");
        before.commands = {command};
        CommandManager::instance()->updateSet(setId, before);
        QTRY_COMPARE(selector->currentText(), before.name);

        const QString renamed = QStringLiteral("生产线 A");
        bool dialogAccepted = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            dialog->setTextValue(renamed);
            dialog->accept();
            dialogAccepted = true;
        });
        rename->click();
        QVERIFY(dialogAccepted);

        const CommandSet after = CommandManager::instance()->getSet(setId);
        QCOMPARE(after.id, before.id);
        QCOMPARE(after.name, renamed);
        QCOMPARE(after.notes, before.notes);
        QCOMPARE(after.commands.size(), 1);
        QCOMPARE(after.commands.first().content, command.content);
        QCOMPARE(after.commands.first().description, command.description);
        QCOMPARE(selector->currentText(), renamed);
    }

    // ★ 快捷指令的发送必须真的到达运行时。
    //
    // 早先 MainWindow::handleCommandSend 调的是 EndpointRuntime 的 "write" 槽，
    // 而运行时**根本没有这个槽**——invokeMethod 找不到方法就什么也不做，
    // 于是"快捷面板点了没反应"，且没有任何报错（静默失败）。
    // 顺带钉住两件同源的事：目标取发送区当前选中（指令里不存目标）、
    // 结尾符在**解析之后**按字节追加（否则 HEX 指令的结尾符会被 fromHex 静默吃掉）。
    void quickPanelSendReachesTheRuntime()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 800);
        window.show();
        QTest::qWait(30);

        auto *table = window.findChild<CommandTableWidget *>();
        auto *model = window.findChild<TrafficModel *>();
        auto *sendA = window.findChild<QPushButton *>(QStringLiteral("sendTargetA"));
        auto *sendB = window.findChild<QPushButton *>(QStringLiteral("sendTargetB"));
        QVERIFY(table && model && sendA && sendB);
        QUdpSocket receiver;
        QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
        auto *endpoint = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(endpoint);
        PortDraft udp;
        udp.kind = PortDraft::Udp;
        udp.values = {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
                      {"udp.target", "127.0.0.1"}, {"udp.targetPort", receiver.localPort()}};
        endpoint->setDraft(udp);
        auto *open = endpoint->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(open);
        open->click();
        QTRY_COMPARE(open->text(), QStringLiteral("解  绑"));

        CommandSet set;
        CommandItem text;
        text.content = QStringLiteral("+CR,GO;");
        text.description = QStringLiteral("文本指令");
        text.delayMs = 0; // 用例不必等延时
        text.tail = QStringLiteral("CR+LF");
        CommandItem hex;
        hex.content = QStringLiteral("AA 55");
        hex.description = QStringLiteral("HEX指令");
        hex.delayMs = 0;
        hex.isHex = true;
        hex.tail = QStringLiteral("CR+LF");
        set.commands << text << hex;
        table->setCommandSet(set);
        QTRY_COMPARE(table->rowCount(), 2);
        QTest::qWait(20);

        // 行内发送按钮：靠 "row" 属性认出来（底栏那几个按钮没有这个属性）。
        QList<QPushButton *> rowSends;
        for (auto *button : table->findChildren<QPushButton *>())
            if (button->property("row").isValid()) rowSends.append(button);
        QCOMPARE(rowSends.size(), 2);

        // 发送区目标是 A、B 两个独立开关，当前选中的就是唯一目标来源。
        // 默认只勾着 A；先勾上 B 再取消 A，只留 B，验证指令发去了发送区选的那个口。
        if (!sendB->isChecked()) sendB->click();
        if (sendA->isChecked()) sendA->click();
        QVERIFY(!sendA->isChecked() && sendB->isChecked());

        int rowsSeen = model->rowCount();
        rowSends.at(0)->click(); // 文本指令 → B
        QTRY_VERIFY(receiver.hasPendingDatagrams());
        QByteArray received(int(receiver.pendingDatagramSize()), '\0');
        receiver.readDatagram(received.data(), received.size());
        QCOMPARE(received, QByteArray("+CR,GO;\r\n"));
        QTRY_VERIFY(model->rowCount() > rowsSeen);
        auto sent = model->record(model->rowCount() - 1);
        QCOMPARE(sent.direction, Traffic::Direction::Transmit);
        QCOMPARE(sent.source.endpointIndex, 1); // 发去了 B，不是写死的 A
        QCOMPARE(sent.raw, QByteArray("+CR,GO;\r\n")); // 结尾符必须真的在末尾

        rowsSeen = model->rowCount();
        rowSends.at(1)->click(); // HEX 指令
        QTRY_VERIFY(receiver.hasPendingDatagrams());
        received.resize(int(receiver.pendingDatagramSize()));
        receiver.readDatagram(received.data(), received.size());
        QCOMPARE(received, QByteArray::fromHex("AA550D0A"));
        QTRY_VERIFY(model->rowCount() > rowsSeen);
        sent = model->record(model->rowCount() - 1);
        QCOMPARE(sent.direction, Traffic::Direction::Transmit);
        // HEX 内容 + 结尾符字节：AA 55 0D 0A。若把结尾符当文本拼进内容再交给
        // fromHex，那两个控制字符不是十六进制数字会被跳过，这条断言就会失败。
        QCOMPARE(sent.raw, QByteArray::fromHex("AA550D0A"));
    }

    // ★ 源码里引用的每一个 :/icons/... 都必须真的在资源里。
    //
    // 这个类别的错**不报错、不崩溃**，只是图标变成空白或纯色块——本次的
    // `image: url(:/icons/check-white.svg)` 与 `chevron-double-*.svg` 就是这样：
    // 文件躺在 resources/icons/ 里，但没登记进 resources.qrc，于是
    // ① QSS 的 image 静默失败 → 勾选态只剩一块蓝底、看不到白勾；
    // ② 图标引擎只留下一条 QWARN，跑测试时混在几十行警告里没人看。
    //
    // 判据取"源码里出现过这个路径"而不是手抄一份清单：手抄的清单会和源码一起过期，
    // 那正是 bug 的成因。扫描根目录由 __FILE__ 推出（编译期绝对路径）。
    void everyReferencedIconIsBundled()
    {
        // 项目根从 exe 所在目录逐级向上找：同时含 `src/ui/ui.pri` 与 `resources/resources.qrc`
        // 的那一级就是根。**不靠 __FILE__、也不靠工作目录**——编译器给的 __FILE__ 是相对
        // 路径，而相对谁取决于构建方式，配出来的绝对路径会指到 build/ 而不是项目根。
        QDir probe(QCoreApplication::applicationDirPath());
        QString rootPath;
        while (!probe.isRoot()) {
            if (probe.exists(QStringLiteral("src/ui/ui.pri"))
                && probe.exists(QStringLiteral("resources/resources.qrc"))) {
                rootPath = probe.absolutePath();
                break;
            }
            probe.cdUp();
        }
        QVERIFY2(!rootPath.isEmpty(),
            qPrintable(QStringLiteral("从 %1 向上找不到项目根").arg(QCoreApplication::applicationDirPath())));
        const QDir root(rootPath);

        // 只扫代码与样式表——文档里提到图标名不代表运行时要加载它。
        const QStringList filters{"*.cpp", "*.h", "*.qss"};
        // 必须带扩展名：不加这一条，本文件里那行模式字符串自身的字面量 ":/icons/"
        // 会被当成一个图标引用（`...` 被 [A-Za-z0-9_.-]+ 吃掉），报出一堆假缺失。
        // 真实引用一律带 .svg / .xpm / .png。
        static const QRegularExpression pattern(
            QStringLiteral(":/icons/[A-Za-z0-9_-]+\\.[A-Za-z0-9]+"));
        QSet<QString> referenced;
        QDirIterator it(rootPath, filters, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            // 构建产物（build*/）里是生成物与拷贝件，不是"源码引用了什么"的依据。
            // Windows 下 QDir 遍历给原生反斜杠，两种分隔符都要排除——
            // 只查 "/build" 的话，build/ 下生成物里的旧图标引用会误报"被源码引用"。
            if (QDir::fromNativeSeparators(path).contains(QStringLiteral("/build"))) continue;
            // rcc 生成物（qrc_*.cpp）内嵌的是打包时刻的资源引用，不是"源码引用了什么"；
            // 源码树里早期 qmake 散落的 release/ 目录因此被扫到过，统一跳过生成物。
            if (QFileInfo(path).fileName().startsWith(QStringLiteral("qrc_"))) continue;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            const QString text = QString::fromUtf8(file.readAll());
            auto match = pattern.globalMatch(text);
            while (match.hasNext()) referenced.insert(match.next().captured());
        }
        QVERIFY2(referenced.size() > 10,
            qPrintable(QStringLiteral("在 %1 下只扫到 %2 个图标引用，扫描范围大概不对")
                           .arg(rootPath).arg(referenced.size())));

        QStringList missing;
        for (const auto &path : referenced) {
            QFile icon(path);
            if (!icon.exists()) missing << path;
        }
        missing.sort();
        QVERIFY2(missing.isEmpty(),
            qPrintable(QStringLiteral("这些图标被源码引用但不在资源里（补进 resources.qrc）：\n  ")
                + missing.join(QStringLiteral("\n  "))));
    }

    // ★ 批量发送要有进度标识（用户钦定）：正在发的那一条勾选框转琥珀，
    // 这一轮发过的留绿色，一眼看出"发到哪条了"。
    void batchSendMarksProgressOnRows()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 800);
        window.show();
        QTest::qWait(30);

        auto *table = window.findChild<CommandTableWidget *>();
        // 批量发送按钮的 objectName 是 primaryButton（QSS 按它画强调底，见实现里的说明）。
        auto *batch = window.findChild<QPushButton *>(QStringLiteral("primaryButton"));
        QVERIFY(table && batch);

        CommandSet set;
        for (int i = 0; i < 3; ++i) {
            CommandItem item;
            item.content = QStringLiteral("+CR,%1;").arg(i);
            item.description = QStringLiteral("指令%1").arg(i);
            item.delayMs = 0; // 用例不等延时，只验进度标识
            set.commands.append(item);
        }
        table->setCommandSet(set);
        QTRY_COMPARE(table->rowCount(), 3);
        QTest::qWait(20);

        // 起点干净：没有任何行带着发送痕迹。
        for (int row = 0; row < 3; ++row) QVERIFY(table->sendStateOf(row).isEmpty());

        // 光验属性还不够——属性对了、QSS 没覆盖到（选择器写错、被更具体的规则压掉、
        // 动态属性改了但没重新 polish）都是一样的白干，而屏幕上就是没变色。
        // 所以这里**真的把它画出来看**：临时装上应用样式表，取勾选框指示器的像素。
        // （测试进程默认不带样式表，因为 QSS 是 main.cpp 装的。）
        QFile qss(QStringLiteral(":/styles/workbench.qss"));
        QVERIFY(qss.open(QIODevice::ReadOnly));
        const QString previousStyle = qApp->styleSheet();
        qApp->setStyleSheet(QString::fromUtf8(qss.readAll()));

        // 指示器在勾选框左侧 16×16；按"这个区域里最饱和的那个像素"取色，
        // 比盯死某个坐标稳（不同 DPI 下居中留白会变）。
        const auto indicatorColor = [&table](int row) {
            const QString state = table->sendStateOf(row);
            if (state.isEmpty()) return QColor();
            // 走公开接口拿不到控件，按"动态属性值对得上"定位（每行至多一个带标记的勾选框）。
            QCheckBox *target = nullptr;
            for (auto *box : table->findChildren<QCheckBox *>())
                if (box->property("sendState").toString() == state) { target = box; break; }
            if (!target) return QColor();
            const QImage shot = target->grab().toImage();
            QColor best;
            int bestScore = -1;
            for (int y = 0; y < qMin(18, shot.height()); ++y) {
                for (int x = 0; x < qMin(18, shot.width()); ++x) {
                    const QColor c = shot.pixelColor(x, y);
                    const int score = qMax(qMax(c.red(), c.green()), c.blue())
                        - qMin(qMin(c.red(), c.green()), c.blue());
                    if (score > bestScore) { bestScore = score; best = c; }
                }
            }
            return best;
        };

        table->beginSendRun();
        table->setSendCursor(0);
        QTest::qWait(20);
        const QColor sending = indicatorColor(0);
        table->setSendCursor(1); // 0 号转为"已发送"
        QTest::qWait(20);
        const QColor sent = indicatorColor(0);
        table->clearSendRun();
        QTest::qWait(20);

        qApp->setStyleSheet(previousStyle); // 复原，别影响其他用例

        // 琥珀（正在发）：红高、绿中、蓝很低。绿（已发）：绿最高、红最低。
        QVERIFY2(sending.isValid() && sending.red() > 190 && sending.green() > 110
                && sending.green() < 210 && sending.blue() < 90,
            qPrintable(QStringLiteral("正在发送的勾选框应为琥珀色，实测 rgb(%1,%2,%3)")
                           .arg(sending.red()).arg(sending.green()).arg(sending.blue())));
        QVERIFY2(sent.isValid() && sent.green() > 110 && sent.red() < 90,
            qPrintable(QStringLiteral("已发送的勾选框应为绿色，实测 rgb(%1,%2,%3)")
                           .arg(sent.red()).arg(sent.green()).arg(sent.blue())));

        // 游标语义单独验一遍（批量发送跑得快，中间态不适合抢时间窗去抓）。
        table->beginSendRun();
        table->setSendCursor(1);
        QCOMPARE(table->sendStateOf(1), QStringLiteral("sending"));
        QVERIFY(table->sendStateOf(0).isEmpty());
        table->setSendCursor(2); // 游标前移：1 变成"已发送"
        QCOMPARE(table->sendStateOf(1), QStringLiteral("sent"));
        QCOMPARE(table->sendStateOf(2), QStringLiteral("sending"));
        table->endSendRun();
        QCOMPARE(table->sendStateOf(2), QStringLiteral("sent"));
        QVERIFY(table->sendStateOf(0).isEmpty());
        table->clearSendRun();
        for (int row = 0; row < 3; ++row) QVERIFY(table->sendStateOf(row).isEmpty());

        // 端到端：勾选 → 点「按序发送勾选项」→ 三条最终都留下"已发送"标记。
        // 整条链（表格取行号 → 面板排时序 → 回写进度 → 收尾）一次跑通。
        //
        // 等待要盯**收尾完成**这个条件本身（最后一行由 sending 转 sent），不能只等
        // 第一行变 sent 就往下断言：游标是逐条推进的，第一行早早就是 sent 了，
        // 而最后一行还停在 sending —— 那时断言等于在抢时间窗。
        for (auto *box : table->findChildren<QCheckBox *>()) {
            if (box->objectName() == QStringLiteral("loopBatchSend")) continue;
            box->setChecked(true);
        }
        batch->click();
        QTRY_COMPARE_WITH_TIMEOUT(table->sendStateOf(2), QStringLiteral("sent"), 3000);
        QCOMPARE(table->sendStateOf(0), QStringLiteral("sent"));
        QCOMPARE(table->sendStateOf(1), QStringLiteral("sent"));

        // 重建表格必须把痕迹清干净：行号会被重排，留着高亮只会指到别的指令上。
        table->setCommandSet(set);
        for (int row = 0; row < 3; ++row) QVERIFY(table->sendStateOf(row).isEmpty());

        // 「循环发送」在按钮左侧。勾上后同一批指令发完再来一轮，按钮变成「停止循环」。
        auto *loop = table->findChild<QCheckBox *>(QStringLiteral("loopBatchSend"));
        QVERIFY(loop);
        QVERIFY2(loop->mapTo(table, QPoint()).x() < batch->mapTo(table, QPoint()).x(),
            "循环发送应在按序发送按钮的左侧");
        QVERIFY(!loop->isChecked());
        loop->setChecked(true);
        // 每条留出可见延时，游标才会在第一行停住，第二轮回来时才能被断言抓住。
        for (auto *spin : table->findChildren<QSpinBox *>()) spin->setValue(80);
        for (auto *box : table->findChildren<QCheckBox *>()) {
            if (box == loop) continue;
            box->setChecked(true);
        }
        batch->click();
        QTRY_COMPARE(batch->text(), QStringLiteral("停止循环"));
        QTRY_COMPARE_WITH_TIMEOUT(table->sendStateOf(0), QStringLiteral("sending"), 2000);
        // 第一轮走完后游标离开第一行。再等它回到第一行，才是第二轮。
        QTRY_VERIFY_WITH_TIMEOUT(table->sendStateOf(0) != QStringLiteral("sending"), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(table->sendStateOf(0), QStringLiteral("sending"), 2000);
        batch->click();
        QTRY_COMPARE(batch->text(), QStringLiteral("按序发送勾选项"));
        // 停之后不再开下一轮，第一行不会又被点成正在发送。
        QTest::qWait(400);
        QVERIFY(table->sendStateOf(0) != QStringLiteral("sending"));
        loop->setChecked(false);
    }

    // 快捷面板顶栏的「换行」：关着时发送框单行（长指令不把行撑高），
    // 开着时按列宽折行，长指令那一行明显高于短指令。选择要记住。
    void commandContentWrapToggle()
    {
        CommandPanel panel;
        panel.setAttribute(Qt::WA_DontShowOnScreen);
        panel.setExpanded(true);
        panel.resize(760, 520);
        panel.show();
        QTest::qWait(30);

        auto *wrap = panel.findChild<QPushButton *>(QStringLiteral("commandContentWrap"));
        auto *table = panel.findChild<CommandTableWidget *>();
        QVERIFY(wrap && table);
        QVERIFY(wrap->isCheckable());
        QVERIFY(!wrap->isChecked());
        QCOMPARE(wrap->text(), QStringLiteral("换行"));
        QVERIFY(!wrap->icon().isNull());
        QVERIFY(!table->contentWordWrap());
        QVERIFY2(wrap->mapTo(&panel, QPoint()).y() < table->mapTo(&panel, QPoint()).y(),
            "换行按钮应在指令表上方");

        CommandSet set;
        set.name = QStringLiteral("折行");
        CommandItem shortCmd;
        shortCmd.content = QStringLiteral("AT");
        shortCmd.description = QStringLiteral("短");
        CommandItem longCmd;
        longCmd.content = QString(200, QLatin1Char('W'));
        longCmd.description = QStringLiteral("长");
        set.commands = {shortCmd, longCmd};
        table->setCommandSet(set);
        QTest::qWait(30);

        auto *grid = table->findChild<QTableWidget *>();
        QVERIFY(grid);
        const int shortOff = grid->rowHeight(0);
        const int longOff = grid->rowHeight(1);
        QVERIFY2(qAbs(shortOff - longOff) <= 6,
            qPrintable(QStringLiteral("不换行时两行应同高：短 %1 长 %2").arg(shortOff).arg(longOff)));
        QCOMPARE(grid->item(1, 2)->toolTip(), longCmd.content);

        wrap->click();
        QVERIFY(wrap->isChecked());
        QVERIFY(table->contentWordWrap());
        QTest::qWait(30);
        QVERIFY2(grid->rowHeight(1) > grid->rowHeight(0) + 8,
            qPrintable(QStringLiteral("换行后长指令应更高：短 %1 长 %2")
                           .arg(grid->rowHeight(0)).arg(grid->rowHeight(1))));
        // 折行只用在绘制上，单元格里存的仍是原文，不能混进零宽字符。
        QCOMPARE(table->commandSet().commands.at(1).content, longCmd.content);

        wrap->click();
        QVERIFY(!table->contentWordWrap());
        QTest::qWait(30);
        QVERIFY2(qAbs(grid->rowHeight(0) - grid->rowHeight(1)) <= 6,
            qPrintable(QStringLiteral("关掉换行后两行应回到同高：短 %1 长 %2")
                           .arg(grid->rowHeight(0)).arg(grid->rowHeight(1))));

        // 选择落盘。用单独的 ini，别碰测试进程共用的那份；指令集单例先备份再还回去。
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PreferencesStore backup(nullptr, dir.filePath(QStringLiteral("backup.ini")));
        CommandManager::instance()->save(&backup);
        {
            PreferencesStore store(nullptr, dir.filePath(QStringLiteral("wrap.ini")));
            CommandPanel remembered;
            remembered.setPreferencesStore(&store);
            auto *againWrap = remembered.findChild<QPushButton *>(QStringLiteral("commandContentWrap"));
            QVERIFY(againWrap);
            QVERIFY(!againWrap->isChecked());
            againWrap->click();
            QCOMPARE(store.value(QStringLiteral("commandPanel/contentWrap")).toBool(), true);

            CommandPanel restored;
            restored.setPreferencesStore(&store);
            auto *restoredWrap = restored.findChild<QPushButton *>(QStringLiteral("commandContentWrap"));
            auto *restoredTable = restored.findChild<CommandTableWidget *>();
            QVERIFY(restoredWrap && restoredTable);
            QVERIFY(restoredWrap->isChecked());
            QVERIFY(restoredTable->contentWordWrap());
        }
        CommandManager::instance()->load(&backup);
    }

    // 行内发送按钮要铺满发送列。居中时按钮停在文字宽度上，点文字两侧点不中。
    void commandSendButtonsFillTheirColumn()
    {
        CommandPanel panel;
        panel.setAttribute(Qt::WA_DontShowOnScreen);
        panel.setExpanded(true);
        panel.resize(760, 520);
        panel.show();
        QTest::qWait(30);

        auto *table = panel.findChild<CommandTableWidget *>();
        QVERIFY(table);
        CommandSet set;
        set.name = QStringLiteral("发送列");
        CommandItem shortCmd;
        shortCmd.content = QStringLiteral("AT");
        shortCmd.description = QStringLiteral("发送");
        CommandItem longCmd;
        longCmd.content = QStringLiteral("AT+REBOOT");
        longCmd.description = QStringLiteral("重启");
        set.commands = {shortCmd, longCmd};
        table->setCommandSet(set);
        QTest::qWait(30);

        auto *grid = table->findChild<QTableWidget *>();
        QVERIFY(grid);
        const int column = 6; // 发送列，见 CommandTableWidget 的列序
        QList<QPushButton *> buttons;
        for (auto *button : table->findChildren<QPushButton *>()) {
            if (button->property("row").isValid()) buttons.append(button);
        }
        QCOMPARE(buttons.size(), 2);
        for (auto *button : buttons) {
            const int row = button->property("row").toInt();
            const QRect cell = grid->visualRect(grid->model()->index(row, column));
            QVERIFY2(cell.width() > 40, qPrintable(QStringLiteral("发送列过窄：%1").arg(cell.width())));
            // 左右各留 2px 内边距，按钮应贴到列的可用宽度。
            QVERIFY2(button->width() >= cell.width() - 8,
                qPrintable(QStringLiteral("发送按钮未铺满列：按钮 %1，列 %2")
                    .arg(button->width()).arg(cell.width())));
        }
    }

    void commandRowEditsSurviveStructuralChanges_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::newRow("copy") << QStringLiteral("复制该指令");
        QTest::newRow("delete") << QStringLiteral("删除该指令");
        QTest::newRow("drag") << QStringLiteral("drag");
    }

    void commandRowEditsSurviveStructuralChanges()
    {
        QFETCH(QString, operation);
        CommandTableWidget table;
        table.setAttribute(Qt::WA_DontShowOnScreen);
        table.resize(850, 400);
        CommandSet set("edits");
        for (int i = 0; i < 3; ++i) {
            CommandItem command;
            command.content = QString::number(i);
            command.packMode = QStringLiteral("组包");
            set.commands.append(command);
        }
        table.setCommandSet(set);
        table.show();
        auto *grid = table.findChild<QTableWidget *>();
        QVERIFY(grid);
        QTest::qWait(20);
        grid->item(0, 2)->setText(QStringLiteral("EDITED"));
        grid->cellWidget(0, 0)->findChild<QCheckBox *>()->setChecked(false);
        grid->cellWidget(0, 3)->findChild<QCheckBox *>()->setChecked(true);
        grid->cellWidget(0, 4)->findChild<QSpinBox *>()->setValue(321);
        auto *tail = grid->cellWidget(0, 5)->findChild<QComboBox *>();
        QCOMPARE(tail->itemText(1), QStringLiteral("+\\r"));
        QCOMPARE(tail->itemText(2), QStringLiteral("+\\n"));
        QCOMPARE(tail->itemText(3), QStringLiteral("+\\r\\n"));
        tail->setCurrentIndex(3);
        auto expected = set.commands.first();
        expected.content = QStringLiteral("EDITED");
        expected.enabled = false;
        expected.isHex = true;
        expected.delayMs = 321;
        expected.tail = QStringLiteral("CR+LF");
        QCOMPARE(table.commandSet().commands.first(), expected);
        if (operation == "drag") {
            const auto handle = grid->visualRect(grid->model()->index(0, 7)).center();
            const auto target = grid->visualRect(grid->model()->index(2, 7));
            const QPoint drop(target.center().x(), target.bottom() - 1);
            QTest::mousePress(grid->viewport(), Qt::LeftButton, Qt::NoModifier, handle);
            QTest::mouseMove(grid->viewport(), drop);
            QTest::mouseRelease(grid->viewport(), Qt::LeftButton, Qt::NoModifier, drop);
            QTRY_COMPARE(table.commandSet().commands.last(), expected);
        } else {
            bool invoked = false;
            QTimer::singleShot(0, &table, [&] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) return;
                for (auto *action : menu->actions()) {
                    if (action->text() == operation) { action->trigger(); invoked = true; break; }
                }
                menu->close();
            });
            const int row = operation == QStringLiteral("删除该指令") ? 1 : 0;
            grid->customContextMenuRequested(grid->visualItemRect(grid->item(row, 2)).center());
            QVERIFY(invoked);
            QCOMPARE(table.commandSet().commands.first(), expected);
            if (operation == QStringLiteral("复制该指令")) {
                auto copy = table.commandSet().commands.at(1);
                QVERIFY(copy.id != expected.id);
                copy.id = expected.id;
                QCOMPARE(copy, expected);
            }
        }
        QCOMPARE(table.commandSet().commands.size(), operation == QStringLiteral("复制该指令") ? 4
                 : operation == QStringLiteral("删除该指令") ? 2 : 3);
    }

    void commandDeferredActionsCannotModifyAnotherGroup()
    {
        CommandTableWidget table;
        table.resize(850, 400);
        table.setAttribute(Qt::WA_DontShowOnScreen);
        CommandSet first("first");
        CommandItem item;
        item.content = "FIRST";
        first.commands = {item, CommandItem()};
        CommandSet second("second");
        item.id = QUuid::createUuid();
        item.content = "SECOND";
        second.commands = {item};
        table.setCommandSet(first);
        table.show();
        auto *grid = table.findChild<QTableWidget *>();
        QTest::qWait(20);
        const auto handle = grid->visualRect(grid->model()->index(0, 7)).center();
        const auto last = grid->visualRect(grid->model()->index(1, 7));
        const QPoint drop(last.center().x(), last.bottom() - 1);
        QTest::mousePress(grid->viewport(), Qt::LeftButton, Qt::NoModifier, handle);
        QTest::mouseRelease(grid->viewport(), Qt::LeftButton, Qt::NoModifier, drop);
        table.setCommandSet(second);
        QTest::qWait(20);
        QCOMPARE(table.commandSet().toJson(), second.toJson());
        table.setCommandSet(first);
        bool invoked = false;
        QTimer::singleShot(0, &table, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            table.setCommandSet(second);
            for (auto *action : menu->actions()) {
                if (action->text() == QStringLiteral("删除该指令")) {
                    action->trigger();
                    invoked = true;
                    break;
                }
            }
            menu->close();
        });
        grid->customContextMenuRequested(grid->visualItemRect(grid->item(0, 2)).center());
        QVERIFY(invoked);
        QCOMPARE(table.commandSet().toJson(), second.toJson());

        // 底栏移动菜单也不能在切组后使用旧菜单移动另一组的勾选项。
        table.setCommandSet(first);
        table.setMoveTargetSets({first, second});
        auto *move = table.findChild<QPushButton *>(QStringLiteral("moveCheckedCommands"));
        QVERIFY(move && move->menu());
        QSignalSpy requests(&table, &CommandTableWidget::moveCommandsRequested);
        move->menu()->popup(move->mapToGlobal(QPoint(0, move->height())));
        QVERIFY(!move->menu()->actions().isEmpty());
        auto *staleAction = move->menu()->actions().first();
        table.setCommandSet(second);
        staleAction->trigger();
        move->menu()->close();
        QCOMPARE(requests.count(), 0);
        QCOMPARE(table.commandSet().toJson(), second.toJson());
    }

    void commandMetadataChangesKeepEditorsAndSelection()
    {
        QTemporaryDir directory;
        auto *manager = CommandManager::instance();
        PreferencesStore backup(nullptr, directory.filePath("backup.ini"));
        manager->save(&backup);
        const auto restore = qScopeGuard([&] { manager->load(&backup); });
        PreferencesStore store(nullptr, directory.filePath("metadata.ini"));
        CommandPanel panel;
        panel.setPreferencesStore(&store);
        panel.setExpanded(true);
        panel.setAttribute(Qt::WA_DontShowOnScreen);
        panel.show();
        auto source = manager->allSets().first();
        CommandItem command;
        command.content = "CURRENT";
        command.delayMs = 500;
        source.commands = {command};
        manager->updateSet(source.id, source);
        auto *table = panel.findChild<CommandTableWidget *>();
        auto *grid = table->findChild<QTableWidget *>();
        auto *check = grid->cellWidget(0, 0);
        grid->selectRow(0);
        grid->editItem(grid->item(0, 2));
        QVERIFY(table->hasActiveEditor());
        CommandSet other("other");
        manager->addSet(other);
        source.name = "renamed";
        manager->updateSet(source.id, source);
        QCOMPARE(grid->cellWidget(0, 0), check);
        QCOMPARE(grid->selectionModel()->selectedRows().size(), 1);
        QVERIFY(table->hasActiveEditor());
        QCOMPARE(table->commandSet().name, source.name);
        auto *batch = table->findChild<QPushButton *>(QStringLiteral("primaryButton"));
        batch->click();
        QVERIFY(panel.hasPendingSends());
        auto *selector = panel.findChild<QComboBox *>(QStringLiteral("commandSetSelector"));
        selector->setCurrentIndex(selector->findData(other.id.toString()));
        QVERIFY(!panel.hasPendingSends());
    }

    void movingCheckedCommandsPreservesDataAndPersists()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto *manager = CommandManager::instance();
        PreferencesStore backup(nullptr, dir.filePath(QStringLiteral("backup.ini")));
        manager->save(&backup);
        const auto restore = qScopeGuard([&] { manager->load(&backup); });
        PreferencesStore store(nullptr, dir.filePath(QStringLiteral("move.ini")));
        CommandPanel panel;
        panel.setPreferencesStore(&store);
        panel.setExpanded(true);
        panel.resize(900, 500);
        panel.setAttribute(Qt::WA_DontShowOnScreen);
        panel.show();
        auto *table = panel.findChild<CommandTableWidget *>();
        auto *selector = panel.findChild<QComboBox *>(QStringLiteral("commandSetSelector"));
        auto *move = panel.findChild<QPushButton *>(QStringLiteral("moveCheckedCommands"));
        QVERIFY(table && selector && move && move->menu());
        CommandSet source = manager->getSet(selector->currentData().toUuid());
        source.notes = QStringLiteral("保留来源备注");
        for (int i = 0; i < 4; ++i) {
            CommandItem command;
            command.content = QStringLiteral("AA 0%1").arg(i);
            command.description = QStringLiteral("指令%1").arg(i);
            command.isHex = true;
            command.tail = QStringLiteral("CR+LF");
            command.packMode = QStringLiteral("组包");
            command.delayMs = 80;
            command.enabled = false;
            source.commands.append(command);
        }
        manager->updateSet(source.id, source);
        QVERIFY(!move->isEnabled()); // 只有一组。
        CommandSet target(QStringLiteral("目标组"));
        target.notes = QStringLiteral("保留目标备注");
        CommandItem existing;
        existing.content = QStringLiteral("EXISTING");
        target.commands = {existing};
        manager->addSet(target);
        QVERIFY(!move->isEnabled()); // 没有勾选。
        target.name = QStringLiteral("目标 & 分组");
        manager->updateSet(target.id, target);
        auto *grid = table->findChild<QTableWidget *>();
        QVERIFY(grid);
        grid->cellWidget(0, 0)->findChild<QCheckBox *>()->setChecked(true);
        grid->cellWidget(2, 0)->findChild<QCheckBox *>()->setChecked(true);
        grid->item(0, 2)->setText(QStringLiteral("AA FF")); // 移动最新编辑值。
        grid->cellWidget(2, 4)->findChild<QSpinBox *>()->setValue(123);
        grid->selectRow(1); // 行选中不影响「移动勾选」。
        const auto before = table->commandSet();
        QVERIFY(move->isEnabled());
        panel.resize(panel.expandedMinimumWidth(), 500);
        QTest::qWait(20);
        const auto artifactDir = qEnvironmentVariable("QQCOM_UI_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QVERIFY(QDir().mkpath(artifactDir));
            QVERIFY(panel.grab().save(artifactDir + QStringLiteral("/command-move-panel.png")));
        }

        QSignalSpy sent(&panel, &CommandPanel::sendCommand);
        auto *batch = table->findChild<QPushButton *>(QStringLiteral("primaryButton"));
        QVERIFY(batch);
        batch->click();
        QVERIFY(panel.hasPendingSends());
        QSignalSpy broadcast(manager, &CommandManager::setsChanged);
        bool selectedTarget = false;
        QStringList menuTargets;
        QTimer::singleShot(0, &panel, [&] {
            menuTargets.append(QStringLiteral("菜单已打开"));
            const auto actions = move->menu()->actions();
            for (auto *action : actions)
                menuTargets.append(action->text() + action->data().toUuid().toString());
            if (actions.size() == 1 && actions.first()->data().toUuid() == target.id
                && actions.first()->text() == QStringLiteral("目标 && 分组")) {
                selectedTarget = true;
                actions.first()->trigger();
            }
            move->menu()->close();
        });
        move->showMenu();
        QTRY_VERIFY2(selectedTarget, qPrintable(menuTargets.join(QLatin1Char('\n'))));
        QCOMPARE(broadcast.count(), 1);
        QVERIFY(!panel.hasPendingSends());
        QTest::qWait(230);
        QCOMPARE(sent.count(), 0);
        const auto remaining = manager->getSet(source.id);
        const auto moved = manager->getSet(target.id);
        QCOMPARE(remaining.notes, source.notes);
        QCOMPARE(remaining.commands.size(), 2);
        QCOMPARE(remaining.commands.at(0).toJson(), before.commands.at(1).toJson());
        QCOMPARE(remaining.commands.at(1).toJson(), before.commands.at(3).toJson());
        QCOMPARE(moved.notes, target.notes);
        QCOMPARE(moved.commands.size(), 3);
        QCOMPARE(moved.commands.at(0).toJson(), existing.toJson());
        QCOMPARE(moved.commands.at(1).toJson(), before.commands.at(0).toJson());
        QCOMPARE(moved.commands.at(2).toJson(), before.commands.at(2).toJson());
        QCOMPARE(table->commandSet().toJson(), remaining.toJson());
        QCOMPARE(selector->currentData().toUuid(), source.id);
        QVERIFY(!move->isEnabled());
        selector->setCurrentIndex(selector->findData(target.id.toString()));
        QCOMPARE(table->commandSet().toJson(), moved.toJson());
        manager->load(&store);
        QCOMPARE(manager->getSet(source.id).toJson(), remaining.toJson());
        QCOMPARE(manager->getSet(target.id).toJson(), moved.toJson());
    }

    void movingSelectedCommandsUsesRowsRatherThanChecks()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto *manager = CommandManager::instance();
        PreferencesStore backup(nullptr, dir.filePath(QStringLiteral("backup.ini")));
        manager->save(&backup);
        const auto restore = qScopeGuard([&] { manager->load(&backup); });
        PreferencesStore store(nullptr, dir.filePath(QStringLiteral("move.ini")));
        CommandPanel panel;
        panel.setPreferencesStore(&store);
        panel.setExpanded(true);
        panel.resize(900, 500);
        panel.setAttribute(Qt::WA_DontShowOnScreen);
        panel.show();
        auto *table = panel.findChild<CommandTableWidget *>();
        auto *selector = panel.findChild<QComboBox *>(QStringLiteral("commandSetSelector"));
        QVERIFY(table && selector);
        CommandSet source = manager->getSet(selector->currentData().toUuid());
        for (int i = 0; i < 4; ++i) {
            CommandItem command;
            command.content = QStringLiteral("ROW%1").arg(i);
            command.enabled = (i == 1);
            source.commands.append(command);
        }
        manager->updateSet(source.id, source);
        CommandSet target(QStringLiteral("目标组"));
        manager->addSet(target);
        // 同名分组按 UUID 区分。
        CommandSet other(target.name);
        manager->addSet(other);
        auto *grid = table->findChild<QTableWidget *>();
        QVERIFY(grid);
        QTest::qWait(20);
        const auto rowPoint = [&](int row) { return grid->visualItemRect(grid->item(row, 2)).center(); };
        QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(2));
        QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::ControlModifier, rowPoint(0));
        QCOMPARE(grid->selectionModel()->selectedRows().size(), 2);
        QSignalSpy sent(&panel, &CommandPanel::sendCommand);
        QSignalSpy requests(table, &CommandTableWidget::moveCommandsRequested);
        bool menuValid = false;
        const auto openContextMenu = [&](int row, bool choose) {
            QTimer::singleShot(0, &panel, [&, choose] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                auto *moves = menu ? menu->findChild<QMenu *>(QStringLiteral("moveSelectedCommands")) : nullptr;
                if (moves && moves->isEnabled() && moves->actions().size() == 2) {
                    for (auto *action : moves->actions()) {
                        if (action->data().toUuid() == target.id) {
                            menuValid = true;
                            if (choose) action->trigger();
                            break;
                        }
                    }
                }
                if (menu) menu->close();
            });
            grid->customContextMenuRequested(rowPoint(row));
        };
        openContextMenu(0, false); // 取消菜单不修改数据。
        QVERIFY(menuValid);
        QCOMPARE(requests.count(), 0);
        QCOMPARE(table->commandSet().toJson(), source.toJson());
        openContextMenu(0, true);
        QCOMPARE(requests.count(), 1);
        QCOMPARE(requests.first().at(0).value<QList<int>>(), QList<int>({0, 2}));
        auto moved = manager->getSet(target.id);
        QCOMPARE(moved.commands.size(), 2);
        QCOMPARE(moved.commands.at(0).toJson(), source.commands.at(0).toJson());
        QCOMPARE(moved.commands.at(1).toJson(), source.commands.at(2).toJson());
        QVERIFY(manager->getSet(other.id).commands.isEmpty());

        // 右键未选中的行时，只移动右键所在行。
        grid->selectRow(0);
        openContextMenu(1, true);
        QCOMPARE(requests.count(), 2);
        QCOMPARE(requests.last().at(0).value<QList<int>>(), QList<int>({1}));
        moved = manager->getSet(target.id);
        QCOMPARE(moved.commands.last().toJson(), source.commands.at(3).toJson());
        QCOMPARE(table->commandSet().commands.first().toJson(), source.commands.at(1).toJson());
        QCOMPARE(sent.count(), 0);

        const auto beforeInvalid = manager->getSet(source.id).toJson();
        table->moveCommandsRequested({0}, source.id);
        table->moveCommandsRequested({0}, QUuid::createUuid());
        table->moveCommandsRequested({0, 9}, target.id);
        QCOMPARE(manager->getSet(source.id).toJson(), beforeInvalid);
        QCOMPARE(manager->getSet(target.id).toJson(), moved.toJson());
        manager->load(&store);
        QCOMPARE(manager->getSet(source.id).toJson(), beforeInvalid);
        QCOMPARE(manager->getSet(target.id).toJson(), moved.toJson());
    }

    // 扩展面板的「分组」：只改下拉框就要落盘，下次启动还要回到那一组。
    // 上次若是展开着退出，恢复展开不能把分组盖回第一组。
    void commandGroupSelectionPersists()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PreferencesStore backup(nullptr, dir.filePath(QStringLiteral("backup.ini")));
        CommandManager::instance()->save(&backup);
        {
            PreferencesStore store(nullptr, dir.filePath(QStringLiteral("groups.ini")));
            CommandPanel editor;
            editor.setPreferencesStore(&store);

            CommandSet second(QStringLiteral("乙组"));
            CommandItem command;
            command.content = QStringLiteral("BBB");
            command.description = QStringLiteral("乙");
            second.commands = {command};
            CommandManager::instance()->addSet(second);

            auto *selector = editor.findChild<QComboBox *>(QStringLiteral("commandSetSelector"));
            auto *table = editor.findChild<CommandTableWidget *>();
            QVERIFY(selector && table);
            const int index = selector->findData(second.id.toString());
            QVERIFY(index > 0);
            selector->setCurrentIndex(index);
            QCOMPARE(selector->currentData().toString(), second.id.toString());
            QCOMPARE(table->commandSet().name, second.name);
            QCOMPARE(table->commandSet().commands.first().content, command.content);
            QCOMPARE(store.value(QStringLiteral("commandPanel/currentSet")).toString(),
                     second.id.toString());

            // 展开会再保存一次。这时下拉框已经在乙组，不能写回第一组。
            editor.setExpanded(true);
            QCOMPARE(store.value(QStringLiteral("commandPanel/expanded")).toBool(), true);
            QCOMPARE(store.value(QStringLiteral("commandPanel/currentSet")).toString(),
                     second.id.toString());

            CommandPanel restored;
            restored.setPreferencesStore(&store);
            auto *again = restored.findChild<QComboBox *>(QStringLiteral("commandSetSelector"));
            auto *againTable = restored.findChild<CommandTableWidget *>();
            QVERIFY(again && againTable);
            QVERIFY(restored.isExpanded());
            QCOMPARE(again->currentData().toString(), second.id.toString());
            QCOMPARE(againTable->commandSet().name, second.name);
            QCOMPARE(againTable->commandSet().commands.first().content, command.content);
            // 恢复展开本身不得把已存的分组盖掉。
            QCOMPARE(store.value(QStringLiteral("commandPanel/currentSet")).toString(),
                     second.id.toString());
        }
        CommandManager::instance()->load(&backup);
    }

    // ★ 展开/收起指令面板时，**数据区宽度原地不动、窗口宽度跟着变**（用户钦定）。
    //
    // 早先的行为是"收起就把空出来的宽度全给数据区"，于是收起那一下监视区突然变宽、
    // 展开又缩回去——折行位置与列宽跟着跳，正看着的内容会整屏重排。
    // 这条用例把"数据区宽度是锚点"钉死：两种切换下数据区都必须原地不动，
    // 而窗口宽度必须**恰好**增减面板那一段（只验"变了"是不够的）。
    void togglingPanelKeepsDataAreaWidth()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1600, 900);
        window.show();
        QTest::qWait(50);

        auto *splitter = window.findChild<QSplitter *>(QStringLiteral("horizontalSplitter"));
        auto *toggle = window.findChild<QPushButton *>(QStringLiteral("commandPanelToggle"));
        QVERIFY(splitter && toggle);
        // 没有保存过展开状态时，面板默认收起，分栏里只有左列。先展开再量基准。
        if (!toggle->isChecked()) toggle->setChecked(true);
        QTest::qWait(50);
        QCOMPARE(splitter->count(), 2);
        auto *panel = window.findChild<CommandPanel *>();
        QVERIFY(panel);

        const int expandedData = splitter->widget(0)->width();
        const int expandedPanel = splitter->widget(1)->width();
        const int expandedWindow = window.width();
        qInfo() << "EXPANDED data" << expandedData << "panel" << expandedPanel
                << "window" << expandedWindow << "splitter" << splitter->width()
                << "handle" << splitter->handleWidth();
        QVERIFY2(expandedData > 0 && expandedPanel > 0,
            qPrintable(QStringLiteral("展开态几何不对：数据区 %1 面板 %2")
                           .arg(expandedData).arg(expandedPanel)));

        // ★ 端口条必须**止步于分栏线**，不许横贯到指令面板下方。
        //
        // 用户反馈："点击扩展时端口设置区域跟着拉长"。根因是端口条原本挂在**根布局**里
        // 横贯全宽：面板展开把窗口加宽，横贯的端口条就跟着被拉长了。端口条改为左侧列的
        // 成员之后，它的右边缘应当不超过面板的左边缘——这条断言直接编码用户的诉求
        //（"让快捷面板占据整个右侧"），比只验宽度更有意义。
        auto *endpointBar = window.findChild<QFrame *>(QStringLiteral("EndpointBar"));
        QVERIFY(endpointBar);
        const int expandedBarWidth = endpointBar->width();
        const int barRight = endpointBar->mapTo(&window, QPoint()).x() + expandedBarWidth;
        const int panelLeft = splitter->widget(1)->mapTo(&window, QPoint()).x();
        QVERIFY2(barRight <= panelLeft,
            qPrintable(QStringLiteral("端口条越过了分栏线（应止步于指令面板左侧）："
                                      "右缘 %1 vs 面板左缘 %2")
                           .arg(barRight).arg(panelLeft)));

        // 收起：数据区原地不动，面板归零，窗口宽度恰好减去面板那一段。
        toggle->setChecked(false);
        QTest::qWait(50);
        const int collapsedData = splitter->widget(0)->width();
        const int collapsedWindow = window.width();
        qInfo() << "COLLAPSED data" << collapsedData << "window" << collapsedWindow
                << "splitter" << splitter->width() << "shrunk" << (expandedWindow - collapsedWindow);
        // 数据区是锚点：两种状态下的宽度必须**逐像素相等**。
        QCOMPARE(collapsedData, expandedData);
        QCOMPARE(splitter->indexOf(panel), -1);
        QCOMPARE(splitter->count(), 1);
        // 扩展区移出分栏后，中间那条拖动条不存在，分栏宽度全部给左列。
        QVERIFY2(!splitter->handle(1),
            qPrintable(QStringLiteral("收起后扩展区拖动条仍在")));
        QCOMPARE(splitter->width() - splitter->widget(0)->width(), 0);
        auto *central = window.centralWidget();
        const int leftInset = endpointBar->mapTo(central, QPoint(0, 0)).x();
        const int rightInset = central->width()
            - endpointBar->mapTo(central, QPoint(endpointBar->width(), 0)).x();
        QCOMPARE(rightInset, leftInset);
        // 端口条同理：它在左列里，左列宽度不变，它就不该变——这是用户报的
        // "端口设置区域跟着拉长"的直接反面。
        QCOMPARE(endpointBar->width(), expandedBarWidth);
        QVERIFY2(collapsedWindow < expandedWindow,
            qPrintable(QStringLiteral("收起后窗口应变窄：%1 → %2")
                           .arg(expandedWindow).arg(collapsedWindow)));
        // 窗口收窄量应当约等于面板宽度（那一段还给了桌面）。给一点余量：
        // 分栏对手柄怎么记账是 Qt 的实现细节（手柄在收起时归谁、QSS 画多宽），
        // 钉死几像素只会让用例在无关改动上红——**关键不变式是上面那条"数据区逐像素不变"**。
        const int shrunk = expandedWindow - collapsedWindow;
        QVERIFY2(qAbs(shrunk - expandedPanel) <= 12,
            qPrintable(QStringLiteral("窗口收窄 %1px，应约等于面板宽度 %2px")
                           .arg(shrunk).arg(expandedPanel)));

        // 展开：数据区仍然原地不动（核心不变式），面板重新出现，窗口加宽回去。
        toggle->setChecked(true);
        QTest::qWait(50);
        QCOMPARE(splitter->widget(0)->width(), expandedData);
        QCOMPARE(splitter->indexOf(panel), 1);
        QVERIFY2(splitter->widget(1)->width() > 0, qPrintable(QStringLiteral("面板应重新出现")));
        QVERIFY2(splitter->handle(1)->isVisible(),
            qPrintable(QStringLiteral("展开后扩展区拖动条应重新出现")));
        const int expandedGap = splitter->width() - splitter->widget(0)->width()
            - splitter->widget(1)->width();
        QVERIFY2(expandedGap > 0,
            qPrintable(QStringLiteral("展开后左右拖动条应重新占宽，实际 %1").arg(expandedGap)));
        const int onceExpandedWindow = window.width();
        QVERIFY2(onceExpandedWindow > collapsedWindow,
            qPrintable(QStringLiteral("展开后窗口应变宽：%1 → %2")
                           .arg(collapsedWindow).arg(onceExpandedWindow)));

        // ★ 幂等：再收起再展开一轮，几何必须与上一轮**完全相同**。
        // 这条比"回到最初那个数字"更有意义——最初那个宽度是初始布局给的（面板拿到的
        // 实际宽度可能略小于它自己的最小宽度），把它当基准会钉在一个偶然值上。
        // 而"反复切换不漂移"才是用户真正会遇到的：连点几次扩展，窗口不该越点越宽。
        toggle->setChecked(false);
        QTest::qWait(50);
        QCOMPARE(splitter->widget(0)->width(), expandedData);
        QCOMPARE(window.width(), collapsedWindow);
        QVERIFY(!splitter->handle(1));
        toggle->setChecked(true);
        QTest::qWait(50);
        QCOMPARE(splitter->widget(0)->width(), expandedData);
        QCOMPARE(window.width(), onceExpandedWindow);
    }

    // 左右拖动条要能往主界面一侧推：左列内容再宽，也不能把快捷面板的宽度封住。
    void splitterMovesIntoTheMainColumn()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 800);
        window.show();
        QTest::qWait(50);

        auto *splitter = window.findChild<QSplitter *>(QStringLiteral("horizontalSplitter"));
        auto *toggle = window.findChild<QPushButton *>(QStringLiteral("commandPanelToggle"));
        QVERIFY(splitter && toggle);
        if (!toggle->isChecked()) toggle->setChecked(true);
        QTest::qWait(50);
        QCOMPARE(splitter->count(), 2);

        const int panelBefore = splitter->sizes().value(1);
        int lower = -1;
        int upper = -1;
        splitter->getRange(1, &lower, &upper);
        QVERIFY2(lower < 80,
            qPrintable(QStringLiteral("往主界面拖动被内容宽度挡住：手柄最近只能到 %1（最远 %2）")
                           .arg(lower).arg(upper)));

        const int handle = qMax(0, splitter->width() - splitter->sizes().value(0)
            - splitter->sizes().value(1));
        splitter->setSizes({80, qMax(1, splitter->width() - handle - 80)});
        QTest::qWait(30);
        QVERIFY2(splitter->sizes().value(0) < 160,
            qPrintable(QStringLiteral("主界面没有让出宽度：左列 %1")
                           .arg(splitter->sizes().value(0))));
        QVERIFY2(splitter->sizes().value(1) > panelBefore,
            qPrintable(QStringLiteral("快捷面板没有变宽：%1 → %2")
                           .arg(panelBefore).arg(splitter->sizes().value(1))));
    }

    void messagesCarryStructuredFacts()
    {
        // 消息是值类型：原始字节、时间、来源、长度都是数据，显示设置改不了它们。
        PortDraft port;
        port.kind = PortDraft::TcpServer;
        const auto source = Traffic::makeSource(port, 1);
        QCOMPARE(source.endpointIndex, 1);
        QCOMPARE(source.endpointKind, int(PortDraft::TcpServer));
        QVERIFY(!source.hasPeer()); // 未连接时没有对端，字段留空而不是塞占位串

        Traffic::Message message;
        message.id = 7;
        message.timestamp = QDateTime::fromString(QStringLiteral("2026-10-02T12:00:00.123"), Qt::ISODateWithMs);
        message.direction = Traffic::Direction::Transmit;
        message.origin = Traffic::Origin::Forward;
        message.framing = Traffic::Framing::Stream;
        message.hasMoreFragments = true;
        message.source = source;
        message.raw = QByteArray::fromHex("0102");
        message.rawLength = 4096; // 真实长度远大于保留的预览
        message.truncated = true;
        message.state = Traffic::State::Partial;

        // 语义查询：方向、来源、发送状态互不干扰。
        QVERIFY(message.isData());
        QVERIFY(message.isFragment());
        QCOMPARE(message.displayBytes(), Q_INT64_C(4096)); // 显示真实长度，不是预览长度
        QVERIFY(message.isValid());
        QCOMPARE(Traffic::directionName(message.direction), QStringLiteral("TX"));
        QCOMPARE(Traffic::originName(message.origin), QStringLiteral("FWD"));
        QCOMPARE(Traffic::stateName(message.state), QStringLiteral("部分写出"));
        QCOMPARE(Traffic::framingName(message.framing), QStringLiteral("流片段"));
        // 非数据类消息不参与收发字节统计，isData() 是唯一的判定入口。
        Traffic::Message state;
        state.direction = Traffic::Direction::State;
        QVERIFY(!state.isData());
        QVERIFY(!state.isValid()); // 空消息无任何事实，不该被当成有效记录
    }

    void structuredFiltersUseFactsNotLabels()
    {
        TrafficModel model;

        PortDraft portA, portB;
        portA.kind = PortDraft::Serial;
        portB.kind = PortDraft::TcpServer;
        const auto a = Traffic::makeSource(portA, 0);
        auto b = Traffic::makeSource(portB, 1);
        b.peerAddress = QStringLiteral("127.0.0.1:52344");

        const auto stream = Traffic::makeSampleMessage(
            1, QDateTime(), a, Traffic::Direction::Receive, QByteArray::fromHex("aa"), Traffic::Framing::Stream);
        const auto datagram = Traffic::makeSampleMessage(
            2, QDateTime(), b, Traffic::Direction::Receive, QByteArray::fromHex("bb"), Traffic::Framing::Datagram);
        const auto message = Traffic::makeSampleMessage(
            3, QDateTime(), b, Traffic::Direction::Receive, QByteArray(64, 'z'), Traffic::Framing::Message);
        model.replaceRecords({stream, datagram, message});

        const auto accepted = [&model](const TrafficFilter::Options &options) {
            int count = 0;
            for (int i = 0; i < model.rowCount(); ++i)
                if (TrafficFilter::accepts(model.record(i), options, PayloadCodec::Format::Hex)) ++count;
            return count;
        };
        TrafficFilter::Options options;
        options.endpoints = {a.endpointId, b.endpointId};
        QCOMPARE(accepted(options), 3);

        // 对端地址是来源的结构化字段，能被关键词直接命中，不必先渲染成显示文本。
        options.keywords = {QStringLiteral("52344")};
        QCOMPARE(accepted(options), 2);
        options.keywords.clear();

        // 转发开关只藏桥接产生的发送行。手动发送和接收仍在。
        auto forwarded = Traffic::makeSampleMessage(
            6, QDateTime(), a, Traffic::Direction::Transmit, QByteArray("fwd"));
        forwarded.origin = Traffic::Origin::Forward;
        const auto manual = Traffic::makeSampleMessage(
            7, QDateTime(), a, Traffic::Direction::Transmit, QByteArray("hand"));
        model.replaceRecords({stream, forwarded, manual});
        options.endpoints = {a.endpointId};
        QCOMPARE(accepted(options), 3);
        options.forwards = false;
        QCOMPARE(accepted(options), 2);
        options.tx = false;
        QCOMPARE(accepted(options), 1);
        options.tx = true;
        options.forwards = true;
        options.endpoints = {a.endpointId, b.endpointId};
        model.replaceRecords({stream, datagram, message});

        // 信息/警告/错误/状态各自有独立开关：关掉后对应事件不进显示，数据不受影响。
        const auto warning = Traffic::makeSampleMessage(
            4, QDateTime(), b, Traffic::Direction::Warning, {});
        const auto error = Traffic::makeSampleMessage(
            5, QDateTime(), b, Traffic::Direction::Error, {});
        const auto info = Traffic::makeSampleMessage(
            8, QDateTime(), b, Traffic::Direction::Info, {});
        const auto state = Traffic::makeSampleMessage(
            9, QDateTime(), b, Traffic::Direction::State, {});
        model.replaceRecords({stream, datagram, message, warning, error, info, state});
        QCOMPARE(accepted(options), 7);
        options.warnings = false;
        options.errors = false;
        options.infos = false;
        options.states = false;
        QCOMPARE(accepted(options), 3);
        options.warnings = true;
        options.errors = true;
        options.infos = true;
        options.states = true;
    }

    // 端点草稿现在随配置即时持久化（endpoint/A、endpoint/B）：共享 ini 里
    // 前序用例留下的端点状态会污染"默认端点"假设，依赖该假设的用例先清场。
    // 用 remove 而不是写 QVariant()：后者留下 @Invalid()，不是"没有这个键"。
    void clearEndpointDrafts()
    {
        const QString ini = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/preferences.ini");
        PreferencesStore sweep(nullptr, ini);
        sweep.remove(QStringLiteral("endpoint/A"));
        sweep.remove(QStringLiteral("endpoint/B"));
    }

    // 每个用例跑完清掉共享 ini 里的过滤条件。过滤条会把条件写进共享配置，
    // 前一个用例留下的关键词会让后面的用例"一条都收不到"——与端点草稿同一类串扰，
    // 只是后果更隐蔽：屏幕全空看起来像面板坏了，而不是配置被污染了。
    // 放在 cleanup() 而不是各用例开头，是为了让用例中途断言失败也照样清场。
    void cleanup()
    {
        const QString ini = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/preferences.ini");
        PreferencesStore sweep(nullptr, ini);
        for (const auto *key : {"filter/keywords", "filter/excludes", "filter/regex",
                 "filter/caseSensitive", "filter/rx", "filter/tx", "filter/forward", "filter/state",
                 "filter/info", "filter/warning", "filter/error", "filter/states",
                 "filter/warnings", "filter/errors",
                 "filter/expanded",
                 // 显示开关同样会落盘：某个用例改过长度、HEX、换行后不还原，
                 // 下一个用例就会在"行数多一倍"的前提下断言——与过滤条件同一类串扰。
                 "traffic/prefix", "traffic/prefixLineNumber", "traffic/prefixTimestamp",
                 "traffic/prefixLength", "traffic/alwaysShowHex", "traffic/showAnsi", "traffic/autoWrap",
                 "traffic/number", "traffic/timestamp", "traffic/direction", "traffic/length",
                 "traffic/hex", "traffic/pure", "traffic/wrap", "traffic/follow",
                 // 编码格式与发送格式也会落盘，且后果最隐蔽：某个用例把发送格式切成
                 // GBK/UTF-8 后不切回来，下一个用例就会在"输入按文本编码"的前提下断言——
                 // 实测 `sendPreviewRejectsInvalidInput` 因此间歇失败，报的是
                 // `"01 GG"` 被解析成 7 字节（5 字符 + CRLF）而不是"输入错误"。
                 // 这类用例大多**显式设置** format，不依赖残留值，所以清掉是安全的。
                 "traffic/format", "send/format", "send/suffix", "send/enterToSend",
                 "send/clearOnEnter", "send/text",
                 "file/mode", "file/endpoint", "file/block", "file/direction",
                 "file/beforeId", "file/afterId", "file/beforeDelay", "file/afterDelay",
                 "file/send/beforeHex", "file/send/afterHex",
                 "file/receive/beforeHex", "file/receive/afterHex",
                 // 窗口几何与指令面板展开状态同样会落盘，而且后果最直接：某个用例把面板
                 // 收起 / 把窗口拖小之后不还原，下一个用例就在"面板已收起"的前提下断言。
                 // （commandPanel/expanded 由 CommandPanel::savePreferences 写入，属既有遗漏。）
                 "window/geometry", "window/width", "window/height", "window/stayOnTop",
                 "window/colorScheme",
                 "commandPanel/expanded", "commandPanel/contentWrap", "commandPanel/currentSet"})
            sweep.remove(QString::fromLatin1(key));
        // 外观是进程级单例。某个用例切到暗色后若没切回来，后面按浅色像素断言的用例会整片失败。
        if (AppTheme::mode() != AppTheme::Mode::Light)
            AppTheme::setMode(AppTheme::Mode::Light);
    }

    void portDraftsRemainIndependent()
    {
        clearEndpointDrafts();
        MainWindow window;
        auto *a = window.findChild<PortSelectorWidget *>("endpointA");
        auto *b = window.findChild<PortSelectorWidget *>("endpointB");
        QVERIFY(a && b);
        // Both endpoints are configurable at once; no row has to be selected first.
        QVERIFY(a->isVisibleTo(&window) && b->isVisibleTo(&window));
        auto *serial = a->findChild<QComboBox *>("serial.port");
        auto *bType = b->findChild<QComboBox *>("portType");
        a->setSerialPorts({{"COM17", "USB Serial", {}, {}}});
        serial->setCurrentIndex(serial->findData("COM17"));
        bType->setCurrentIndex(3);
        auto *udpPort = b->findChild<QSpinBox *>("udp.port");
        udpPort->setValue(23456);
        QCOMPARE(a->draft().values.value("serial.port").toString(), QString("COM17"));
        QCOMPARE(a->draft().kind, 0);
        QCOMPARE(b->draft().kind, 3);
        QCOMPARE(b->draft().values.value("udp.port").toInt(), 23456);
        QVERIFY(!b->draft().values.contains("serial.portUnset"));
        auto *badgeA = a->findChild<QLabel *>("EndpointBadge");
        auto *badgeB = b->findChild<QLabel *>("EndpointBadge");
        QCOMPARE(badgeA->text(), QString("A"));
        QCOMPARE(badgeB->text(), QString("B"));
        QCOMPARE(badgeA->property("port").toString(), QString("A"));
        QCOMPARE(badgeB->property("port").toString(), QString("B"));
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        const auto fill = [](QLabel *badge) {
            const QImage image = badge->grab().toImage();
            // 中心可能是抗锯齿字形；背景占像素多数，不依赖字体与 DPI。
            QHash<QRgb, int> counts;
            QRgb dominant = 0;
            int maximum = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const auto pixel = image.pixel(x, y);
                    const int count = ++counts[pixel];
                    if (count > maximum) { maximum = count; dominant = pixel; }
                }
            }
            return QColor::fromRgba(dominant);
        };
        QCOMPARE(fill(badgeA), QColor(0xB4, 0x53, 0x09));
        QCOMPARE(fill(badgeB), QColor(0x0E, 0x74, 0x90));
    }

    void portInputValidation()
    {
        PortSelectorWidget selector;
        PortDraft port;
        port.values.insert("serial.port", "COM3");
        selector.setDraft(port);
        auto *baud = selector.findChild<QComboBox *>("serial.baud");
        auto *edit = baud->lineEdit();
        QVERIFY(selector.hasValidConfiguration());
        edit->selectAll();
        QTest::keyClicks(edit, "250000xyz");
        QCOMPARE(edit->text(), QString("250000"));
        QInputMethodEvent ime;
        ime.setCommitString(QStringLiteral("我"));
        QApplication::sendEvent(edit, &ime);
        QCOMPARE(edit->text(), QString("250000"));
        QApplication::clipboard()->setText("115200qw");
        edit->selectAll();
        edit->paste();
        QCOMPARE(edit->text(), QString("250000"));
        QVERIFY(selector.hasValidConfiguration());
        edit->clear();
        QVERIFY(!selector.hasValidConfiguration());
        QVERIFY(selector.validationError().contains(QStringLiteral("波特率")));
        for (const auto &invalid : {"0", "-1", "1.5", "2147483648", "115200qw"}) {
            port.values.insert("serial.baud", invalid);
            selector.setDraft(port); // Imported/programmatic drafts must also be checked.
            QVERIFY2(!selector.hasValidConfiguration(), invalid);
        }
        baud->setEditText("2147483647");
        QVERIFY(selector.hasValidConfiguration());
        port = selector.draft();
        port.kind = 2;
        port.values.insert("server.address", "999.1.1.1");
        selector.setDraft(port);
        QVERIFY(!selector.hasValidConfiguration());
        auto *address = selector.findChild<QComboBox *>("server.address");
        QVERIFY(!address->isEditable());
        address->setCurrentIndex(address->findData(QStringLiteral("0.0.0.0")));
        QVERIFY(selector.hasValidConfiguration());
        port.kind = 1;
        port.values.insert("client.host", "device-1.example.com");
        selector.setDraft(port);
        QVERIFY(selector.hasValidConfiguration());
        selector.findChild<QComboBox *>("client.host")->setEditText("300.1.2.3");
        QVERIFY(!selector.hasValidConfiguration());
        selector.findChild<QComboBox *>("client.host")->setEditText("localhost");
        auto *remotePort = selector.findChild<QSpinBox *>("client.port");
        auto *portEdit = remotePort->findChild<QLineEdit *>();
        portEdit->selectAll();
        QTest::keyClicks(portEdit, "12345xyz");
        QCOMPARE(remotePort->value(), 12345);
        portEdit->clear();
        QVERIFY(!selector.hasValidConfiguration());
        remotePort->setValue(9000);
        QVERIFY(selector.hasValidConfiguration());
        for (const auto &invalid : {"0", "65536", "abc", "1.5"}) {
            port.values.insert("client.port", invalid);
            selector.setDraft(port);
            QVERIFY2(!selector.hasValidConfiguration(), invalid);
            QCOMPARE(selector.draft().values.value("client.port").toString(), QString(invalid));
        }
        port.values.insert("client.port", 65535);
        selector.setDraft(port);
        QVERIFY(selector.hasValidConfiguration());
        port.kind = 3;
        port.values.insert("udp.target", ""); // Receive-only UDP remains valid.
        selector.setDraft(port);
        QVERIFY(selector.hasValidConfiguration());
    }

    void editableBaudRateUsesTheWholeComboEditField()
    {
        PortSelectorWidget selector;
        PortDraft port;
        port.values.insert(QStringLiteral("serial.port"), QStringLiteral("COM3"));
        port.values.insert(QStringLiteral("serial.baud"), QStringLiteral("2147483647"));
        selector.setDraft(port);
        selector.show();

        auto *baud = selector.findChild<QComboBox *>(QStringLiteral("serial.baud"));
        QVERIFY(baud);
        auto *edit = baud->lineEdit();
        QVERIFY(edit);
        edit->setFocus();
        edit->setCursorPosition(edit->text().size());
        QCoreApplication::processEvents();

        const auto editFieldWidth = [baud] {
            QStyleOptionComboBox option;
            option.initFrom(baud);
            option.currentText = baud->currentText();
            return baud->style()->subControlRect(
                QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, baud).width();
        };
        QVERIFY2(editFieldWidth() >= edit->fontMetrics().horizontalAdvance(edit->text()),
                 "The editable baud-rate field must fit the longest legal value");

        // A preset loaded after a longer custom value must reset the embedded editor's
        // horizontal scroll. The first digit of 921600 was previously hidden in this path.
        port.values.insert(QStringLiteral("serial.baud"), QStringLiteral("2000000"));
        selector.setDraft(port);
        QCoreApplication::processEvents();
        QCOMPARE(edit->text(), QStringLiteral("2000000"));
        QVERIFY2(editFieldWidth() >= edit->fontMetrics().horizontalAdvance(edit->text()),
                 "The editable baud-rate field must fit 2000000");

        port.values.insert(QStringLiteral("serial.baud"), QStringLiteral("921600"));
        selector.setDraft(port);
        QCoreApplication::processEvents();
        QCOMPARE(edit->text(), QStringLiteral("921600"));
        QCOMPARE(edit->cursorPosition(), 0);

        // The embedded editor must not retain the generic line-edit frame/padding. Otherwise
        // Qt may scroll the text left even when the combo still has visibly unused space.
        QCOMPARE(edit->textMargins(), QMargins());
        QVERIFY(edit->contentsRect().width() >= edit->fontMetrics().horizontalAdvance(edit->text()));
    }

    void localBindingsUseInterfacePickerAndAutoPort()
    {
        PortSelectorWidget selector;
        PortDraft port;
        port.kind = PortDraft::Udp;
        port.values.insert("udp.target", "");
        selector.setDraft(port);

        auto *bindAddress = selector.findChild<QComboBox *>("udp.address");
        auto *bindPort = selector.findChild<QSpinBox *>("udp.port");
        auto *target = selector.findChild<QComboBox *>("udp.target");
        QVERIFY(bindAddress && bindPort && target);
        QVERIFY(!bindAddress->isEditable());
        QVERIFY(bindAddress->findData(QStringLiteral("0.0.0.0")) >= 0);
        QCOMPARE(bindAddress->currentData().toString(), QStringLiteral("0.0.0.0"));
        QCOMPARE(bindPort->minimum(), 0);
        QCOMPARE(bindPort->value(), 0);
        QVERIFY(target->isEditable());
        QVERIFY(target->lineEdit()->placeholderText().contains(QStringLiteral("仅接收")));
        QVERIFY(bindAddress->toolTip().contains(QStringLiteral("本机 IPv4 网卡")));
        QVERIFY(selector.hasValidConfiguration());
        QCOMPARE(selector.draft().values.value("udp.address").toString(), QStringLiteral("0.0.0.0"));

        port = PortDraft{};
        port.kind = PortDraft::TcpServer;
        selector.setDraft(port);
        auto *serverBind = selector.findChild<QComboBox *>("server.address");
        QVERIFY(serverBind);
        QVERIFY(!serverBind->isEditable());
        QVERIFY(serverBind->findData(QStringLiteral("0.0.0.0")) >= 0);
        QCOMPARE(serverBind->currentData().toString(), QStringLiteral("0.0.0.0"));
        QVERIFY(serverBind->toolTip().contains(QStringLiteral("本机 IPv4 网卡")));
        QCOMPARE(selector.draft().values.value("server.address").toString(), QStringLiteral("0.0.0.0"));

        for (int i = 1; i < serverBind->count(); ++i) {
            const QString address = serverBind->itemData(i).toString();
            QVERIFY(serverBind->itemText(i).contains(QStringLiteral(" · ")));
            QVERIFY(serverBind->itemText(i).endsWith(address));
            QVERIFY(serverBind->itemData(i, Qt::ToolTipRole).toString().contains(QStringLiteral("网卡：")));
        }
    }

    void udpSourcesAreVisibleWithoutChangingTheSendTarget()
    {
        // 每条 UDP 接收记录都在监视区的 RX 后带上实际来源。
        TrafficTextView view;
        TrafficTextView::DisplayOptions options;
        options.prefixLineNumber = false;
        options.prefixTimestamp = false;
        // 这条只核对来源地址的位置。数据长度和 HEX 对照是另一组开关，这里关掉。
        options.prefixLength = false;
        options.alwaysShowHex = false;
        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.framing = Traffic::Framing::Datagram;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Udp;
        message.source.endpointId = QUuid::createUuid();
        message.source.peerAddress = QStringLiteral("127.0.0.1:9001");
        message.raw = QByteArray("ping");
        message.rawLength = message.raw.size();
        view.appendMessages({message}, PayloadCodec::Format::Utf8, options);
        QCOMPARE(view.toPlainText(), QStringLiteral("[A] RX <<-- 127.0.0.1:9001: ping"));

        Traffic::Message sent = message;
        sent.direction = Traffic::Direction::Transmit;
        sent.origin = Traffic::Origin::Manual;
        sent.raw = QByteArray("pong");
        sent.rawLength = sent.raw.size();
        TrafficTextView sentView;
        sentView.appendMessages({sent}, PayloadCodec::Format::Utf8, options);
        QCOMPARE(sentView.toPlainText(), QStringLiteral("[A] TX+MAN -->> 127.0.0.1:9001: pong"));

        // 字节数跟在方向后面，按内容拼接，不补空格。状态行不画箭头。
        Traffic::Message rx = message;
        rx.source.peerAddress.clear();
        rx.source.endpointKind = PortDraft::Serial;
        Traffic::Message tx = sent;
        tx.source.peerAddress.clear();
        Traffic::Message err = rx;
        err.direction = Traffic::Direction::Error;
        err.raw.clear();
        err.rawLength = 0;
        err.statusText = QStringLiteral("denied");
        TrafficTextView::DisplayOptions withLength = options;
        withLength.prefixLength = true;
        TrafficTextView lengthView;
        lengthView.appendMessages({rx, tx, err}, PayloadCodec::Format::Utf8, withLength);
        const auto lengthLines = lengthView.toPlainText().split(QLatin1Char('\n'));
        QCOMPARE(lengthLines.at(0), QStringLiteral("[A] RX [4B] <<--: ping"));
        QCOMPARE(lengthLines.at(1), QStringLiteral("[A] TX+MAN [4B] -->>: pong"));
        QCOMPARE(lengthLines.at(2), QStringLiteral("[A] E: denied"));

        // 关掉「转发」只从数据区拿走桥接产生的发送行。接收和手动发送留下。
        clearEndpointDrafts();
        MainWindow forwardWindow;
        forwardWindow.setAttribute(Qt::WA_DontShowOnScreen);
        forwardWindow.show();
        auto *panel = forwardWindow.findChild<TrafficPanel *>();
        QVERIFY(panel);
        panel->setPreferences(nullptr);
        // 显式使用文本格式，避免已有偏好影响正文断言。
        forwardWindow.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        auto *portA = forwardWindow.findChild<QPushButton *>(QStringLiteral("showPortA"));
        QVERIFY(portA);
        // 端点身份由 refreshEndpoints 在显示之后写入。读早了是空 UUID，行会被端点过滤丢掉。
        QTRY_VERIFY(!portA->property("endpoint").toUuid().isNull());
        const QUuid endpointId = portA->property("endpoint").toUuid();
        auto line = [&](Traffic::Direction direction, Traffic::Origin origin, const char *body) {
            Traffic::Message row;
            row.id = 1;
            row.timestamp = QDateTime::currentDateTime();
            row.direction = direction;
            row.origin = origin;
            row.source.endpointId = endpointId;
            row.source.endpointIndex = 0;
            row.raw = QByteArray(body);
            row.rawLength = row.raw.size();
            return row;
        };
        panel->appendMessages({
            line(Traffic::Direction::Receive, Traffic::Origin::Automatic, "in"),
            line(Traffic::Direction::Transmit, Traffic::Origin::Forward, "bridged"),
            line(Traffic::Direction::Transmit, Traffic::Origin::Manual, "hand"),
        });
        auto *text = forwardWindow.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(text);
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("bridged")));
        auto *hideForward = forwardWindow.findChild<QPushButton *>(QStringLiteral("filterForward"));
        QVERIFY(hideForward);
        hideForward->click();
        auto *rebuild = forwardWindow.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        QVERIFY(rebuild);
        rebuild->click();
        QTRY_VERIFY(!text->toPlainText().contains(QStringLiteral("bridged")));
        QVERIFY(text->toPlainText().contains(QStringLiteral("hand")));
        QVERIFY(text->toPlainText().contains(QStringLiteral("in")));

        // 关掉前缀后不加行号、时间、端口方向，HEX 对照也不再另起一行。
        TrafficTextView pureView;
        options.prefix = false;
        options.alwaysShowHex = true;
        pureView.appendMessages({message}, PayloadCodec::Format::Utf8, options);
        QCOMPARE(pureView.toPlainText(), QStringLiteral("ping"));

        PortSelectorWidget selector;
        PortDraft draft;
        draft.kind = PortDraft::Udp;
        draft.values.insert(QStringLiteral("udp.target"), QStringLiteral("192.168.1.10"));
        selector.setDraft(draft);
        auto *hint = selector.findChild<QLabel *>(QStringLiteral("udpPeerHint"));
        QVERIFY(hint);
        selector.setPeerList({QStringLiteral("127.0.0.1:9001"), QStringLiteral("192.168.1.20:8000")});
        QCOMPARE(hint->text(), QStringLiteral("本次已收到来自 2 个来源的数据报"));
        QCOMPARE(hint->toolTip(), QStringLiteral("127.0.0.1:9001\n192.168.1.20:8000"));
        QCOMPARE(selector.draft().values.value(QStringLiteral("udp.target")).toString(),
                 QStringLiteral("192.168.1.10"));
    }

    void endpointSummaryTracksConfigurationAndType()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        auto *selector = window.findChild<PortSelectorWidget *>("endpointA");
        QVERIFY(selector);
        auto *type = selector->findChild<QComboBox *>("portType");
        auto *badge = selector->findChild<QLabel *>("EndpointBadge");
        QVERIFY(type && badge);
        const auto summary = [selector] { return portDraftSummary(selector->draft()); };
        QVERIFY(summary().contains("115200"));
        QVERIFY(summary().contains("8N1"));
        QVERIFY(!summary().contains("127.0.0.1"));
        selector->setSerialPorts({{"COM3", "USB-SERIAL CH340", {}, "COM3"}});
        auto *serial = selector->findChild<QComboBox *>("serial.port");
        serial->setCurrentIndex(serial->findData("COM3"));
        QCOMPARE(summary(), QStringLiteral("COM3 · 115200 · 8N1 · 无流控"));
        selector->findChild<QComboBox *>("serial.baud")->setEditText("921600");
        selector->findChild<QComboBox *>("serial.bits")->setCurrentText("7");
        selector->findChild<QComboBox *>("serial.parity")->setCurrentText("Even");
        selector->findChild<QComboBox *>("serial.stop")->setCurrentText("2");
        selector->findChild<QComboBox *>("serial.flow")->setCurrentText("RTS/CTS");
        QCOMPARE(summary(), QString("COM3 · 921600 · 7E2 · RTS/CTS"));
        QVERIFY(badge->toolTip().contains(QStringLiteral("偶校验")));
        QVERIFY(!badge->toolTip().contains(QStringLiteral("状态"))); // State lives on the button.
        type->setCurrentIndex(1);
        // Identity is derived from the row and the live type; it is never user-edited.
        QCOMPARE(endpointLabel(0, type->currentIndex()), QStringLiteral("A · TCP 客户端"));
        QCOMPARE(summary(), QString("127.0.0.1:9000"));
        // No status label: the action button carries the state for every type.
        QVERIFY(!selector->findChild<QLabel *>("EndpointStatus"));
        auto *action = selector->findChild<QPushButton *>("endpointAction");
        QCOMPARE(action->text(), QStringLiteral("连  接"));
        // The glyph follows the action: a link to dial out, an access point for the ones that
        // listen, a broken link once open. Compared as pixels because QIcon lost its
        // comparison operators in Qt 6.
        QVERIFY(!action->icon().isNull());
        const QSize glyphSize(16, 16);
        const QImage connectGlyph = action->icon().pixmap(glyphSize).toImage();
        type->setCurrentIndex(2);
        QVERIFY(action->icon().pixmap(glyphSize).toImage() != connectGlyph);
        type->setCurrentIndex(1);
        QCOMPARE(action->icon().pixmap(glyphSize).toImage(), connectGlyph);
        selector->setConnectionOpen(true);
        QVERIFY(action->icon().pixmap(glyphSize).toImage() != connectGlyph);
        selector->setConnectionOpen(false);
        QCOMPARE(action->icon().pixmap(glyphSize).toImage(), connectGlyph);
        type->setCurrentIndex(2);
        QCOMPARE(action->text(), QStringLiteral("监  听"));
        type->setCurrentIndex(3);
        QCOMPARE(action->text(), QStringLiteral("绑  定"));
        // Open state flips the verb and the colour flag, without a second indicator.
        selector->setConnectionOpen(true);
        QCOMPARE(action->text(), QStringLiteral("解  绑"));
        QCOMPARE(action->property("open").toBool(), true);
        type->setCurrentIndex(2);
        QCOMPARE(action->text(), QStringLiteral("停  止"));
        type->setCurrentIndex(1);
        QCOMPARE(action->text(), QStringLiteral("断  开"));
        selector->setConnectionOpen(false);
        QCOMPARE(action->text(), QStringLiteral("连  接"));
        QCOMPARE(action->property("open").toBool(), false);
        type->setCurrentIndex(0);
        QCOMPARE(endpointLabel(0, type->currentIndex()), QStringLiteral("A · 串口"));
        QCOMPARE(summary(), QString("COM3 · 921600 · 7E2 · RTS/CTS"));
        // The editable name field is gone; there is no "portName" widget any more.
        QVERIFY(!selector->findChild<QLineEdit *>("portName"));
        type->setCurrentIndex(3);
        QCOMPARE(endpointLabel(0, type->currentIndex()), QStringLiteral("A · UDP"));
        type->setCurrentIndex(0);
        for (const auto &setting : QMap<QString, QString>{{"None", "N"}, {"Odd", "O"}, {"Even", "E"},
                                                          {"Mark", "M"}, {"Space", "S"}}.asKeyValueRange()) {
            auto draft = selector->draft();
            draft.values.insert("serial.parity", setting.first);
            QVERIFY(portDraftSummary(draft).contains("7" + setting.second + "2"));
        }
        const auto artifactDir = qEnvironmentVariable("QQCOM_UI_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QTest::qWait(30);
            QDir().mkpath(artifactDir);
            QVERIFY(window.grab().save(artifactDir + "/endpoint-summary.png"));
        }
    }

    void serialSelectionUsesPortIdentity()
    {
        PortSelectorWidget selector;
        PortDraft port;
        selector.setDraft(port);
        selector.setSerialPorts({{"COM3", "USB-SERIAL CH340", {}, "COM3"},
                                 {"COM17", "USB Serial Port", {}, "COM17"}});
        auto *combo = selector.findChild<QComboBox *>("serial.port");
        QVERIFY(combo && !combo->isEditable());
        QCOMPARE(combo->count(), 2);
        QCOMPARE(combo->currentIndex(), -1); // Scanning must not silently choose a device.
        combo->setCurrentIndex(combo->findData("COM17"));
        QVERIFY(combo->currentText().contains("USB Serial Port"));
        QCOMPARE(selector.draft().values.value("serial.port").toString(), QString("COM17"));
        QVERIFY(selector.hasValidConfiguration());
        selector.setSerialPorts({{"COM17", "Updated friendly name", {}, "COM17"},
                                 {"COM3", "USB-SERIAL CH340", {}, "COM3"}});
        QCOMPARE(combo->currentData().toString(), QString("COM17"));
        QVERIFY(combo->currentText().contains("Updated friendly name"));
        selector.setSerialPorts({{"COM3", "USB-SERIAL CH340", {}, "COM3"}});
        QCOMPARE(combo->currentData().toString(), QString("COM17"));
        QVERIFY(combo->currentText().contains(QStringLiteral("不可用")));
        QVERIFY(!selector.hasValidConfiguration());
        selector.setSerialPorts({{"COM17", "USB Serial Port", {}, "COM17"}});
        QVERIFY(selector.hasValidConfiguration());
        // 手动刷新按钮已移除：扫描由组合根的定时器驱动，服务侧行为见 serialDiscoveryPublishesSnapshot。
    }

    void serialDiscoveryPublishesSnapshot()
    {
        PortDiscoveryService discovery;
        QSignalSpy snapshots(&discovery, &PortDiscoveryService::portsChanged);
        QSignalSpy busy(&discovery, &PortDiscoveryService::refreshingChanged);
        discovery.refresh();
        discovery.refresh(); // Coalesce repeated clicks while discovery is running.
        QCOMPARE(busy.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(snapshots.count(), 1, 10000);
        QCOMPARE(busy.count(), 2);
        QSignalSpy scans(&discovery, &PortDiscoveryService::scanFinished);
        discovery.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(scans.count(), 1, 10000);
        QCOMPARE(scans.first().at(0).toULongLong(), 2ULL);
        QCOMPARE(busy.count(), 4);
        QVERIFY(!busy.last().first().toBool());
        const auto ports = qvariant_cast<QList<SerialPortDescriptor>>(snapshots.first().first());
        QSet<QString> names;
        for (const auto &port : ports) {
            QVERIFY(!port.portName.isEmpty());
            QVERIFY(!names.contains(port.portName));
            names.insert(port.portName);
        }
    }

    void addressHistoryPersistsAndIsBounded()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto fileName = directory.filePath("preferences.ini");
        {
            PreferencesStore store(nullptr, fileName);
            store.rememberAddress("bad address", false);
            store.rememberAddress("999.1.1.1", false);
            store.rememberAddress("192.168.", true);
            QVERIFY(store.addressHistory(false).isEmpty());
            QVERIFY(store.addressHistory(true).isEmpty());
            for (int i = 1; i <= 25; ++i) store.rememberAddress(QString("10.0.0.%1").arg(i), false);
            QCOMPARE(store.addressHistory(false).size(), 20);
            QVERIFY(!store.addressHistory(false).contains("10.0.0.1"));
            store.rememberAddress("10.0.0.10", false);
            QCOMPARE(store.addressHistory(false).first(), QString("10.0.0.10"));
            QCOMPARE(store.addressHistory(false).size(), 20);
            store.rememberAddress("Device.Example", false);
            store.rememberAddress("DEVICE.EXAMPLE", false);
            QCOMPARE(store.addressHistory(false).count("device.example"), 1);
            store.rememberAddress("0.0.0.0", true);
            store.rememberAddress("device.example", true);
        }
        PreferencesStore reloaded(nullptr, fileName);
        QCOMPARE(reloaded.addressHistory(false).first(), QString("device.example"));
        QCOMPARE(reloaded.addressHistory(false).size(), 20);
        QCOMPARE(reloaded.addressHistory(true), QStringList{"0.0.0.0"});
    }

    void addressEditsCommitOnCompletion()
    {
        QTemporaryDir directory;
        PreferencesStore store(nullptr, directory.filePath("preferences.ini"));
        PortSelectorWidget selector;
        selector.setAttribute(Qt::WA_DontShowOnScreen);
        selector.resize(980, 96);
        const auto update = [&] { selector.setAddressHistory(store.addressHistory(false), store.addressHistory(true)); };
        connect(&selector, &PortSelectorWidget::addressCommitted, &store, &PreferencesStore::rememberAddress);
        connect(&store, &PreferencesStore::addressHistoryChanged, &selector, update);
        PortDraft port;
        port.kind = 1;
        selector.setDraft(port);
        selector.show();
        auto *client = selector.findChild<QComboBox *>("client.host");
        auto *udp = selector.findChild<QComboBox *>("udp.target");
        auto *edit = client->lineEdit();
        edit->setFocus();
        edit->selectAll();
        QTest::keyClicks(edit, "192.168.8.10");
        QVERIFY(store.addressHistory(false).isEmpty());
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(store.addressHistory(false).first(), QString("192.168.8.10"));
        QVERIFY(udp->findText("192.168.8.10") >= 0);
        edit->selectAll();
        QTest::keyClicks(edit, "device.example");
        QTest::keyClick(edit, Qt::Key_Return);
        QVERIFY(client->findText("device.example") >= 0);
        QCOMPARE(udp->findText("device.example"), -1); // UDP only offers IPv4 destinations.
        edit->selectAll();
        QTest::keyClicks(edit, "10.20.30.40");
        selector.setDraft(port); // Switching endpoints also commits the last complete edit.
        QCOMPARE(store.addressHistory(false).first(), QString("10.20.30.40"));
        edit->selectAll();
        QTest::keyClicks(edit, "999.1.1.1");
        selector.commitAddressHistory();
        QVERIFY(!store.addressHistory(false).contains("999.1.1.1"));
        QCOMPARE(store.addressHistory(true).size(), 0);
        PreferencesStore reloaded(nullptr, directory.filePath("preferences.ini"));
        PortSelectorWidget reopened;
        reopened.setAddressHistory(reloaded.addressHistory(false), reloaded.addressHistory(true));
        QVERIFY(reopened.findChild<QComboBox *>("client.host")->findText("10.20.30.40") >= 0);
    }

    // 历史上限必须同时限字节和条数。只限字节时，串口遥测那种几十字节的小帧
    // 能把条数堆到十几万，每条 Message 的元数据（时间、来源、三个 QUuid、
    // 两个 QString）远大于它那点载荷——"限了字节"不等于"限了内存"。
    // 默认预算是**安全阀**（512 MiB / 2000 万条，正常压测碰不到），用例要验的是
    // 淘汰机制本身，所以显式把预算压小——被测对象从"那个数字"变成"这套语义"。
    void historyBudgetAlsoLimitsRowCount()
    {
        TrafficModel model;
        const int rowBudget = 5000;
        model.setBudget(64LL * 1024 * 1024, rowBudget);
        Traffic::Message tiny;
        tiny.direction = Traffic::Direction::Receive;
        tiny.source.endpointIndex = 0;
        tiny.source.endpointKind = PortDraft::Serial;
        tiny.source.endpointId = QUuid::createUuid();
        tiny.raw = QByteArray::fromHex("0102"); // 2 字节：64 MiB 字节预算能装三千万条
        tiny.rawLength = tiny.raw.size();

        const int rounds = rowBudget / 1000 + 10;
        quint64 id = 1;
        for (int round = 0; round < rounds; ++round) {
            QList<Traffic::Message> batch;
            batch.reserve(1000);
            for (int i = 0; i < 1000; ++i) {
                auto message = tiny;
                message.id = id++;
                batch.append(message);
            }
            model.appendRecords(batch);
        }
        QCOMPARE(model.rowCount(), rowBudget);
        // 淘汰只从最旧一端发生：最新的一定在，最旧的恰好被挤掉。
        QCOMPARE(model.record(model.rowCount() - 1).id, id - 1);
        QCOMPARE(model.record(0).id, id - rowBudget);
    }

    void historyBudgetHandlesOversizedBatch()
    {
        TrafficModel model;
        model.setBudget(5, 3);
        QList<Traffic::Message> batch;
        for (int i = 1; i <= 10; ++i) {
            Traffic::Message message;
            message.id = i;
            message.raw = "xy";
            batch.append(message);
        }
        QCOMPARE(model.appendRecords(batch), 8);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.rawBytes(), qint64(4));
        QCOMPARE(model.record(0).id, quint64(9));
        QCOMPARE(model.record(1).id, quint64(10));
        model.setBudget(100, 1);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.record(0).id, quint64(10));
        Traffic::Message oversized;
        oversized.raw = QByteArray(8192, 'x');
        QCOMPARE(model.appendRecords({oversized}), 2);
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.rawBytes(), qint64(0));
    }

    void displayPreferencesPersistAcrossPanels()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto fileName = directory.filePath("preferences.ini");
        // 第一组面板：改显示开关即写入偏好。QSettings 在 PreferencesStore 析构时刷盘，
        // 所以每一组都放在自己的作用域里，下一组打开文件时上一组已经写完。
        {
            PreferencesStore firstStore(nullptr, fileName);
            TrafficPanel firstTraffic;
            firstTraffic.setPreferences(&firstStore);
            SendPanel firstSender;
            firstSender.setPreferences(&firstStore);
            auto *wrap = firstTraffic.findChild<QAction *>(QStringLiteral("autoWrap"));
            auto *rx = firstTraffic.findChild<QPushButton *>(QStringLiteral("filterRx"));
            auto *format = firstTraffic.findChild<QComboBox *>(QStringLiteral("trafficFormat"));
            auto *sendFormat = firstSender.findChild<QComboBox *>(QStringLiteral("sendFormat"));
            auto *enterToSend = firstSender.findChild<QPushButton *>(QStringLiteral("enterToSend"));
            auto *clearOnEnter = firstSender.findChild<QPushButton *>(QStringLiteral("clearOnEnter"));
            auto *editor = firstSender.findChild<QPlainTextEdit *>(QStringLiteral("sendEditor"));
            QVERIFY(wrap && rx && format && sendFormat && enterToSend && clearOnEnter && editor);
            wrap->setChecked(false);
            rx->setChecked(false);
            format->setCurrentText(QStringLiteral("HEX"));
            sendFormat->setCurrentText(QStringLiteral("GBK"));
            enterToSend->setChecked(true);
            clearOnEnter->setChecked(true);
            editor->setPlainText(QStringLiteral("01 03 00 00 00 02 C4 0B"));
        }
        // 第二组面板：从同一份配置读回，用户上次的选择原样恢复。
        {
            PreferencesStore secondStore(nullptr, fileName);
            TrafficPanel secondTraffic;
            secondTraffic.setPreferences(&secondStore);
            SendPanel secondSender;
            secondSender.setPreferences(&secondStore);
            QCOMPARE(secondTraffic.findChild<QAction *>(QStringLiteral("autoWrap"))->isChecked(), false);
            QCOMPARE(secondTraffic.findChild<QPushButton *>(QStringLiteral("filterRx"))->isChecked(), false);
            QCOMPARE(secondTraffic.findChild<QComboBox *>(QStringLiteral("trafficFormat"))->currentText(),
                     QStringLiteral("HEX"));
            QCOMPARE(secondSender.findChild<QComboBox *>(QStringLiteral("sendFormat"))->currentText(),
                     QStringLiteral("GBK"));
            QCOMPARE(secondSender.findChild<QPushButton *>(QStringLiteral("enterToSend"))->isChecked(), true);
            QCOMPARE(secondSender.findChild<QPushButton *>(QStringLiteral("clearOnEnter"))->isChecked(), true);
            QCOMPARE(secondSender.findChild<QPlainTextEdit *>(QStringLiteral("sendEditor"))->toPlainText(),
                     QStringLiteral("01 03 00 00 00 02 C4 0B"));
            // 清空也要记住。键存在且值为空时，不能退回示例正文。
            secondSender.findChild<QPlainTextEdit *>(QStringLiteral("sendEditor"))->clear();
        }
        {
            PreferencesStore thirdStore(nullptr, fileName);
            SendPanel thirdSender;
            thirdSender.setPreferences(&thirdStore);
            QCOMPARE(thirdSender.findChild<QPlainTextEdit *>(QStringLiteral("sendEditor"))->toPlainText(),
                     QString());
        }
    }

    void pendingTrafficIsLoggedOnDestruction()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PreferencesStore store(nullptr, directory.filePath("preferences.ini"));
        store.setValue("traffic/recordPath", directory.filePath("tail.log"));
        store.setValue("traffic/record", true);
        store.setValue("traffic/format", "UTF-8");
        {
            TrafficPanel panel;
            panel.setPreferences(&store);
            Traffic::Message message;
            message.timestamp = QDateTime::currentDateTime();
            message.raw = "pending-at-exit";
            message.rawLength = message.raw.size();
            panel.appendMessage(message);
            // 不推进事件循环，确保这一批尚未进入显示刷新。
        }
        const auto paths = QDir(directory.path()).entryList({"tail-*.log"}, QDir::Files);
        QCOMPARE(paths.size(), 1);
        QFile file(directory.filePath(paths.first()));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("pending-at-exit"));
    }

    void logRecordingFollowsDisplayFormat()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 用户选定的是"模板路径"：实际文件名每次带时间戳另起，模板本身不写。
        const auto templatePath = directory.filePath(QStringLiteral("traffic.log"));
        PreferencesStore store(nullptr, directory.filePath(QStringLiteral("preferences.ini")));
        // 预置路径：没手动指定时会写到程序目录下的 log，这里改到临时目录。
        store.setValue(QStringLiteral("traffic/recordPath"), templatePath);
        TrafficPanel panel;
        panel.setPreferences(&store);
        auto *record = panel.findChild<QAction *>(QStringLiteral("recordTraffic"));
        auto *format = panel.findChild<QComboBox *>(QStringLiteral("trafficFormat"));
        QVERIFY(record && format);
        QCOMPARE(format->currentText(), QStringLiteral("UTF-8"));
        format->setCurrentText(QStringLiteral("HEX")); // 先验证 HEX 日志，再切回文本。

        // 本次实际写入的文件 = 模板基名 + 时间戳 + 扩展名，落在同一目录。
        const auto currentLogFile = [&directory]() -> QString {
            const auto names = QDir(directory.path()).entryList(
                {QStringLiteral("traffic-*.log")}, QDir::Files);
            return names.isEmpty() ? QString() : QDir(directory.path()).filePath(names.first());
        };

        record->setChecked(true);
        const auto logPath = currentLogFile();
        QVERIFY2(!logPath.isEmpty(), "开启记录后应生成本次带时间戳的日志文件");
        QVERIFY(logPath != templatePath); // 绝不覆盖/复用模板路径本身

        Traffic::Message rx;
        rx.direction = Traffic::Direction::Receive;
        rx.source.endpointIndex = 0;
        rx.source.endpointKind = PortDraft::Serial;
        rx.source.endpointId = QUuid::createUuid();
        rx.source.peerAddress = QStringLiteral("127.0.0.1:9001");
        rx.raw = QByteArray::fromHex("0102");
        Traffic::Message tx;
        tx.direction = Traffic::Direction::Transmit;
        tx.origin = Traffic::Origin::Manual;
        tx.source.endpointIndex = 1;
        tx.source.endpointKind = PortDraft::TcpServer;
        tx.source.endpointId = QUuid::createUuid();
        tx.raw = QByteArray::fromHex("FF");
        panel.appendMessages({rx, tx});
        // 批入模是 16ms 异步的，轮询模型行数即等到了写日志的同一拍。
        auto *model = panel.findChild<TrafficModel *>();
        QVERIFY(model);
        QTRY_COMPARE(model->rowCount(), 2);

        QFile log(logPath);
        QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
        const auto hexLines = QString::fromUtf8(log.readAll()).trimmed().split(QLatin1Char('\n'));
        // 元信息头后是固定的列名：日志里的 Peer 字段有明确含义。
        QCOMPARE(hexLines.size(), 4);
        QVERIFY2(hexLines.at(0).startsWith(QStringLiteral("# QQCom")), qPrintable(hexLines.at(0)));
        QVERIFY(hexLines.at(0).contains(QStringLiteral("格式=HEX")));
        QCOMPARE(hexLines.at(1), QStringLiteral("# Time\tPort\tPeer\tDirection\tData"));
        QVERIFY(hexLines.at(2).contains(QStringLiteral("A · 串口")));
        QVERIFY(hexLines.at(2).contains(QStringLiteral("127.0.0.1:9001")));
        QVERIFY(hexLines.at(2).contains(QStringLiteral("RX")));
        QVERIFY(hexLines.at(2).endsWith(QStringLiteral("01 02"))); // HEX 模式记 HEX
        QVERIFY(hexLines.at(3).endsWith(QStringLiteral("FF")));
        log.close();

        // 切换显示格式后新行跟随新模式：所见即所记；并追加一行"格式切换"元信息。
        format->setCurrentText(QStringLiteral("UTF-8"));
        Traffic::Message text;
        text.direction = Traffic::Direction::Receive;
        text.source.endpointIndex = 0;
        text.source.endpointKind = PortDraft::Serial;
        text.source.endpointId = QUuid::createUuid();
        text.raw = "ok";
        panel.appendMessages({text});
        QTRY_COMPARE(model->rowCount(), 3);
        QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
        const auto all = QString::fromUtf8(log.readAll()).trimmed().split(QLatin1Char('\n'));
        // 元信息 + 列名 + 2 数据 + 格式切换行 + 1 数据 = 6 行
        QCOMPARE(all.size(), 6);
        QVERIFY(all.at(4).contains(QStringLiteral("格式切换")));
        QVERIFY(all.at(4).contains(QStringLiteral("UTF-8")));
        QVERIFY(all.at(5).endsWith(QStringLiteral("ok")));
    }

    // 三时刻重开：① 启动恢复记录、② 点「清空」、③ 手动勾选「记录到文件」——每次都
    // 新起一个带时间戳的文件，**绝不覆盖、绝不追加到旧文件**。这条用例用 QTemporaryDir
    // 预置偏好绕开文件对话框，独立验收整条语义（工程师改过的 logRecordingFollowsDisplayFormat
    // 只覆盖了"开启记录后跟随显示格式"这一条路径）。
    void logRecordingReopensOnSessionBoundaries()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // 模板路径 = 用户选定的"目录 + 基名 + 扩展名"，它自身不是任何一次记录的目标。
        // 事先写入垃圾内容：任何一次记录都不许改写或追加到这个文件。
        const auto templatePath = directory.filePath(QStringLiteral("traffic.log"));
        const QByteArray seed("SEED-MUST-NOT-BE-TOUCHED\n");
        {
            QFile file(templatePath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            QCOMPARE(file.write(seed), qint64(seed.size()));
        }

        PreferencesStore store(nullptr, directory.filePath(QStringLiteral("preferences.ini")));
        store.setValue(QStringLiteral("traffic/recordPath"), templatePath);
        // record=true → 第一时刻：启动恢复记录，直接开新文件、不弹对话框。
        store.setValue(QStringLiteral("traffic/record"), true);

        TrafficPanel panel;
        panel.setPreferences(&store);
        auto *record = panel.findChild<QAction *>(QStringLiteral("recordTraffic"));
        auto *format = panel.findChild<QComboBox *>(QStringLiteral("trafficFormat"));
        auto *clear = panel.findChild<QPushButton *>(QStringLiteral("clearTraffic"));
        auto *model = panel.findChild<TrafficModel *>();
        QVERIFY(record && format && clear && model);
        QCOMPARE(record->isChecked(), true); // 启动恢复：记录开关勾上

        // 目录里本次记录产生的全部文件（traffic-<stamp>[-n].log），按名字排序便于比对。
        const auto logFiles = [&directory]() {
            return QDir(directory.path()).entryList(
                {QStringLiteral("traffic-*.log")}, QDir::Files, QDir::Name);
        };
        // 当前正在写的文件：从「记录到文件」的 tooltip 精确读出，比 glob 更不易误判。
        const auto currentLogPath = [&directory, record]() -> QString {
            const QString marker = QStringLiteral("正在写 ");
            const int at = record->toolTip().indexOf(marker);
            if (at < 0) return QString();
            return QDir(directory.path()).filePath(record->toolTip().mid(at + marker.size()).trimmed());
        };
        const auto readLines = [](const QString &path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QStringList();
            QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
            while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
            return lines;
        };

        // ---- C1：启动恢复记录 → 实际文件是 traffic-<stamp>.log；模板不被改写/追加 ----
        const QString firstLog = currentLogPath();
        QVERIFY2(!firstLog.isEmpty(), "启动恢复记录后应已有正在写入的日志文件");
        QVERIFY(firstLog != templatePath);
        QVERIFY(QFileInfo(firstLog).fileName().startsWith(QStringLiteral("traffic-")));
        QVERIFY(QFileInfo(firstLog).fileName().endsWith(QStringLiteral(".log")));
        {
            QFile file(templatePath);
            // 写时是 Text 模式（Windows 下 '\n'→"\r\n"），读回也要 Text 模式才能还原，
            // 否则比的是行尾差异而不是"内容有没有被改写"。
            QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
            QCOMPARE(file.readAll(), seed); // 模板内容原样：它只提供目录+基名+扩展名
        }

        // ---- C2：新文件首行是元信息头 # QQCom ... 格式=... ----
        QStringList lines = readLines(firstLog);
        QVERIFY(!lines.isEmpty());
        QVERIFY2(lines.at(0).startsWith(QStringLiteral("# QQCom ")), qPrintable(lines.at(0)));
        QVERIFY2(lines.at(0).contains(QStringLiteral("格式=")), qPrintable(lines.at(0)));

        // ---- C3：记录若干条 → 数据行数正确、每行 4 个制表符、且没有额外的 # 行 ----
        Traffic::Message rx;
        rx.direction = Traffic::Direction::Receive;
        rx.source.endpointIndex = 0;
        rx.source.endpointKind = PortDraft::Serial;
        rx.source.endpointId = QUuid::createUuid();
        rx.raw = QByteArray::fromHex("0102");
        rx.rawLength = rx.raw.size();
        Traffic::Message tx = rx;
        tx.direction = Traffic::Direction::Transmit;
        tx.source.endpointIndex = 1;
        tx.source.endpointKind = PortDraft::TcpServer;
        tx.source.endpointId = QUuid::createUuid();
        tx.raw = QByteArray::fromHex("FF");
        tx.rawLength = tx.raw.size();
        panel.appendMessages({rx, tx});
        QTRY_COMPARE(model->rowCount(), 2); // 攒批落地即已写日志

        lines = readLines(firstLog);
        QCOMPARE(lines.size(), 4); // 元信息 + 列名 + 2 数据
        int dataLines = 0;
        int metaLines = 0;
        for (const auto &line : lines) {
            if (line.startsWith(QLatin1Char('#'))) {
                ++metaLines;
                continue;
            }
            ++dataLines;
            QCOMPARE(line.count(QLatin1Char('\t')), 4); // 时间\t端点\t对端\t方向\t载荷
        }
        QCOMPARE(dataLines, 2);
        QCOMPARE(metaLines, 2); // 元信息与列名各占一行

        // ---- C4：点「清空视图」是会话边界 → 另起一个文件，从头写、开关保持勾选 ----
        QCOMPARE(logFiles().size(), 1);
        clear->click();
        QCOMPARE(record->isChecked(), true); // 清的是屏幕，记录任务保持勾选
        QCOMPARE(logFiles().size(), 2);      // 换新文件；旧文件原样留在磁盘上
        const QString secondLog = currentLogPath();
        QVERIFY(!secondLog.isEmpty());
        QVERIFY(secondLog != firstLog);
        // 同秒撞名按约定退到 -2。用时间戳是否相同决定断言哪一支，避免跨秒抖动。
        const QString firstBase = QFileInfo(firstLog).completeBaseName();
        const QString secondBase = QFileInfo(secondLog).completeBaseName();
        if (secondBase == firstBase)
            QVERIFY2(secondBase == firstBase + QStringLiteral("-2"), qPrintable(secondBase));
        else
            QVERIFY(secondBase.startsWith(QStringLiteral("traffic-"))); // 跨秒：新时间戳
        const QStringList secondLines = readLines(secondLog);
        QCOMPARE(secondLines.size(), 2); // 元信息 + 列名，清空前的 2 条数据不在其中
        QVERIFY2(secondLines.at(0).startsWith(QStringLiteral("# QQCom ")), qPrintable(secondLines.at(0)));
        QCOMPARE(secondLines.at(1), QStringLiteral("# Time\tPort\tPeer\tDirection\tData"));

        // ---- C5：切换显示格式 → 当前文件追加一行 # 格式切换 → ... ----
        format->setCurrentText(QStringLiteral("HEX"));
        const QStringList afterSwitch = readLines(secondLog);
        QCOMPARE(afterSwitch.size(), 3); // 元信息 + 列名 + 格式切换
        QVERIFY2(afterSwitch.last().startsWith(QStringLiteral("# 格式切换 → ")), qPrintable(afterSwitch.last()));
        QVERIFY(afterSwitch.last().contains(QStringLiteral("HEX")));

        // ================= D：独立反证（不重跑工程师走过的路径） =================
        // D②：同一秒内重开撞名 → 基名-stamp-2.后缀，而不是覆盖。系统时钟无法冻结，
        // 于是把"未来若干秒的基础名"全部用占位文件预先占住：无论触发落在哪一秒，代码
        // 都必须避开已存在的同名文件、退到 -2；且占位文件一个都不许被覆盖。
        {
            QTemporaryDir dir2;
            QVERIFY(dir2.isValid());
            const auto template2 = dir2.filePath(QStringLiteral("traffic.log"));
            PreferencesStore store2(nullptr, dir2.filePath(QStringLiteral("preferences.ini")));
            store2.setValue(QStringLiteral("traffic/recordPath"), template2);
            TrafficPanel panel2;
            panel2.setPreferences(&store2); // record 默认 false：此刻不应有文件
            auto *record2 = panel2.findChild<QAction *>(QStringLiteral("recordTraffic"));
            auto *openLog2 = panel2.findChild<QAction *>(QStringLiteral("openCurrentLog"));
            QVERIFY(record2 && openLog2);
            QVERIFY2(record2->toolTip().indexOf(QStringLiteral("正在写 ")) < 0,
                     "未开启记录时不应声称正在写文件");
            QVERIFY2(!openLog2->isEnabled(), "未记录时不能打开当前日志");

            const QDateTime now = QDateTime::currentDateTime();
            QList<QString> decoys;
            for (int s = 0; s < 12; ++s) {
                const QString stamp = now.addSecs(s).toString(QStringLiteral("yyyyMMdd-HHmmss"));
                const QString path = QDir(dir2.path()).filePath(QStringLiteral("traffic-%1.log").arg(stamp));
                QFile decoy(path);
                QVERIFY(decoy.open(QIODevice::WriteOnly | QIODevice::Text));
                decoy.write("DECOY");
                decoy.close();
                decoys.append(path);
            }
            record2->setChecked(true); // 第三时刻：手动开启记录，m_logPath 非空 → 不弹对话框
            const QString tip = record2->toolTip();
            const int at = tip.indexOf(QStringLiteral("正在写 "));
            QVERIFY2(at >= 0, "开启记录后 tooltip 应显示正在写的文件");
            const QString produced = tip.mid(at + QStringLiteral("正在写 ").size()).trimmed();
            QVERIFY2(produced.endsWith(QStringLiteral("-2.log")), qPrintable(produced));
            QVERIFY(produced.startsWith(QStringLiteral("traffic-")));
            // 每个占位文件都原封不动：撞名走"改名"，不是"覆盖"。
            for (const auto &path : decoys) {
                QFile decoy(path);
                QVERIFY(decoy.open(QIODevice::ReadOnly | QIODevice::Text));
                QCOMPARE(decoy.readAll(), QByteArray("DECOY"));
            }
        }

        // D③：关闭记录开关后再清空视图 → 不产生新文件（记录任务已结束）。
        {
            QTemporaryDir dir3;
            QVERIFY(dir3.isValid());
            const auto template3 = dir3.filePath(QStringLiteral("traffic.log"));
            PreferencesStore store3(nullptr, dir3.filePath(QStringLiteral("preferences.ini")));
            store3.setValue(QStringLiteral("traffic/recordPath"), template3);
            store3.setValue(QStringLiteral("traffic/record"), true);
            TrafficPanel panel3;
            panel3.setPreferences(&store3);
            auto *record3 = panel3.findChild<QAction *>(QStringLiteral("recordTraffic"));
            auto *clear3 = panel3.findChild<QPushButton *>(QStringLiteral("clearTraffic"));
            QVERIFY(record3 && clear3);
            QCOMPARE(record3->isChecked(), true);
            const auto countFiles = [&dir3]() {
                return QDir(dir3.path()).entryList({QStringLiteral("traffic-*.log")}, QDir::Files).size();
            };
            QCOMPARE(countFiles(), 1); // 启动恢复记录：已有一个文件
            record3->setChecked(false); // 关闭记录 → 文件收口
            clear3->click();            // 清空视图：记录已关，不应再开新文件
            QCOMPARE(record3->isChecked(), false);
            QCOMPARE(countFiles(), 1); // 文件数不变
        }
    }

    // 没手动指定目录时，日志写到程序目录下的 log，不再弹保存对话框。
    // 指定过的目录留在偏好里，显示设置里的「日志位置」和「恢复默认位置」可以再改。
    void logDirectoryDefaultsToProgramFolderAndCanChange()
    {
        const QString programLog = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("log"));
        QStringList created;
        struct Guard {
            QStringList *files;
            ~Guard()
            {
                for (const auto &path : *files) QFile::remove(path);
            }
        } guard{&created};
        const auto currentLogPath = [](QAction *record, const QString &directory) {
            const QString marker = QStringLiteral("正在写 ");
            const int at = record->toolTip().indexOf(marker);
            if (at < 0) return QString();
            return QDir(directory).filePath(record->toolTip().mid(at + marker.size()).trimmed());
        };

        QTemporaryDir iniDir;
        QVERIFY(iniDir.isValid());
        PreferencesStore store(nullptr, iniDir.filePath(QStringLiteral("preferences.ini")));
        store.setValue(QStringLiteral("traffic/record"), true);
        TrafficPanel panel;
        panel.setPreferences(&store);
        auto *record = panel.findChild<QAction *>(QStringLiteral("recordTraffic"));
        auto *openLog = panel.findChild<QAction *>(QStringLiteral("openCurrentLog"));
        auto *choose = panel.findChild<QAction *>(QStringLiteral("chooseLogDirectory"));
        auto *reset = panel.findChild<QAction *>(QStringLiteral("resetLogDirectory"));
        QVERIFY(record && openLog && choose && reset);
        QVERIFY(record->isChecked());
        QVERIFY2(openLog->isEnabled(), "正在记录时可以打开当前日志");
        QVERIFY(!reset->isEnabled());
        QCOMPARE(QDir::cleanPath(panel.logDirectory()), QDir::cleanPath(programLog));
        QVERIFY(choose->toolTip().contains(QDir::toNativeSeparators(programLog)));
        const QString first = currentLogPath(record, programLog);
        QVERIFY2(!first.isEmpty(), qPrintable(record->toolTip()));
        QVERIFY(QFileInfo(first).fileName().startsWith(QStringLiteral("QQCom-log-")));
        QVERIFY(QFileInfo(first).fileName().endsWith(QStringLiteral(".txt")));
        QVERIFY(QFile::exists(first));
        QVERIFY(openLog->toolTip().contains(QDir::toNativeSeparators(first)));
        QVERIFY(!QFile::exists(QDir(programLog).filePath(QStringLiteral("QQCom-log.txt"))));
        QVERIFY(store.value(QStringLiteral("traffic/recordPath")).toString().isEmpty());
        created.append(first);

        QTemporaryDir custom;
        QVERIFY(custom.isValid());
        panel.setLogDirectory(custom.path());
        QVERIFY(reset->isEnabled());
        QVERIFY(record->isChecked());
        QCOMPARE(QDir::cleanPath(panel.logDirectory()), QDir::cleanPath(custom.path()));
        QCOMPARE(QDir::cleanPath(store.value(QStringLiteral("traffic/recordPath")).toString()),
                 QDir::cleanPath(QDir(custom.path()).filePath(QStringLiteral("QQCom-log.txt"))));
        const QString moved = currentLogPath(record, custom.path());
        QVERIFY(QFile::exists(moved));
        QVERIFY(QFileInfo(moved).fileName().startsWith(QStringLiteral("QQCom-log-")));
        QVERIFY(QFile::exists(first)); // 换目录另起新文件，旧文件留在原地

        panel.resetLogDirectory();
        QVERIFY(!reset->isEnabled());
        QVERIFY(record->isChecked());
        QCOMPARE(QDir::cleanPath(panel.logDirectory()), QDir::cleanPath(programLog));
        QVERIFY(store.value(QStringLiteral("traffic/recordPath")).toString().isEmpty());
        const QString restored = currentLogPath(record, programLog);
        QVERIFY(restored != first);
        QVERIFY(QFile::exists(restored));
        created.append(restored);

        // 换目录只改文件夹，基名和扩展名留着。
        QTemporaryDir namedIni;
        QTemporaryDir namedDest;
        QVERIFY(namedIni.isValid() && namedDest.isValid());
        PreferencesStore namedStore(nullptr, namedIni.filePath(QStringLiteral("preferences.ini")));
        namedStore.setValue(QStringLiteral("traffic/recordPath"),
                            namedIni.filePath(QStringLiteral("session.log")));
        TrafficPanel named;
        named.setPreferences(&namedStore);
        QVERIFY(named.findChild<QAction *>(QStringLiteral("resetLogDirectory"))->isEnabled());
        named.setLogDirectory(namedDest.path());
        QCOMPARE(QDir::cleanPath(namedStore.value(QStringLiteral("traffic/recordPath")).toString()),
                 QDir::cleanPath(namedDest.filePath(QStringLiteral("session.log"))));
        named.findChild<QAction *>(QStringLiteral("recordTraffic"))->setChecked(true);
        const auto produced = QDir(namedDest.path()).entryList({QStringLiteral("session-*.log")}, QDir::Files);
        QCOMPARE(produced.size(), 1);
    }

    void portControlsShowTheirOwnContent()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        // Both endpoints, all four types, collapsed and expanded: a control that cannot show
        // its own value is a bug wherever it sits in the row.
        for (auto *selector : window.findChildren<PortSelectorWidget *>()) {
            auto *type = selector->findChild<QComboBox *>("portType");
            auto *advanced = selector->findChild<QToolButton *>("advancedToggle");
            QVERIFY(type && advanced);
            for (const bool expanded : {false, true}) {
                if (advanced->isChecked() != expanded) {
                    advanced->click();
                    QTest::qWait(20);
                }
                for (int kind = 0; kind < 4; ++kind) {
                    type->setCurrentIndex(kind);
                    QTest::qWait(20);
                    const auto clipped = controlsThatClipContent(selector);
                    QVERIFY2(clipped.isEmpty(),
                             qPrintable(QString("%1 %2: %3").arg(selector->objectName(),
                                         expanded ? QStringLiteral("expanded")
                                                  : QStringLiteral("collapsed"),
                                         clipped.join("; "))));
                }
            }
        }
    }

    // 端口打开失败（用户点连接的同步失败）必须主动可见（用户报"无法打开/冲突没有弹窗、
    // 也没有日志提示"）：① 监视区出现 Error 行，正文带端点标识 + 失败原因；② 非模态弹窗
    // （模态会独占 GUI 线程，自动化测试也会被卡死），标题带端点名、正文带原因；③ 同一
    // 端点同类错误 3 秒内只弹一次；④ 记录期间状态栏摘要出现当前日志大小，关掉后消失。
    // 打开失败用 TCP 服务器监听一个已被占用的端口制造：listen() 同步失败、立即返回，
    // 不依赖网络栈对"无人监听端口"的延迟（本机对被拒绝连接可能要有秒级 SYN 重试）。
    void portOpenFailureSurfacesErrorRowAndStatus()
    {
        clearEndpointDrafts();
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PreferencesStore store(nullptr, directory.filePath(QStringLiteral("preferences.ini")));
        const auto templatePath = directory.filePath(QStringLiteral("traffic.log"));
        store.setValue(QStringLiteral("traffic/recordPath"), templatePath);
        store.setValue(QStringLiteral("traffic/record"), true);

        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *status = window.findChild<QLabel *>(QStringLiteral("TrafficStatus"));
        auto *record = window.findChild<QAction *>(QStringLiteral("recordTraffic"));
        QVERIFY(panel && text && model && status && record);
        panel->setPreferences(&store); // 把日志落到临时目录，验证"日志里查得到原因"
        QVERIFY(record->isChecked());

        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(b);
        QTcpServer blocker; // 占住端口的"另一个进程"：保持监听，直到本用例结束
        QVERIFY(blocker.listen(QHostAddress::AnyIPv4, 0));
        const quint16 conflictPort = blocker.serverPort();
        b->findChild<QSpinBox *>(QStringLiteral("server.port"))->setValue(conflictPort);
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(action);

        const auto errorRows = [model] {
            int count = 0;
            for (int i = 0; i < model->rowCount(); ++i)
                if (model->record(i).direction == Traffic::Direction::Error) ++count;
            return count;
        };

        action->click(); // 监听一个已被占用的端口 → listen() 同步失败
        QTRY_VERIFY2(text->toPlainText().contains(QStringLiteral("监听")),
            qPrintable(text->toPlainText()));
        QVERIFY2(text->toPlainText().contains(QStringLiteral("失败")),
            qPrintable(text->toPlainText()));
        QVERIFY2(text->toPlainText().contains(QStringLiteral("B · ")),
            qPrintable(QStringLiteral("Error 行应带端点标识：") + text->toPlainText()));
        QCOMPARE(errorRows(), 1);

        // 当前交互通过日志和状态栏报错，不弹窗打断调试。
        QVERIFY(window.findChildren<QMessageBox *>().isEmpty());

        // 状态栏短消息同样带端点标识——读状态栏右端的临时提示格 StatusNote，
        // 不再是 statusBar()->currentMessage()：后者（showMessage）会隐藏 addWidget
        // 挂的统计行，已按项目铁律改用 MainWindow::showStatusNote（见其说明）。
        auto *note = window.findChild<QLabel *>(QStringLiteral("StatusNote"));
        QVERIFY(note);
        QVERIFY2(note->text().contains(QStringLiteral("B · ")), qPrintable(note->text()));
        QCOMPARE(note->property("tone").toString(), QStringLiteral("error"));

        // 按钮仍是「监  听」。原因必须出现在这一行端口上，悬停按钮也能读到。
        auto *failure = b->findChild<QLabel *>(QStringLiteral("openFailure"));
        QVERIFY(failure);
        QVERIFY(failure->isVisibleTo(&window));
        QVERIFY2(failure->text().contains(QStringLiteral("监听"))
                     && failure->text().contains(QStringLiteral("失败")),
            qPrintable(failure->text()));
        QVERIFY2(action->toolTip().contains(QStringLiteral("失败")), qPrintable(action->toolTip()));

        // 纯数据不画错误行，端口行上的原因还在。
        auto *prefix = window.findChild<QAction *>(QStringLiteral("prefix"));
        QVERIFY(prefix);
        prefix->setChecked(false);
        QTRY_VERIFY2(!text->toPlainText().contains(QStringLiteral("失败")),
            qPrintable(text->toPlainText()));
        QVERIFY(failure->isVisibleTo(&window));
        prefix->setChecked(true);
        QTRY_VERIFY2(text->toPlainText().contains(QStringLiteral("失败")),
            qPrintable(text->toPlainText()));

        // 防轰炸：3 秒内同端点同类错误再失败一次，不再弹新窗。
        action->click();
        QTRY_COMPARE(errorRows(), 2);
        QVERIFY(window.findChildren<QMessageBox *>().isEmpty());

        // 日志大小：记录中出现在状态栏摘要里；关掉记录后不再出现。
        QTRY_VERIFY2(status->text().contains(QStringLiteral("日志 ")), qPrintable(status->text()));
        record->setChecked(false);
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        Traffic::Message rx;
        rx.id = 9001;
        rx.timestamp = QDateTime::currentDateTime();
        rx.direction = Traffic::Direction::Receive;
        rx.source.endpointId = endpointId;
        rx.source.endpointIndex = 0;
        rx.source.endpointKind = PortDraft::Serial;
        rx.raw = QByteArray("OK");
        rx.rawLength = 2;
        panel->appendMessage(rx);
        QTRY_VERIFY2(!status->text().contains(QStringLiteral("日志 ")), qPrintable(status->text()));
    }

    // 运行中异常断开（TCP 对端 abort）→ Error 行 + 状态栏播报一句，**不弹窗**
    // （长压测中断开/重连风暴会把弹窗叠成轰炸）。用 localhost TCP Server：QQCom
    // 监听、脚本/QTcpSocket 连上、先发一字节确认已接受，再 abort 掉。
    // 用户主动停止是正常迁移，走 State 行，同样不弹窗。
    void runtimeDisconnectReportsErrorRowWithoutPopup()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(text);
        // 换行/正文断言依赖文本渲染：显式切 UTF-8，不依赖共享配置残留。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));

        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(b); // 默认 TCP 服务器
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();
        b->findChild<QSpinBox *>(QStringLiteral("server.port"))->setValue(port);
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QCOMPARE(action->text(), QStringLiteral("监  听"));

        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("停  止"));

        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, port);
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        client.write("ping");
        QTRY_COMPARE(client.bytesToWrite(), qint64(0));
        // 先确认 QQCom 已接受并收到了这条数据，再 abort——否则连接还没从 backlog
        // 接进来，断开事件无 peer 可挂。
        QTRY_VERIFY2(text->toPlainText().contains(QStringLiteral("ping")),
            qPrintable(text->toPlainText()));

        client.abort(); // 模拟对端异常掉线
        QTRY_VERIFY2(text->toPlainText().contains(QStringLiteral("已断开")),
            qPrintable(text->toPlainText()));
        QVERIFY2(text->toPlainText().contains(QStringLiteral("B · ")),
            qPrintable(QStringLiteral("Error 行应带端点标识：") + text->toPlainText()));
        // 与"打开失败"路径同理：读临时提示格 StatusNote，而不是 statusBar()->currentMessage()。
        auto *note = window.findChild<QLabel *>(QStringLiteral("StatusNote"));
        QVERIFY(note);
        QTRY_VERIFY2(note->text().contains(QStringLiteral("已断开")), qPrintable(note->text()));
        QVERIFY2(note->text().contains(QStringLiteral("B · ")), qPrintable(note->text()));
        // 运行中异常断开不弹窗（与"打开失败"路径的分野）。
        QVERIFY(window.findChildren<QMessageBox *>().isEmpty());

        // 用户主动停止是正常状态迁移：State 行「已关闭」，不弹窗。
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("监  听"));
        QTRY_VERIFY2(text->toPlainText().contains(QStringLiteral("已关闭")),
            qPrintable(text->toPlainText()));
        QVERIFY(window.findChildren<QMessageBox *>().isEmpty());
    }

    // 性能状态必须"随手可得"而不是"卡了才出现"：状态栏那段文字的 tooltip 里
    // 始终有最近 2 秒的量化值（界面迟滞 / 单批处理 / 缓冲 / 日志占用）。
    // 用户要的是"能判断性能"，所以健康时也要能看到数字。
    void performanceStatusIsAlwaysAvailable()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *status = window.findChild<QLabel *>(QStringLiteral("TrafficStatus"));
        QVERIFY(status);
        QTRY_VERIFY2(status->toolTip().contains(QStringLiteral("界面迟滞 ≤")),
            qPrintable(status->toolTip()));
        QVERIFY(status->toolTip().contains(QStringLiteral("单批处理 ≤")));
        QVERIFY(status->toolTip().contains(QStringLiteral("缓冲 ")));
        QVERIFY(status->toolTip().contains(QStringLiteral("日志 ")));
    }

    // 语义配色：方向令牌按类型分色（RX 饱和绿 / TX 主蓝 / 状态紫 / 信息青 / 警告琥珀 / 错误红），
    // 错误与警告整行同色——这是"一眼分辨消息类型"的落点，改动必须被这里挡住。
    void directionTokensCarrySemanticColors()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(text && model);
        // 正文断言依赖文本渲染：显式切 UTF-8，不依赖共享配置里的残留格式。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        // 默认字节数会插在方向和箭头之间，HEX 对照会把五种方向拆成隔行。本用例只看方向色。
        window.findChild<QAction *>(QStringLiteral("prefixLength"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(false);
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        const auto make = [endpointId](quint64 id, Traffic::Direction direction, const char *body) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = direction;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray(body);
            message.rawLength = message.raw.size();
            return message;
        };
        panel->appendMessages({make(1, Traffic::Direction::Receive, "rx-body"),
            make(2, Traffic::Direction::Transmit, "tx-body"),
            make(3, Traffic::Direction::State, "state-body"),
            make(4, Traffic::Direction::Warning, "warning-body"),
            make(5, Traffic::Direction::Error, "error-body"),
            make(6, Traffic::Direction::Info, "info-body")});
        QTRY_COMPARE(model->rowCount(), 6);
        const auto fragmentColor = [text](int blockIndex, const QString &needle, bool exact = false) {
            const QTextBlock block = text->document()->findBlockByNumber(blockIndex);
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const bool hit = exact ? it.fragment().text() == needle
                                       : it.fragment().text().contains(needle);
                if (hit) return it.fragment().charFormat().foreground().color();
            }
            return QColor();
        };
        QCOMPARE(fragmentColor(0, QStringLiteral("[A] "), true), QColor(0xB4, 0x53, 0x09));
        QCOMPARE(fragmentColor(0, QStringLiteral("RX")), QColor(0x04, 0x6C, 0x4E));
        // 串口没有对端，箭头仍要画出来，否则只剩绿色 RX，看不出收还是发。
        QVERIFY(text->document()->findBlockByNumber(0).text().contains(QStringLiteral("[A] RX <<--")));
        QVERIFY(text->document()->findBlockByNumber(1).text().contains(QStringLiteral("[A] TX -->>")));
        QCOMPARE(fragmentColor(1, QStringLiteral("TX")), QColor(0x1D, 0x4E, 0xD8));
        QCOMPARE(fragmentColor(2, QStringLiteral("S")), QColor(0x6D, 0x28, 0xD9));
        QCOMPARE(fragmentColor(3, QStringLiteral("W")), QColor(0x92, 0x40, 0x0E));
        QCOMPARE(fragmentColor(4, QStringLiteral("E")), QColor(0xC6, 0x28, 0x28));
        QCOMPARE(fragmentColor(5, QStringLiteral("I")), QColor(0x0C, 0x4A, 0x6E));
        // 数据行的正文保持近黑（数据是主体）；错误/警告整行随类型着色。
        QCOMPARE(fragmentColor(0, QStringLiteral("rx-body")), QColor(0x1C, 0x24, 0x30));
        QCOMPARE(fragmentColor(3, QStringLiteral("warning-body")), QColor(0x92, 0x40, 0x0E));
        QCOMPARE(fragmentColor(4, QStringLiteral("error-body")), QColor(0xC6, 0x28, 0x28));
        // 前缀 DemiBold（用户钦定"前缀加粗 + 与正文同色，靠字重区分"）：时间这类
        // 元信息与方向令牌必须同为 DemiBold，且元信息用正文色 #1C2430。这里钉的是字重
        // 与元色，防止以后有人把 Bold 当"够粗"、或把前缀退回更浅的灰。
        // 行号不在正文里，端口色在左侧槽上。
        const auto fragmentFormat = [text](int blockIndex, const QString &needle, bool exact = false) {
            const QTextBlock block = text->document()->findBlockByNumber(blockIndex);
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const bool hit = exact ? it.fragment().text() == needle
                                       : it.fragment().text().contains(needle);
                if (hit) return it.fragment().charFormat();
            }
            return QTextCharFormat();
        };
        const QString first = text->document()->firstBlock().text();
        const int stampAt = first.indexOf(QRegularExpression(QStringLiteral(R"(\d{2}:\d{2}:\d{2})")));
        QVERIFY(stampAt >= 0);
        const auto metaFormat = fragmentFormat(0, first.mid(stampAt, 12));
        QCOMPARE(metaFormat.fontWeight(), int(QFont::DemiBold));
        QCOMPARE(metaFormat.foreground().color(), QColor(0x1C, 0x24, 0x30));
        QCOMPARE(text->lineEndpointAt(0), 0);
        QCOMPARE(fragmentFormat(0, QStringLiteral("[A] "), true).fontWeight(), int(QFont::DemiBold));
        QCOMPARE(fragmentFormat(0, QStringLiteral("RX")).fontWeight(), int(QFont::DemiBold));
    }

    void endpointParameterColumnsAlign()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        auto *a = window.findChild<PortSelectorWidget *>("endpointA");
        auto *b = window.findChild<PortSelectorWidget *>("endpointB");
        QVERIFY(a && b);
        const auto leftInWindow = [&window](QWidget *w) {
            return w->mapTo(&window, QPoint(0, 0)).x();
        };
        const auto rightInWindow = [&window](QWidget *w) {
            return w->mapTo(&window, QPoint(w->width(), 0)).x();
        };
        // 主字段左缘仍对齐。尾部框按当前文字收紧，所以 115200 比 9000 宽，右缘仍对齐。
        auto *aMain = a->findChild<QComboBox *>("serial.port");
        auto *bMain = b->findChild<QComboBox *>("server.address");
        auto *aTrail = a->findChild<QComboBox *>("serial.baud");
        auto *bTrail = b->findChild<QSpinBox *>("server.port");
        QVERIFY(aMain && bMain && aTrail && bTrail);
        QCOMPARE(aTrail->currentText(), QStringLiteral("115200"));
        QCOMPARE(bTrail->text(), QStringLiteral("9000"));
        // 主字段左缘仍对齐。尾部框按各自文字收紧，所以两行不必同宽。
        // B 是 TCP 服务端，不显示「自动恢复」，参数区更宽，右缘也不强求重合。
        QCOMPARE(leftInWindow(aMain), leftInWindow(bMain));
        QVERIFY2(aTrail->width() > bTrail->width(),
            qPrintable(QStringLiteral("115200 的框应宽于 9000：波特率 %1，监听端口 %2")
                           .arg(aTrail->width()).arg(bTrail->width())));
    }

    // 串口行只有下拉框，网络行还有数字框。数字框不能因为内嵌编辑框再垫一层
    // 上边距而把整行撑高，四种类型的端口条和控件顶缘必须一样。
    void portRowsShareHeightAcrossKinds()
    {
        PortSelectorWidget selector;
        selector.setAttribute(Qt::WA_DontShowOnScreen);
        selector.resize(1100, 80);
        selector.show();
        QTest::qWait(30);
        auto *type = selector.findChild<QComboBox *>(QStringLiteral("portType"));
        QVERIFY(type);

        const auto band = [&selector, type] {
            int top = -1;
            int height = -1;
            const auto take = [&](QWidget *field) {
                if (!field || field == type || !field->isVisible()) return;
                const int y = field->mapTo(&selector, QPoint(0, 0)).y();
                top = top < 0 ? y : qMin(top, y);
                height = qMax(height, field->height());
            };
            for (auto *combo : selector.findChildren<QComboBox *>()) take(combo);
            for (auto *spin : selector.findChildren<QSpinBox *>()) take(spin);
            return qMakePair(top, height);
        };

        type->setCurrentIndex(0);
        QApplication::processEvents();
        const int barHeight = selector.height();
        const auto serial = band();
        QVERIFY(serial.first >= 0);
        QVERIFY(serial.second > 0);
        for (int kind = 1; kind < type->count(); ++kind) {
            type->setCurrentIndex(kind);
            QApplication::processEvents();
            const auto current = band();
            QVERIFY2(selector.height() == barHeight,
                qPrintable(QStringLiteral("类型 %1 行高 %2，串口 %3")
                               .arg(kind).arg(selector.height()).arg(barHeight)));
            QVERIFY2(current.first == serial.first,
                qPrintable(QStringLiteral("类型 %1 控件顶缘 %2，串口 %3")
                               .arg(kind).arg(current.first).arg(serial.first)));
            QVERIFY2(current.second == serial.second,
                qPrintable(QStringLiteral("类型 %1 控件高 %2，串口 %3")
                               .arg(kind).arg(current.second).arg(serial.second)));
        }
    }

    void baudAndPortFieldsHugTheirText()
    {
        PortSelectorWidget selector;
        selector.setAttribute(Qt::WA_DontShowOnScreen);
        selector.resize(900, 80);
        selector.show();
        QTest::qWait(20);

        const auto editSlack = [](QWidget *field, const QString &text, bool combo) {
            const int textWidth = field->fontMetrics().horizontalAdvance(text);
            int editWidth = 0;
            if (combo) {
                auto *box = static_cast<QComboBox *>(field);
                QStyleOptionComboBox option;
                option.initFrom(box);
                option.editable = true;
                option.currentText = text;
                option.rect = box->rect();
                editWidth = box->style()->subControlRect(
                    QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, box).width();
            } else {
                auto *box = static_cast<QAbstractSpinBox *>(field);
                QStyleOptionSpinBox option;
                option.initFrom(box);
                option.buttonSymbols = box->buttonSymbols();
                option.rect = box->rect();
                editWidth = box->style()->subControlRect(
                    QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxEditField, box).width();
            }
            return qMakePair(editWidth - textWidth, field->width());
        };
        const auto hugs = [&](QWidget *field, const QString &text, bool combo) {
            const auto slack = editSlack(field, text, combo);
            // 编辑区盖住文字，两侧余量只留几个像素，不再按量程或 10 位上限留白。
            if (slack.first < 0 || slack.first > 8)
                return QStringLiteral("「%1」编辑区余量 %2（框宽 %3），应在 0～8")
                    .arg(text).arg(slack.first).arg(slack.second);
            return QString();
        };

        auto *baud = selector.findChild<QComboBox *>(QStringLiteral("serial.baud"));
        auto *listen = selector.findChild<QSpinBox *>(QStringLiteral("server.port"));
        auto *remote = selector.findChild<QSpinBox *>(QStringLiteral("client.port"));
        auto *udpPort = selector.findChild<QSpinBox *>(QStringLiteral("udp.port"));
        auto *targetPort = selector.findChild<QSpinBox *>(QStringLiteral("udp.targetPort"));
        QVERIFY(baud && listen && remote && udpPort && targetPort);

        QCOMPARE(baud->currentText(), QStringLiteral("115200"));
        const QString baudFit = hugs(baud, baud->currentText(), true);
        QVERIFY2(baudFit.isEmpty(), qPrintable(baudFit));
        QCOMPARE(listen->text(), QStringLiteral("9000"));
        QCOMPARE(remote->text(), QStringLiteral("9000"));
        const QString listenFit = hugs(listen, listen->text(), false);
        QVERIFY2(listenFit.isEmpty(), qPrintable(listenFit));
        const QString remoteFit = hugs(remote, remote->text(), false);
        QVERIFY2(remoteFit.isEmpty(), qPrintable(remoteFit));
        QCOMPARE(udpPort->text(), QStringLiteral("0"));
        QCOMPARE(targetPort->text(), QStringLiteral("9002"));
        const QString udpFit = hugs(udpPort, udpPort->text(), false);
        QVERIFY2(udpFit.isEmpty(), qPrintable(udpFit));
        const QString targetFit = hugs(targetPort, targetPort->text(), false);
        QVERIFY2(targetFit.isEmpty(), qPrintable(targetFit));
        QVERIFY(udpPort->width() < targetPort->width());

        const int shortBaud = baud->minimumWidth();
        baud->setEditText(QStringLiteral("2147483647"));
        QVERIFY2(baud->minimumWidth() > shortBaud,
            qPrintable(QStringLiteral("10 位波特率应把框撑开：%1 -> %2，当前宽 %3")
                           .arg(shortBaud).arg(baud->minimumWidth()).arg(baud->width())));
        const QString longBaudFit = hugs(baud, QStringLiteral("2147483647"), true);
        QVERIFY2(longBaudFit.isEmpty(), qPrintable(longBaudFit));

        baud->setEditText(QStringLiteral("96"));
        QVERIFY2(baud->minimumWidth() < shortBaud,
            qPrintable(QStringLiteral("短波特率应把框收回：%1 -> %2")
                           .arg(shortBaud).arg(baud->minimumWidth())));
        const QString tinyBaudFit = hugs(baud, QStringLiteral("96"), true);
        QVERIFY2(tinyBaudFit.isEmpty(), qPrintable(tinyBaudFit));

        const int port9000 = listen->minimumWidth();
        listen->setValue(65535);
        QVERIFY2(listen->minimumWidth() > port9000,
            qPrintable(QStringLiteral("65535 应比 9000 更宽：%1 -> %2，文字「%3」宽 %4")
                           .arg(port9000).arg(listen->minimumWidth()).arg(listen->text())
                           .arg(listen->fontMetrics().horizontalAdvance(listen->text()))));
        const QString maxPortFit = hugs(listen, listen->text(), false);
        QVERIFY2(maxPortFit.isEmpty(), qPrintable(maxPortFit));
        listen->setValue(1);
        QVERIFY2(listen->minimumWidth() < port9000,
            qPrintable(QStringLiteral("1 应比 9000 更窄：%1 -> %2，文字「%3」")
                           .arg(port9000).arg(listen->minimumWidth()).arg(listen->text())));
        const QString minPortFit = hugs(listen, listen->text(), false);
        QVERIFY2(minPortFit.isEmpty(), qPrintable(minPortFit));
    }

    void serialLabelsDistributeInColumn()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        auto *a = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointA"));
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(a && b);
        auto *serialLabel = a->findChild<QLabel *>(QStringLiteral("serial.portLabel"));
        auto *baudLabel = a->findChild<QLabel *>(QStringLiteral("serial.baudLabel"));
        auto *nicLabel = b->findChild<QLabel *>(QStringLiteral("server.addressLabel"));
        QVERIFY(serialLabel && baudLabel && nicLabel);
        // 字不变，只把短标签在列宽里拉开，左右缘和同列的长标签对齐。
        QCOMPARE(serialLabel->text(), QStringLiteral("串口"));
        QCOMPARE(baudLabel->text(), QStringLiteral("波特率"));
        QCOMPARE(serialLabel->width(), nicLabel->width());
        struct Ink {
            int left = -1;
            int right = -1;
            int width = 0;
            QColor darkest = QColor(255, 255, 255);
        };
        const auto measure = [](QLabel *label) {
            Ink ink;
            const QImage image = label->grab().toImage();
            ink.width = image.width();
            if (image.isNull() || image.width() <= 0) return ink;
            ink.left = image.width();
            int bestSum = 255 * 3;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const QColor pixel = image.pixelColor(x, y);
                    if (pixel.alpha() >= 200) {
                        const int sum = pixel.red() + pixel.green() + pixel.blue();
                        if (sum < bestSum) {
                            bestSum = sum;
                            ink.darkest = pixel;
                        }
                    }
                    if (pixel.alpha() < 40 || pixel.lightness() > 180) continue;
                    ink.left = qMin(ink.left, x);
                    ink.right = qMax(ink.right, x);
                }
            }
            return ink;
        };
        const auto spanError = [](const Ink &ink, const QString &what) {
            const int slack = qMax(8, ink.width / 8);
            if (ink.width <= 0 || ink.left < 0 || ink.left > slack)
                return QStringLiteral("%1 左缘应贴着列首：左 %2，宽 %3")
                    .arg(what).arg(ink.left).arg(ink.width);
            if (ink.right < ink.width - 1 - slack)
                return QStringLiteral("%1 右缘应贴着列尾：右 %2，宽 %3")
                    .arg(what).arg(ink.right).arg(ink.width);
            return QString();
        };
        const Ink serialInk = measure(serialLabel);
        const Ink baudInk = measure(baudLabel);
        const Ink nicInk = measure(nicLabel);
        const QString serialError = spanError(serialInk, QStringLiteral("串口"));
        const QString baudError = spanError(baudInk, QStringLiteral("波特率"));
        const QString nicError = spanError(nicInk, QStringLiteral("监听网卡"));
        QVERIFY2(serialError.isEmpty(), qPrintable(serialError));
        QVERIFY2(baudError.isEmpty(), qPrintable(baudError));
        QVERIFY2(nicError.isEmpty(), qPrintable(nicError));
        QVERIFY2(qAbs(serialInk.darkest.red() - nicInk.darkest.red()) < 40
                     && qAbs(serialInk.darkest.green() - nicInk.darkest.green()) < 40
                     && qAbs(serialInk.darkest.blue() - nicInk.darkest.blue()) < 40,
            qPrintable(QStringLiteral("串口字色应与监听网卡一致：%1 vs %2")
                           .arg(serialInk.darkest.name(), nicInk.darkest.name())));
    }

    void sendPreviewRejectsInvalidInput()
    {
        clearEndpointDrafts();
        MainWindow window;
        auto *editor = window.findChild<QPlainTextEdit *>("sendEditor");
        QVERIFY(editor);
        // 存为命令随快捷命令页一起撤下：命令列表回归前，发送区不留没有落点的按钮。
        QVERIFY(!window.findChild<QPushButton *>("saveCommand"));
        // 预览阶段发送按钮恒为禁用（尚未连接端口），与输入合法性无关。
        for (auto *button : window.findChildren<QPushButton *>("PrimaryButton"))
            QVERIFY(!button->isEnabled());
        auto *preview = window.findChild<QLabel *>("payloadPreview");
        QVERIFY(preview);
        // 字节数与预览数据**已合并进同一个标签**（富文本），所以都从 preview->text() 上看；
        // 字节数在 span 里，用 ">2B<" 这种带标记边界的写法定位，避免误命中 HEX 里恰好出现的 "2B"。
        editor->setPlainText("01 GG");
        QVERIFY2(preview->text().contains(QStringLiteral("输入错误")),
            qPrintable(QStringLiteral("非法输入应显示字节数为「输入错误」：") + preview->text()));
        QVERIFY(preview->text().contains(QStringLiteral("无效 HEX"))); // 错误原因展示在预览位置
        editor->setPlainText("00 FF");
        QVERIFY2(preview->text().contains(QStringLiteral(">2B<")),
            qPrintable(QStringLiteral("字节数应为 2B：") + preview->text()));
        QVERIFY(preview->text().contains(QStringLiteral("00 FF")));
        // 格式/末尾切换的接线：预览字节数必须跟随变化。
        editor->setPlainText("01 03 00 00 00 02 C4 0B");
        QVERIFY2(preview->text().contains(QStringLiteral(">8B<")), qPrintable(preview->text()));
        window.findChild<QComboBox *>("sendSuffix")->setCurrentIndex(3); // CRLF
        QVERIFY2(preview->text().contains(QStringLiteral(">10B<")), qPrintable(preview->text()));
        window.findChild<QComboBox *>("sendSuffix")->setCurrentIndex(0);
        window.findChild<QComboBox *>("sendFormat")->setCurrentIndex(3); // UTF-8：正文按文本编码
        QVERIFY2(preview->text().contains(QStringLiteral(">23B<")), qPrintable(preview->text()));
        window.findChild<QComboBox *>("sendFormat")->setCurrentIndex(0); // 恢复默认，不留跨用例状态
    }

    void sendTargetsMatchDisplayToggles()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        // 手动发送、发送文件、接收文件分成三个标签。快捷指令不再放进发送区。
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("sendModeTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->tabText(0), QStringLiteral("手动发送"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("发送文件"));
        QCOMPARE(tabs->tabText(2), QStringLiteral("接收文件"));
        QCOMPARE(window.findChildren<QTabWidget *>().size(), 1);
        auto *panel = window.findChild<SendPanel *>();
        auto *portA = window.findChild<QPushButton *>("showPortA");
        QVERIFY(panel && portA);
        // 目标只有 A、B 两个可同时选中的开关，没有单独的 AB。
        // 手动发送页的格式/末尾仍是那两个下拉框；文件页另有模式和前后指令下拉框。
        QCOMPARE(panel->findChildren<QComboBox *>(QStringLiteral("sendFormat")).size(), 1);
        QCOMPARE(panel->findChildren<QComboBox *>(QStringLiteral("sendSuffix")).size(), 1);
        // 发送区格式下拉框逐项来自共享 PayloadCodec（顺序与名称的权威断言在 renderFormats）。
        auto *sendFormat = window.findChild<QComboBox *>("sendFormat");
        QVERIFY(sendFormat);
        const auto formats = PayloadCodec::formats();
        QCOMPARE(sendFormat->count(), formats.size());
        for (int i = 0; i < formats.size(); ++i)
            QCOMPARE(sendFormat->itemText(i), PayloadCodec::formatName(formats.at(i)));
        QVERIFY(panel->findChildren<QRadioButton *>().isEmpty());
        QVERIFY(!window.findChild<QPushButton *>("sendTargetAB"));
        const QList<QPushButton *> targets{window.findChild<QPushButton *>("sendTargetA"),
                                           window.findChild<QPushButton *>("sendTargetB")};
        for (auto *button : targets) {
            QVERIFY(button && button->isCheckable());
            // 与数据显示区端点开关同款形态：同为可勾选按钮、同样高度，
            // 勾选态因此共用同一条 QPushButton:checked 样式，不受系统深色调色板影响。
            QCOMPARE(button->sizeHint().height(), portA->sizeHint().height());
        }
        // A、B 各自独立：可以同时选中，两个都选中的掩码是 3。
        // 不能把最后一个也取消，发送必须有目标。
        QVERIFY(targets.at(0)->isChecked());
        QVERIFY(!targets.at(1)->isChecked());
        QCOMPARE(panel->targetMask(), 1);
        targets.at(1)->click();
        QVERIFY(targets.at(0)->isChecked() && targets.at(1)->isChecked());
        QCOMPARE(panel->targetMask(), 3);
        targets.at(0)->click();
        QVERIFY(!targets.at(0)->isChecked() && targets.at(1)->isChecked());
        QCOMPARE(panel->targetMask(), 2);
        targets.at(1)->click();
        QVERIFY(targets.at(1)->isChecked());
        QCOMPARE(panel->targetMask(), 2);
        // 新布局（用户钦定"发送按钮放到发送框的右边，定时发送、编码选择也集成到右边"）：
        // 编码 / 定时 / 目标 / 发送全部落在正文**右侧一列**，不再铺在正文下方。
        auto *editor = window.findChild<QPlainTextEdit *>("sendEditor");
        QVERIFY(editor);
        const int editorRight = editor->mapTo(&window, QPoint()).x() + editor->width();
        for (const QString &name : {"sendFormat", "sendSuffix"}) {
            auto *box = window.findChild<QComboBox *>(name);
            QVERIFY2(box && box->isVisibleTo(&window), qPrintable(name));
            QVERIFY2(box->mapTo(&window, QPoint()).x() >= editorRight,
                qPrintable(QStringLiteral("%1 应落在正文右侧：x=%2，正文右缘=%3")
                               .arg(name)
                               .arg(box->mapTo(&window, QPoint()).x())
                               .arg(editorRight)));
        }
        // 回车发送与定时发送同属右侧列。默认关，回车仍是换行。
        auto *enterToSend = window.findChild<QPushButton *>(QStringLiteral("enterToSend"));
        auto *enterHint = window.findChild<QLabel *>(QStringLiteral("enterToSendHint"));
        QVERIFY(enterToSend && enterHint);
        QVERIFY(!enterToSend->isChecked());
        QVERIFY(!enterHint->isVisibleTo(&window));
        QVERIFY2(enterToSend->mapTo(&window, QPoint()).x() >= editorRight,
            qPrintable(QStringLiteral("回车发送应在正文右侧：x=%1，正文右缘=%2")
                           .arg(enterToSend->mapTo(&window, QPoint()).x())
                           .arg(editorRight)));
        // 自动清空跟在回车发送右侧、同一行。没开回车发送时置灰，不另占一行。
        auto *clearOnEnter = window.findChild<QPushButton *>(QStringLiteral("clearOnEnter"));
        auto *repeat = window.findChild<QCheckBox *>(QStringLiteral("repeatSending"));
        QVERIFY(clearOnEnter && repeat);
        QVERIFY(!clearOnEnter->isChecked());
        QVERIFY(!clearOnEnter->isEnabled());
        const int enterMid = enterToSend->mapTo(&window, QPoint()).y() + enterToSend->height() / 2;
        const int clearMid = clearOnEnter->mapTo(&window, QPoint()).y() + clearOnEnter->height() / 2;
        QVERIFY2(qAbs(enterMid - clearMid) <= 2,
            qPrintable(QStringLiteral("自动清空应与回车发送同一行：%1 vs %2").arg(clearMid).arg(enterMid)));
        QVERIFY2(clearOnEnter->mapTo(&window, QPoint()).x()
                     >= enterToSend->mapTo(&window, QPoint()).x() + enterToSend->width(),
            qPrintable(QStringLiteral("自动清空应在回车发送右侧")));
        QCOMPARE(enterToSend->text(), QStringLiteral("回车发送"));
        QCOMPARE(clearOnEnter->text(), QStringLiteral("回车清空"));
        QCOMPARE(repeat->text(), QStringLiteral("定时"));
        QVERIFY(enterToSend->isCheckable());
        QVERIFY(clearOnEnter->isCheckable());
        const int repeatMid = repeat->mapTo(&window, QPoint()).y() + repeat->height() / 2;
        QVERIFY2(qAbs(repeatMid - enterMid) <= 2,
            qPrintable(QStringLiteral("定时应与回车发送同一行：%1 vs %2").arg(repeatMid).arg(enterMid)));
        enterToSend->setChecked(true);
        QVERIFY(clearOnEnter->isEnabled());
        QVERIFY(enterHint->isVisibleTo(&window));

        QVERIFY(enterHint->text().contains(QStringLiteral("Alt+回车")));
        QVERIFY(editor->toolTip().contains(QStringLiteral("Alt+回车")));
        auto *previewChip = window.findChild<QLabel *>(QStringLiteral("payloadPreview"));
        QVERIFY(previewChip);
        // 提示和 HEX 预览各占一块底色，中间留出空隙，不能再贴成同一段灰字。
        QVERIFY(enterHint->mapTo(&window, QPoint()).x()
            >= previewChip->mapTo(&window, QPoint()).x() + previewChip->width());
        const auto chipFill = [](QWidget *chip) {
            const QImage image = chip->grab().toImage();
            return image.pixelColor(qMin(2, image.width() - 1), image.height() / 2);
        };
        const QColor hintFill = chipFill(enterHint);
        const QColor previewFill = chipFill(previewChip);
        QVERIFY(hintFill != previewFill);
        enterToSend->setChecked(false);
        QVERIFY(!enterHint->isVisibleTo(&window));
        // 定时发送同属右侧列。
        QVERIFY(repeat);
        QVERIFY2(repeat->mapTo(&window, QPoint()).x() >= editorRight,
            qPrintable(QStringLiteral("定时发送应在正文右侧：x=%1，正文右缘=%2")
                           .arg(repeat->mapTo(&window, QPoint()).x())
                           .arg(editorRight)));
        // 发送按钮在正文右侧；目标 A/B 与它同一行、且位于它左侧
        //（这一排的语义就是"发给谁 + 发出去"，拆行会读不出关系）。
        auto *send = window.findChild<QPushButton *>("PrimaryButton");
        QVERIFY(send);
        QVERIFY2(send->mapTo(&window, QPoint()).x() >= editorRight,
            qPrintable(QStringLiteral("发送按钮应在正文右侧：x=%1，正文右缘=%2")
                           .arg(send->mapTo(&window, QPoint()).x())
                           .arg(editorRight)));
        const int sendMid = send->mapTo(&window, QPoint()).y() + send->height() / 2;
        const int targetMid = targets.at(0)->mapTo(&window, QPoint()).y() + targets.at(0)->height() / 2;
        QVERIFY2(qAbs(targetMid - sendMid) <= 2,
                 qPrintable(QStringLiteral("目标与发送应在同一行：目标中心 %1 vs 发送中心 %2")
                                .arg(targetMid).arg(sendMid)));
        QVERIFY2(targets.at(1)->mapTo(&window, QPoint()).x() + targets.at(1)->width()
                     <= send->mapTo(&window, QPoint()).x(),
                 qPrintable(QStringLiteral("B right edge %1 vs send left %2")
                                .arg(targets.at(1)->mapTo(&window, QPoint()).x() + targets.at(1)->width())
                                .arg(send->mapTo(&window, QPoint()).x())));
        auto *interval = window.findChild<QSpinBox *>(QStringLiteral("repeatInterval"));
        QVERIFY(interval);
        QVERIFY2(repeat->mapTo(&window, QPoint()).x() + repeat->width()
                     <= interval->mapTo(&window, QPoint()).x(),
            qPrintable(QStringLiteral("间隔应在定时右侧")));
        QVERIFY2(interval->mapTo(&window, QPoint()).x() + interval->width()
                     <= enterToSend->mapTo(&window, QPoint()).x(),
            qPrintable(QStringLiteral("回车发送应在间隔右侧")));
        const auto intervalFits = [](const QSpinBox *box) {
            const int textWidth = box->fontMetrics().horizontalAdvance(box->text());
            return box->width() >= textWidth + 12 && box->width() <= textWidth + 20;
        };
        QVERIFY2(intervalFits(interval),
            qPrintable(QStringLiteral("间隔框应贴着当前文字：宽 %1，文字 %2，sizeHint %3")
                           .arg(interval->width())
                           .arg(interval->fontMetrics().horizontalAdvance(interval->text()))
                           .arg(interval->sizeHint().width())));
        QVERIFY2(interval->width() < interval->sizeHint().width(),
            qPrintable(QStringLiteral("间隔框不应按量程最大值撑宽：宽 %1，sizeHint %2")
                           .arg(interval->width()).arg(interval->sizeHint().width())));
        interval->setValue(3600000);
        QVERIFY2(intervalFits(interval),
            qPrintable(QStringLiteral("加长后间隔框仍应贴着文字：宽 %1，文字 %2")
                           .arg(interval->width())
                           .arg(interval->fontMetrics().horizontalAdvance(interval->text()))));
        interval->setValue(1000);
        // 提示标签的高度不含 1px 边框，外缘才和按钮齐平。
        QCOMPARE(enterHint->height() + 2, send->height());
        QCOMPARE(previewChip->height(), send->height());
        const int hintTop = enterHint->mapTo(&window, QPoint()).y() - 1;
        const int hintBottom = hintTop + enterHint->height() + 2;
        QCOMPARE(hintTop, send->mapTo(&window, QPoint()).y());
        QCOMPARE(hintBottom, send->mapTo(&window, QPoint()).y() + send->height());
        QCOMPARE(previewChip->mapTo(&window, QPoint()).y(), send->mapTo(&window, QPoint()).y());
        QCOMPARE(send->height(), targets.at(0)->height());
        QCOMPARE(targets.at(0)->height(), targets.at(1)->height());
        QVERIFY2(send->width() > targets.at(0)->width(),
                 qPrintable(QStringLiteral("发送按钮应比目标开关更宽：发送 %1，目标 %2")
                                .arg(send->width()).arg(targets.at(0)->width())));
        // 格式、末尾、间隔、A/B、发送同高。末尾与发送按钮右缘对齐。
        // 三行之间的空隙相等，首行贴着正文上沿，末行与预览同一中线。
        // 间隔框跟在「定时」后面，不再与末尾下拉框同宽、也不再共用右缘。
        auto *suffixBox = window.findChild<QComboBox *>(QStringLiteral("sendSuffix"));
        auto *intervalBox = window.findChild<QSpinBox *>(QStringLiteral("repeatInterval"));
        auto *preview = window.findChild<QLabel *>(QStringLiteral("payloadPreview"));
        QVERIFY(suffixBox && intervalBox && preview);
        const auto centerY = [](const QWidget *widget, const QWidget *origin) {
            return widget->mapTo(origin, QPoint()).y() + widget->height() / 2;
        };
        const auto edgeRight = [](const QWidget *widget, const QWidget *origin) {
            return widget->mapTo(origin, QPoint()).x() + widget->width();
        };
        QCOMPARE(sendFormat->height(), send->height());
        QCOMPARE(suffixBox->height(), send->height());
        QCOMPARE(intervalBox->height(), send->height());
        QVERIFY2(qAbs(sendFormat->mapTo(&window, QPoint()).y() - editor->mapTo(&window, QPoint()).y()) <= 2,
            "格式行应贴着正文上沿");
        QVERIFY2(qAbs(centerY(preview, &window) - centerY(send, &window)) <= 2,
            "预览应与发送按钮同一高度");
        QVERIFY2(qAbs(edgeRight(suffixBox, &window) - edgeRight(send, &window)) <= 2,
            "末尾下拉框应与发送按钮右缘对齐");
        const int formatGap = enterToSend->mapTo(&window, QPoint()).y()
            - (sendFormat->mapTo(&window, QPoint()).y() + sendFormat->height());
        const int actionGap = send->mapTo(&window, QPoint()).y()
            - (enterToSend->mapTo(&window, QPoint()).y() + enterToSend->height());
        QVERIFY2(qAbs(formatGap - actionGap) <= 2,
            qPrintable(QStringLiteral("右栏行距不均：格式到回车 %1，回车到发送 %2")
                           .arg(formatGap).arg(actionGap)));
        // 三页都是内容撑开的高度，而且一样高。切换标签不再改发送区高度。
        auto *manualPage = window.findChild<QWidget *>(QStringLiteral("manualSendPage"));
        auto *fileSendPage = window.findChild<QWidget *>(QStringLiteral("fileSendPage"));
        auto *fileReceivePage = window.findChild<QWidget *>(QStringLiteral("fileReceivePage"));
        QVERIFY(manualPage && fileSendPage && fileReceivePage);
        QVERIFY2(qAbs(manualPage->sizeHint().height() - fileSendPage->sizeHint().height()) <= 2,
            qPrintable(QStringLiteral("手动发送 %1，发送文件 %2")
                           .arg(manualPage->sizeHint().height())
                           .arg(fileSendPage->sizeHint().height())));
        QVERIFY2(qAbs(manualPage->sizeHint().height() - fileReceivePage->sizeHint().height()) <= 2,
            qPrintable(QStringLiteral("手动发送 %1，接收文件 %2")
                           .arg(manualPage->sizeHint().height())
                           .arg(fileReceivePage->sizeHint().height())));
        const int paneH = panel->height();
        const int preferred = panel->preferredHeight();
        const auto artifactDir = qEnvironmentVariable("QQCOM_UI_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QDir().mkpath(artifactDir);
            QVERIFY(window.grab().save(artifactDir + "/send-panel.png"));
        }
        tabs->setCurrentIndex(1);
        QTest::qWait(30);
        QCOMPARE(panel->height(), paneH);
        QCOMPARE(panel->preferredHeight(), preferred);
        if (!artifactDir.isEmpty())
            QVERIFY(window.grab().save(artifactDir + QStringLiteral("/file-send.png")));
        auto *stats = window.findChild<QLabel *>(QStringLiteral("fileTransferStats"));
        QVERIFY(!window.findChild<QComboBox *>(QStringLiteral("fileTransferBeforeTail")));
        QVERIFY(!window.findChild<QComboBox *>(QStringLiteral("fileTransferAfterTail")));
        QVERIFY(!window.findChild<QComboBox *>(QStringLiteral("fileTransferBefore")));
        QVERIFY(stats && stats->isVisibleTo(&window));
        QVERIFY2(!stats->text().contains(QStringLiteral("B/s")), qPrintable(stats->text()));
        QVERIFY(!stats->text().contains(QStringLiteral("重试")));
        QVERIFY(window.findChild<QComboBox *>(QStringLiteral("fileTransferMode")));
        auto *beforeText = window.findChild<QLineEdit *>(QStringLiteral("fileTransferBeforeText"));
        auto *chunk = window.findChild<QSpinBox *>(QStringLiteral("fileTransferChunk"));
        auto *sendMode = window.findChild<QComboBox *>(QStringLiteral("fileTransferMode"));
        auto *sendPath = window.findChild<QLineEdit *>(QStringLiteral("fileTransferPath"));
        QVERIFY(beforeText && beforeText->isVisibleTo(&window));
        QCOMPARE(beforeText->placeholderText(), QStringLiteral("AT+START\\r\\n"));
        auto *beforeFormat = window.findChild<QComboBox *>(QStringLiteral("fileTransferBeforeFormat"));
        QVERIFY(beforeFormat);
        beforeFormat->setCurrentIndex(1);
        QTest::qWait(30);
        QCOMPARE(beforeText->placeholderText(), QStringLiteral("0D 0A，留空不发"));
        beforeFormat->setCurrentIndex(0);
        QTest::qWait(30);
        QCOMPARE(beforeText->placeholderText(), QStringLiteral("AT+START\\r\\n"));
        QVERIFY(chunk && sendMode && sendPath);
        const auto midY = [](const QWidget *widget, const QWidget *origin) {
            return widget->mapTo(origin, QPoint()).y() + widget->height() / 2;
        };
        if (sendMode->currentData().toString() != QLatin1String("raw"))
            sendMode->setCurrentIndex(sendMode->findData(QStringLiteral("raw")));
        QTest::qWait(30);
        QVERIFY2(chunk->isVisibleTo(&window), "RAW 发送要能设置分包");
        auto *chunkGap = window.findChild<QSpinBox *>(QStringLiteral("fileTransferChunkGap"));
        QVERIFY(chunkGap);
        QCOMPARE(chunkGap->suffix(), QStringLiteral(" ms"));
        QCOMPARE(chunkGap->minimum(), 0);
        QCOMPARE(chunkGap->buttonSymbols(), QAbstractSpinBox::NoButtons);
        QVERIFY2(chunkGap->isVisibleTo(&window), "RAW 发送要能设置包间隔");
        QVERIFY2(qAbs(midY(chunk, &window) - midY(chunkGap, &window)) <= 2,
            "分包大小与包间隔应在同一行");
        const int tabBottom = tabs->tabBar()->mapTo(&window, QPoint(0, tabs->tabBar()->height())).y();
        // 页顶边距是 4。间距小于它，说明页签和第一行贴在一起了。
        QVERIFY2(sendMode->mapTo(&window, QPoint()).y() - tabBottom >= 4,
            qPrintable(QStringLiteral("页签与下方内容间距过小：%1")
                           .arg(sendMode->mapTo(&window, QPoint()).y() - tabBottom)));
        QCOMPARE(chunk->buttonSymbols(), QAbstractSpinBox::NoButtons);
        auto *beforeDelay = window.findChild<QSpinBox *>(QStringLiteral("fileTransferBeforeDelay"));
        auto *afterDelay = window.findChild<QSpinBox *>(QStringLiteral("fileTransferAfterDelay"));
        QVERIFY(beforeDelay && afterDelay);
        QCOMPARE(beforeDelay->buttonSymbols(), QAbstractSpinBox::NoButtons);
        QCOMPARE(afterDelay->buttonSymbols(), QAbstractSpinBox::NoButtons);
        auto *block128 = window.findChild<QPushButton *>(QStringLiteral("fileTransferBlock128"));
        auto *blockLabel = window.findChild<QLabel *>(QStringLiteral("fileTransferBlockLabel"));
        QVERIFY(block128 && !block128->isVisibleTo(&window));
        QVERIFY(blockLabel && !blockLabel->isVisibleTo(&window));
        QVERIFY(sendMode->findData(QStringLiteral("xmodem")) >= 0);
        sendMode->setCurrentIndex(sendMode->findData(QStringLiteral("xmodem")));
        QTest::qWait(30);
        QVERIFY2(!block128->isVisibleTo(&window), "XMODEM 固定 128 字节，不提供分包选择");
        QVERIFY2(!blockLabel->isVisibleTo(&window), "XMODEM 不应留下分包标签");
        QVERIFY(!chunk->isVisibleTo(&window));
        QVERIFY2(sendMode->toolTip().contains(QStringLiteral("XMODEM")),
            qPrintable(sendMode->toolTip()));
        QVERIFY2(sendMode->toolTip().contains(QStringLiteral("NAK")),
            qPrintable(sendMode->toolTip()));
        sendMode->setCurrentIndex(sendMode->findData(QStringLiteral("ymodem")));
        QTest::qWait(30);
        QVERIFY(block128->isVisibleTo(&window));
        QVERIFY(blockLabel->isVisibleTo(&window));
        QVERIFY(!chunk->isVisibleTo(&window));
        QVERIFY(!chunkGap->isVisibleTo(&window));
        sendMode->setCurrentIndex(sendMode->findData(QStringLiteral("raw")));
        QTest::qWait(30);
        QVERIFY(chunk->isVisibleTo(&window));
        QVERIFY(chunkGap->isVisibleTo(&window));
        QVERIFY(!block128->isVisibleTo(&window));
        QVERIFY(!blockLabel->isVisibleTo(&window));
        QVERIFY2(qAbs(midY(sendMode, &window) - midY(sendPath, &window)) <= 2,
            "端点/方式与文件路径应在同一行");
        auto *afterText = window.findChild<QLineEdit *>(QStringLiteral("fileTransferAfterText"));
        QVERIFY(afterText && afterText->isVisibleTo(&window));
        QVERIFY2(qAbs(midY(beforeText, &window) - midY(afterText, &window)) <= 2,
            "前置指令与后置指令应在同一行");
        auto *fileStart = window.findChild<QPushButton *>(QStringLiteral("fileTransferStart"));
        auto *fileProgress = window.findChild<QProgressBar *>(QStringLiteral("fileTransferProgress"));
        QVERIFY(fileStart && fileProgress && fileStart->isVisibleTo(&window));
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fileTransferStop")));
        QVERIFY2(fileStart->mapTo(&window, QPoint()).x() > fileProgress->mapTo(&window, QPoint()).x(),
            "发送按钮应在进度条右侧");
        QCOMPARE(fileStart->height(), fileProgress->height());
        auto *fileStatus = window.findChild<QLabel *>(QStringLiteral("fileTransferStatus"));
        QVERIFY(fileStatus);
        sendPath->clear();
        QCOMPARE(fileStatus->text(), QStringLiteral("未选择"));
        QVERIFY2(stats->text().contains(QStringLiteral("大小")), qPrintable(stats->text()));
        QVERIFY(!stats->text().contains(QStringLiteral("CRC16")));
        QTemporaryDir digestDir;
        QVERIFY(digestDir.isValid());
        const QString digestPath = digestDir.filePath(QStringLiteral("payload.bin"));
        QFile digestFile(digestPath);
        QVERIFY(digestFile.open(QIODevice::WriteOnly));
        // 与传输层同一组校验：CRC16-XMODEM 31C3，CRC32-IEEE CBF43926。
        QCOMPARE(digestFile.write(QByteArray("123456789")), qint64(9));
        digestFile.close();
        sendPath->setText(QDir::cleanPath(digestPath));
        QTRY_VERIFY_WITH_TIMEOUT(stats->text().contains(QStringLiteral("CRC16 31C3")), 3000);
        QVERIFY2(stats->text().contains(QStringLiteral("CRC32 CBF43926")), qPrintable(stats->text()));
        QVERIFY(stats->text().contains(QStringLiteral("payload.bin")));
        QVERIFY(stats->text().contains(QStringLiteral("9 B")));
        QCOMPARE(fileStatus->text(), QStringLiteral("待发送"));
        QVERIFY(!stats->text().contains(QStringLiteral("B/s")));
        tabs->setCurrentIndex(2);
        QTest::qWait(30);
        QCOMPARE(panel->height(), paneH);
        QCOMPARE(panel->preferredHeight(), preferred);
        auto *receiveMode = window.findChild<QComboBox *>(QStringLiteral("fileReceiveMode"));
        auto *receivePath = window.findChild<QLineEdit *>(QStringLiteral("fileReceivePath"));
        QVERIFY(receiveMode && receivePath && receivePath->isVisibleTo(&window));
        receiveMode->setCurrentIndex(receiveMode->findData(QStringLiteral("raw")));
        receivePath->clear();
        receiveMode->setCurrentIndex(receiveMode->findData(QStringLiteral("ymodem")));
        receiveMode->setCurrentIndex(receiveMode->findData(QStringLiteral("raw")));
        QTest::qWait(30);
        QVERIFY2(qAbs(midY(receiveMode, &window) - midY(receivePath, &window)) <= 2,
            "接收页端点/方式与保存路径应在同一行");
        QVERIFY2(receivePath->text().contains(QStringLiteral("raw_"))
                && receivePath->text().endsWith(QStringLiteral(".bin")),
            qPrintable(receivePath->text()));
        auto *receiveStats = window.findChild<QLabel *>(QStringLiteral("fileReceiveStats"));
        auto *receiveStatus = window.findChild<QLabel *>(QStringLiteral("fileReceiveStatus"));
        QVERIFY(receiveStats && receiveStatus);
        QCOMPARE(receiveStatus->text(), QStringLiteral("待接收"));
        QVERIFY2(receiveStats->text().contains(QStringLiteral("新文件")), qPrintable(receiveStats->text()));
        QVERIFY(!receiveStats->text().contains(QStringLiteral("CRC16")));
        auto *receiveBeforeDelay = window.findChild<QSpinBox *>(QStringLiteral("fileReceiveBeforeDelay"));
        auto *receiveAfterDelay = window.findChild<QSpinBox *>(QStringLiteral("fileReceiveAfterDelay"));
        QVERIFY(receiveBeforeDelay && receiveAfterDelay);
        QCOMPARE(receiveBeforeDelay->buttonSymbols(), QAbstractSpinBox::NoButtons);
        QCOMPARE(receiveAfterDelay->buttonSymbols(), QAbstractSpinBox::NoButtons);
        receiveMode->setCurrentIndex(receiveMode->findData(QStringLiteral("ymodem")));
        QTest::qWait(30);
        QVERIFY2(QFileInfo(receivePath->text()).isDir(),
            qPrintable(QStringLiteral("YMODEM 应使用目录：%1").arg(receivePath->text())));
        QVERIFY(!receivePath->text().endsWith(QStringLiteral(".bin")));
        QVERIFY2(receiveStats->text().contains(QStringLiteral("对端")), qPrintable(receiveStats->text()));
        QVERIFY(!receiveStats->text().contains(QStringLiteral("CRC16")));
        QCOMPARE(receiveStatus->text(), QStringLiteral("待接收"));
        const QString customDir = QDir(QDir::tempPath()).filePath(QStringLiteral("qqcom-ymodem-dir"));
        QDir().mkpath(customDir);
        receivePath->setText(QDir::cleanPath(customDir));
        receiveMode->setCurrentIndex(receiveMode->findData(QStringLiteral("raw")));
        receiveMode->setCurrentIndex(receiveMode->findData(QStringLiteral("ymodem")));
        QTest::qWait(30);
        QCOMPARE(QDir::cleanPath(receivePath->text()), QDir::cleanPath(customDir));
        tabs->setCurrentIndex(0);
    }

    // 目标 A、B 宽度取齐且不截断文字；发送按钮比它们更宽更高，文字同样不被截断。
    void sendActionButtonsShareOneWidth()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        QVERIFY(!window.findChild<QPushButton *>("sendTargetAB"));
        auto *targetA = window.findChild<QPushButton *>("sendTargetA");
        auto *targetB = window.findChild<QPushButton *>("sendTargetB");
        auto *send = window.findChild<QPushButton *>("PrimaryButton");
        QVERIFY2(targetA && targetB && send && targetA->isVisibleTo(&window)
                     && targetB->isVisibleTo(&window) && send->isVisibleTo(&window),
                 "发送区按钮缺失");
        QCOMPARE(targetA->width(), targetB->width());
        for (auto *button : {targetA, targetB, send}) {
            // 宽度必须撑得下完整文字：QPushButton 放不下会在文字中间插省略号，
            // 用 sizeHint()（含图标/内边距的完整需求）当判据。
            QVERIFY2(button->width() >= button->sizeHint().width(),
                qPrintable(QStringLiteral("按钮文字被截断：%1 需要 %2，实得 %3")
                               .arg(button->text())
                               .arg(button->sizeHint().width())
                               .arg(button->width())));
        }
        QCOMPARE(send->height(), targetA->height());
        QCOMPARE(targetA->height(), targetB->height());
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("sendModeTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 3);
        for (int i = 0; i < tabs->count(); ++i)
            QVERIFY2(!tabs->tabIcon(i).isNull(), qPrintable(tabs->tabText(i)));
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fileTransferBrowse"))->icon().isNull());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fileReceiveBrowse"))->icon().isNull());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fileTransferStart"))->icon().isNull());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fileReceiveStart"))->icon().isNull());
        QVERIFY2(send->width() > targetA->width(),
            qPrintable(QStringLiteral("发送按钮应比目标开关更宽：发送 %1，目标 %2")
                           .arg(send->width()).arg(targetA->width())));
        // 正文跟着文件页一起自适应，至少留得下一行，不能被压成一条缝。
        auto *editor = window.findChild<QPlainTextEdit *>("sendEditor");
        QVERIFY(editor);
        const int lineSpacing = editor->fontMetrics().lineSpacing();
        QVERIFY2(editor->height() >= lineSpacing * 2,
            qPrintable(QStringLiteral("正文只有 %1 行").arg(editor->height() / qMax(1, lineSpacing))));
    }

    // 发送按钮带了对象名，通用的 QPushButton:pressed 盖不住它。
    // 按住（含悬停）时底色必须明显更深，否则看不出点没点上。尺寸不能跟着变。
    void sendButtonDarkensWhilePressed()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *send = window.findChild<QPushButton *>(QStringLiteral("PrimaryButton"));
        QVERIFY(send && send->isVisibleTo(&window));
        send->setEnabled(true);
        QVERIFY(send->isEnabled());

        // 文字和图标只占中间一小块。按出现次数取主色，避开抗锯齿边缘上更亮的蓝。
        const auto fill = [](QWidget *widget) {
            const QImage image = widget->grab().toImage();
            QHash<QRgb, int> counts;
            const int margin = 3;
            for (int y = margin; y < image.height() - margin; ++y) {
                for (int x = margin; x < image.width() - margin; ++x) {
                    const QColor color = image.pixelColor(x, y);
                    if (color.alpha() < 200) continue;
                    const QRgb key = qRgb(color.red() & ~7, color.green() & ~7, color.blue() & ~7);
                    counts[key] += 1;
                }
            }
            QRgb best = 0;
            int bestCount = -1;
            for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
                if (it.value() > bestCount) {
                    bestCount = it.value();
                    best = it.key();
                }
            }
            return QColor::fromRgb(best);
        };

        const QSize size = send->size();
        const QColor idle = fill(send);
        // 真正点击时按钮同时处于悬停和按下。两条规则特异性相同，
        // 按下写在悬停前面的话，这里采到的仍是悬停色。
        send->setAttribute(Qt::WA_UnderMouse, true);
        send->setDown(true);
        const QColor pressed = fill(send);
        QCOMPARE(send->size(), size);
        send->setDown(false);

        QVERIFY2(idle.isValid() && pressed.isValid(),
            qPrintable(QStringLiteral("未采到按钮底色：常态 rgb(%1,%2,%3)，按下 rgb(%4,%5,%6)")
                           .arg(idle.red()).arg(idle.green()).arg(idle.blue())
                           .arg(pressed.red()).arg(pressed.green()).arg(pressed.blue())));
        QVERIFY2(idle.blue() > pressed.blue() + 40
                     && idle.lightness() > pressed.lightness() + 30
                     && pressed.blue() > pressed.red() + 40
                     && pressed.blue() > pressed.green(),
            qPrintable(QStringLiteral("按下应明显更深的蓝：常态 rgb(%1,%2,%3)，按下 rgb(%4,%5,%6)")
                           .arg(idle.red()).arg(idle.green()).arg(idle.blue())
                           .arg(pressed.red()).arg(pressed.green()).arg(pressed.blue())));
    }

    // 展开右侧指令面板**不能**把数据区的头两行压变形（用户反馈：展开后过滤栏按钮
    // 被过度挤压）。约束是双向的：过滤条上的按钮保持自己的最小宽度，数据区不被压到
    // 它的宽度下限之下。这里在最小窗宽下量几何。
    // ★ 发送区的「发送」必须真的把内容发出去（用户反馈："实现发送区的发送功能"）。
    //
    // 这条链上曾断过一环：发送按钮的可用性取决于 `SendPanel::m_openFlags`，而唯一能
    // 喂它的 `SendPanel::setEndpointOpen()` **全项目没有任何调用点** —— 于是
    // m_openFlags 恒为 false、按钮永远禁用，「发送」看上去就是个摆设。
    //
    // 所以用例刻意**从 MainWindow 的真实路径走**（点端点的动作按钮让它真的监听），
    // 而不是直接调 `setEndpointOpen()`：直接调的话，接线又断了也照样绿。
    void sendPanelSendsThroughRealEndpointState()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *send = window.findChild<QPushButton *>(QStringLiteral("PrimaryButton"));
        auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("sendEditor"));
        auto *model = window.findChild<TrafficModel *>();
        auto *panel = window.findChild<SendPanel *>();
        auto *targetA = window.findChild<QPushButton *>(QStringLiteral("sendTargetA"));
        auto *targetB = window.findChild<QPushButton *>(QStringLiteral("sendTargetB"));
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        auto *repeat = window.findChild<QCheckBox *>(QStringLiteral("repeatSending"));
        QVERIFY(send && editor && model && panel && targetA && targetB && b && repeat);

        // 没有端点连上时按钮必须是禁用的——"可用性"是双向契约，
        // 修 bug 时很容易顺手改成无条件启用，那等于把"能不能发"变成"点了才知道"。
        QVERIFY2(!send->isEnabled(), "未连接端点时发送按钮不该可用");

        // 目标切换刷新可用性；B 使用真实 UDP 回环接收器验收发送字节。
        QUdpSocket receiver;
        QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
        PortDraft udp;
        udp.kind = PortDraft::Udp;
        udp.values = {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
                      {"udp.target", "127.0.0.1"}, {"udp.targetPort", receiver.localPort()}};
        b->setDraft(udp);
        if (!targetB->isChecked()) targetB->click();
        if (targetA->isChecked()) targetA->click();
        QVERIFY(!targetA->isChecked() && targetB->isChecked());
        QCOMPARE(panel->targetMask(), 2);

        // 打开 B：走真实端点动作与状态回传。
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(action);
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("解  绑"));

        // 端点打开后发送按钮必须启用。
        QTRY_VERIFY2(send->isEnabled(),
            "端点已监听，但发送按钮仍禁用——检查 handleEndpointState 是否把状态喂给了 SendPanel");

        // 内容与末尾都显式设定，不依赖共享 preferences 的残留（用例间会串扰）。
        editor->setPlainText(QStringLiteral("+CR,GO;"));
        auto *format = window.findChild<QComboBox *>(QStringLiteral("sendFormat"));
        auto *suffix = window.findChild<QComboBox *>(QStringLiteral("sendSuffix"));
        QVERIFY(format && suffix);
        format->setCurrentText(QStringLiteral("UTF-8"));
        suffix->setCurrentIndex(3); // +CR+LF
        QTRY_VERIFY(send->isEnabled());

        // 手动发送：真实接收端和历史记录的载荷一致。
        const int rowsBefore = model->rowCount();
        send->click();
        QTRY_VERIFY(model->rowCount() > rowsBefore);
        auto sent = model->record(model->rowCount() - 1);
        QCOMPARE(sent.direction, Traffic::Direction::Transmit);
        QCOMPARE(sent.source.endpointIndex, 1);       // 发去了选中的 B，不是默认的 A
        QCOMPARE(sent.origin, Traffic::Origin::Manual); // 手点 = 手动
        QCOMPARE(sent.raw, QByteArray("+CR,GO;\r\n")); // 末尾真的追加在字节末尾
        QTRY_VERIFY(receiver.hasPendingDatagrams());
        QByteArray received(int(receiver.pendingDatagramSize()), Qt::Uninitialized);
        QCOMPARE(receiver.readDatagram(received.data(), received.size()), qint64(received.size()));
        QCOMPARE(received, sent.raw);

        // 两个都选中：同一拍发到 A 和 B。A 没打开，也要留下一条失败记录。
        targetA->click();
        QCOMPARE(panel->targetMask(), 3);
        const int rowsBeforeBoth = model->rowCount();
        send->click();
        QTRY_VERIFY(model->rowCount() >= rowsBeforeBoth + 2);
        bool sawA = false;
        bool sawB = false;
        for (int row = rowsBeforeBoth; row < model->rowCount(); ++row) {
            const auto record = model->record(row);
            if (record.source.endpointIndex == 0 && record.direction == Traffic::Direction::Error) sawA = true;
            if (record.source.endpointIndex == 1 && record.direction == Traffic::Direction::Transmit) sawB = true;
        }
        QVERIFY2(sawA && sawB, "两个都选中时，A 关闭应记录失败，B 应成功发送");
        targetA->click();
        QCOMPARE(panel->targetMask(), 2);

        // 定时发送：勾选即发第一拍，且**来源标记必须是 Scheduled**。
        // 这个参数以前在 sendRequested 的 lambda 里被丢掉了（形参只声明了两个），
        // 于是每一拍都被记成手动，屏幕上显示 `TX+MAN` 而不是 `TX+TIMER`。
        const int rowsBeforeTick = model->rowCount();
        repeat->setChecked(true);
        QTRY_VERIFY(model->rowCount() > rowsBeforeTick);
        bool sawScheduled = false;
        for (int row = model->rowCount() - 1; row >= rowsBeforeTick; --row) {
            if (model->record(row).origin == Traffic::Origin::Scheduled) { sawScheduled = true; break; }
        }
        repeat->setChecked(false); // 立刻停，别让定时器继续往模型里灌
        QVERIFY2(sawScheduled, "定时发送的报文来源标记应为 Scheduled（界面显示 TX+TIMER）");

        // 关闭端点后按钮必须回到禁用：可用性跟着连接状态走，不是一次性的。
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("绑  定"));
        QTRY_VERIFY2(!send->isEnabled(), "端点已关闭，发送按钮应回到禁用");
    }

    // ★ 打开/关闭必须在监视区留下**读得出结果**的迁移记录，且不能顶掉状态栏统计。
    //
    // 两个用户可见问题的回归守卫：
    // ① 状态行正文曾经直接取传输层的 detail（`COM2 @115200`），只有端口参数、
    //    没有"已打开/已关闭"这个状态词——屏幕上根本读不出连接成没成。
    // ② 统计行是用 `addWidget()` 挂的**普通**部件，而 `QStatusBar::showMessage()`
    //    会隐藏所有普通部件：打开端点时那句参数摘要一出现，RX/TX/条数/桥接整行
    //    就消失，5 秒后才回来；反复连接每次都重置那个计时器，看起来就是
    //    "统计数据不显示了、很久才恢复"。
    void endpointTransitionsAreLoggedAndKeepStatsVisible()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(text && b);
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(action);

        const auto lastLine = [text] { return text->document()->lastBlock().text(); };

        // 打开：状态行必须同时含方向标记与"已打开"——
        // 只有端口参数（旧行为）就说明状态词没拼上。
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("停  止"));
        QTRY_VERIFY(lastLine().contains(QStringLiteral("已打开")));
        QVERIFY2(lastLine().contains(QStringLiteral("S")),
            qPrintable(QStringLiteral("状态行应带方向词：") + lastLine()));
        const QString openedLine = lastLine();

        // ★ 打开之后统计行必须**仍然可见**。旧实现里 showMessage 会把它整行藏掉，
        //   所以这条断言正是那个 bug 的探测器。
        for (const auto *name : {"RxStatus", "TxStatus", "TrafficStatus", "RouteStatus"}) {
            auto *label = window.findChild<QLabel *>(QString::fromLatin1(name));
            QVERIFY2(label && label->isVisibleTo(&window),
                qPrintable(QStringLiteral("打开端点后统计标签被隐藏：%1").arg(name)));
        }

        // 关闭：同样要留下迁移记录，且统计行照旧可见。
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("监  听"));
        QTRY_VERIFY(lastLine().contains(QStringLiteral("已关闭")));
        QVERIFY2(lastLine() != openedLine,
            qPrintable(QStringLiteral("关闭后状态行没更新：") + lastLine()));
        // 关闭来源必须写进正文：用户主动停止与对端断开/设备错误是不同事件，
        // 一条没有来源的"已关闭"让人无从判断是谁关的（用户反馈"不知道谁关闭了"）。
        QVERIFY2(lastLine().contains(QStringLiteral("用户停止")),
            qPrintable(QStringLiteral("关闭来源应写进正文：") + lastLine()));
        // 而且必须说清**是哪个端口**关了：只说"已关闭 · 用户停止"仍然答不全是 A 还是 B、
        // 是哪个口（用户反馈"需要清晰表明是那个端口关闭"）。身份取草稿的真实配置——
        // 这条用例里 B 是 TCP 服务器，身份形如 "B · 0.0.0.0:<端口>"。
        const QString identity = QStringLiteral("B · 0.0.0.0:%1")
            .arg(b->findChild<QSpinBox *>(QStringLiteral("server.port"))->value());
        QVERIFY2(lastLine().contains(identity),
            qPrintable(QStringLiteral("关闭行应带端点身份「%1」：%2").arg(identity, lastLine())));
        // 把渲染出来的关闭行原样记进测试日志：这里的**文案本身就是契约**
        //（用户要求"清晰表明是那个端口关闭"），出问题时能直接看到实际长什么样，
        // 不必反推。与 SPLITPROBE / window minimum width 那几条诊断同一用途。
        qInfo("CLOSEROW %s", qPrintable(lastLine()));
        for (const auto *name : {"RxStatus", "TxStatus", "TrafficStatus"}) {
            auto *label = window.findChild<QLabel *>(QString::fromLatin1(name));
            QVERIFY2(label && label->isVisibleTo(&window),
                qPrintable(QStringLiteral("关闭端点后统计标签被隐藏：%1").arg(name)));
        }
    }

    // 一次失败只留**一条**日志（用户反馈"错误跟状态感觉重复了"）。传输层对失败成对
    // 发出 errorOccurred + stateChanged(Closed)（两条都是底层真实事件，core 测试亦依赖），
    // 合并发生在 EndpointRuntime：错误行保留完整原因，紧随的 Closed 不再单独成 State 行。
    // 用 TCP 服务器监听一个已被占用的端口制造同步失败（不依赖网络栈超时，确定性最强）。
    void endpointFailureLeavesExactlyOneLogLine()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        auto *model = window.findChild<TrafficModel *>();
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(model && b); // B 默认 TCP 服务器
        QTcpServer blocker;  // 占住端口的"另一个进程"：保持监听直到本用例结束
        QVERIFY(blocker.listen(QHostAddress::AnyIPv4, 0));
        b->findChild<QSpinBox *>(QStringLiteral("server.port"))->setValue(blocker.serverPort());
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(action);

        action->click(); // 监听一个已被占用的端口 → listen() 同步失败
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(model->record(0).direction, Traffic::Direction::Error);
        // 那条唯一的日志必须能读出失败原因（而不是零信息的"打开失败"）。
        QVERIFY2(model->record(0).statusText.contains(QStringLiteral("失败")),
            qPrintable(QStringLiteral("错误行正文应含原因：") + model->record(0).statusText));
        // 同一次失败不再另成一条 State 行——这正是"感觉重复了"的根源。
        int stateRows = 0;
        for (int i = 0; i < model->rowCount(); ++i)
            if (model->record(i).direction == Traffic::Direction::State) ++stateRows;
        QCOMPARE(stateRows, 0);
    }

    // 失败播报必须走 showStatusNote，**不得**用 QStatusBar::showMessage——后者会隐藏所有
    // addWidget 挂的普通部件，把 RX/TX/条数统计整行顶掉 5 秒。旧实现两处残留的 showMessage
    // 正是这么干的，偏偏"打开失败常常连续发生"，用户于是看到统计行反复消失。
    //
    // 为什么必须**独立成例**：同类的端口断言分别住在 portOpenFailureSurfacesErrorRowAndStatus
    // 与 runtimeDisconnectReportsErrorRowWithoutPopup 里，而这两条用例在本机因**预存失败**
    // （断言要求日志行带端点名前缀，属过时用例）会提前中止——挂在它们后面的 StatusNote /
    // 统计行断言**永远执行不到**。独立用例才真正跑到。
    //
    // 两个断言必须**同一次失败、同一时刻**成立：
    //   B（核心）统计行未被顶掉——旧 showMessage 会让 RxStatus/TxStatus/TrafficStatus 不可见；
    //   A（信息不丢）失败原因仍被播报——m_statusNote 文本含"失败"。
    // 把 B 放在前：这样一旦有人把 showStatusNote 改回 showMessage，本用例会**先**在 B 变红，
    // 明确指向"统计行被顶掉"这个回归点，而不是被 A 的连锁失败掩盖。
    void errorBroadcastKeepsStatsVisible()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        auto *model = window.findChild<TrafficModel *>();
        auto *note = window.findChild<QLabel *>(QStringLiteral("StatusNote"));
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(model && note && b); // B 默认 TCP 服务器
        QTcpServer blocker;          // 占住端口的"另一个进程"：保持监听直到本用例结束
        QVERIFY(blocker.listen(QHostAddress::AnyIPv4, 0));
        b->findChild<QSpinBox *>(QStringLiteral("server.port"))->setValue(blocker.serverPort());
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(action);

        action->click(); // 监听一个已被占用的端口 → listen() 同步失败（确定性，不依赖超时）
        // 等这次失败处理完毕：模型里出现那条 Error 行，且播报已落到 StatusNote。
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(model->record(0).direction, Traffic::Direction::Error);

        // —— 断言 B（本次补修的核心）——
        // 用 addWidget 挂的统计行必须**仍然可见**。旧 showMessage 会把它整行隐藏 5 秒。
        for (const auto *name : {"RxStatus", "TxStatus", "TrafficStatus"}) {
            auto *label = window.findChild<QLabel *>(QString::fromLatin1(name));
            QVERIFY2(label && label->isVisibleTo(&window),
                qPrintable(QStringLiteral("失败播报把统计行顶掉了：%1 不可见").arg(name)));
        }
        // —— 断言 A（信息不丢）——
        // 改走 showStatusNote 后，失败原因仍必须被播报出来。
        QVERIFY2(note->text().contains(QStringLiteral("失败")),
            qPrintable(QStringLiteral("失败播报丢失：StatusNote 文本不含失败原因：") + note->text()));
        // 打开失败没有扔掉字节，丢弃计数保持隐藏。
        auto *drop = window.findChild<QLabel *>(QStringLiteral("DropStatus"));
        QVERIFY(drop);
        QVERIFY(!drop->isVisibleTo(&window));
    }

    void rejectedSendCountsDroppedBytesInStatusAndLog()
    {
        clearEndpointDrafts();
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PreferencesStore store(nullptr, directory.filePath(QStringLiteral("preferences.ini")));
        store.setValue(QStringLiteral("traffic/recordPath"), directory.filePath(QStringLiteral("traffic.log")));
        store.setValue(QStringLiteral("traffic/record"), true);

        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *panel = window.findChild<TrafficPanel *>();
        auto *model = window.findChild<TrafficModel *>();
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *send = window.findChild<QPushButton *>(QStringLiteral("PrimaryButton"));
        auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("sendEditor"));
        auto *targetA = window.findChild<QPushButton *>(QStringLiteral("sendTargetA"));
        auto *targetB = window.findChild<QPushButton *>(QStringLiteral("sendTargetB"));
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        auto *dropGroup = window.findChild<QWidget *>(QStringLiteral("DropGroup"));
        auto *drop = window.findChild<QLabel *>(QStringLiteral("DropStatus"));
        auto *note = window.findChild<QLabel *>(QStringLiteral("StatusNote"));
        QVERIFY(panel && model && text && send && editor && targetA && targetB && b && dropGroup && drop && note);
        panel->setPreferences(&store);
        QVERIFY(!dropGroup->isVisibleTo(&window));

        // 只发给已监听但还没有客户端的 B。写入被拒绝，4 字节记入丢弃。
        // 先勾上 B 再取消 A：一个目标都不剩时，点掉最后一个会立刻被勾回去。
        if (!targetB->isChecked()) targetB->click();
        if (targetA->isChecked()) targetA->click();
        QVERIFY(!targetA->isChecked() && targetB->isChecked());
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(action);
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("停  止"));
        QTRY_VERIFY(send->isEnabled());

        auto *format = window.findChild<QComboBox *>(QStringLiteral("sendFormat"));
        auto *suffix = window.findChild<QComboBox *>(QStringLiteral("sendSuffix"));
        QVERIFY(format && suffix);
        format->setCurrentText(QStringLiteral("UTF-8"));
        suffix->setCurrentIndex(0);
        editor->setPlainText(QStringLiteral("LOST"));
        send->click();

        bool sawDrop = false;
        QTRY_VERIFY(([&] {
            for (int row = 0; row < model->rowCount(); ++row) {
                const auto record = model->record(row);
                if (record.direction == Traffic::Direction::Error && record.rawLength == 4) {
                    sawDrop = record.statusText.contains(QStringLiteral("丢弃未写出数据 4 字节"));
                    return true;
                }
            }
            return false;
        })());
        QVERIFY(sawDrop);
        QTRY_VERIFY(dropGroup->isVisibleTo(&window));
        QCOMPARE(drop->text(), QStringLiteral("4 B"));
        QCOMPARE(note->property("tone").toString(), QStringLiteral("error"));
        // 底部是短句，完整原因在悬停和数据区。
        QVERIFY(note->text().contains(QStringLiteral("未写出 4 字节")));
        QVERIFY(note->toolTip().contains(QStringLiteral("丢弃未写出数据 4 字节")));
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("丢弃未写出数据 4 字节")));

        const auto names = QDir(directory.path()).entryList({QStringLiteral("traffic-*.log")}, QDir::Files);
        QVERIFY2(!names.isEmpty(), "开启记录后应生成本次日志文件");
        QFile log(QDir(directory.path()).filePath(names.first()));
        QVERIFY(log.open(QIODevice::ReadOnly));
        const auto body = QString::fromUtf8(log.readAll());
        QVERIFY2(body.contains(QStringLiteral("丢弃未写出数据 4 字节")) && body.contains(QStringLiteral("\tE\t")),
            qPrintable(body));
    }

    // 正向锁定（QA 补充）：失败合并的**边界**。合并锚点 m_pendingFailureGeneration 会一直挂到
    // 下一次 stateChanged 才被消费，所以必须证明它不会"残留到下一代"或"该合却没合"：
    //   C1 连续两次失败（用户连点）——每次各自恰好一条 Error 行，且不残留任何 State 行；
    //   C4 失败后再点连接＝换代——openEndpoint 清锚点，第二次失败仍恰好一条；
    //   A  失败信息不能丢——端点必须仍被上报回 Closed（动作按钮回到"监听"，不是卡在"停止"）；
    //   C2 打开成功后再主动停止——这次关闭必须留行，且能读出"已关闭 · 用户停止"。
    // 若把 endpointruntime.cpp 的合并判断改回"从不合并"，本用例会在 C1/C4 处变红。
    void endpointFailureMergeIsSingleLineAcrossConsecutiveFailures()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        auto *model = window.findChild<TrafficModel *>();
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(model && b); // B 默认 TCP 服务器
        auto *portBox = b->findChild<QSpinBox *>(QStringLiteral("server.port"));
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(portBox && action);

        const auto countRows = [model](Traffic::Direction direction) {
            int n = 0;
            for (int i = 0; i < model->rowCount(); ++i)
                if (model->record(i).direction == direction) ++n;
            return n;
        };

        // C1 + C4：连点两次都命中"监听一个已被占用端口"的确定性同步失败。
        QTcpServer blocker;
        QVERIFY(blocker.listen(QHostAddress::AnyIPv4, 0));
        portBox->setValue(blocker.serverPort());

        action->click(); // 失败 #1
        QTRY_COMPARE(countRows(Traffic::Direction::Error), 1);
        // A：端点最终回到 Closed 这件事不能丢——动作按钮必须回到"监听"。
        QTRY_COMPARE(action->text(), QStringLiteral("监  听"));
        // 一次失败只留一条日志：不得出现零信息的 State 行。
        QCOMPARE(countRows(Traffic::Direction::State), 0);

        action->click(); // C4：失败后再点连接（换代）→ 仍恰好一条 Error
        QTRY_COMPARE(countRows(Traffic::Direction::Error), 2);
        QTRY_COMPARE(action->text(), QStringLiteral("监  听"));
        // ★ 锚点若残留，第二次失败会多出一条 State 行，这里就会红。
        QCOMPARE(countRows(Traffic::Direction::State), 0);
        // 每条失败都读得出原因（错误行正文非空）。
        for (int i = 0; i < model->rowCount(); ++i)
            if (model->record(i).direction == Traffic::Direction::Error)
                QVERIFY2(!model->record(i).statusText.isEmpty(),
                    qPrintable(QStringLiteral("错误行丢失了原因：") + model->record(i).statusText));

        // 收掉失败弹窗，别让它干扰后续路径。
        for (auto *box : window.findChildren<QMessageBox *>())
            box->close();
        QTest::qWait(30);

        // C2：换到空闲端口打开成功，再主动停止——这次关闭必须留行，且读出"用户停止"。
        blocker.close();
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 freePort = probe.serverPort();
        probe.close();
        portBox->setValue(freePort);

        action->click(); // 打开成功 → 留下一条"已打开" State 行，并消费掉可能残留的锚点
        QTRY_COMPARE(action->text(), QStringLiteral("停  止"));
        // 用 QTRY 而不是"读到即断言"：按钮文案与状态行是**两条独立路径**（前者由
        // handleEndpointState 更新，后者经模型通知视图重投影），先后没有保证。
        // 全量跑（机器负载高）时状态行会晚一拍，此刻读到的还是上一行（实测偶发失败）。
        QTRY_VERIFY2(countRows(Traffic::Direction::State) >= 1,
            qPrintable(QStringLiteral("打开成功应留下状态行，实际 ")
                + QString::number(countRows(Traffic::Direction::State))));

        action->click(); // 用户主动停止
        QTRY_COMPARE(action->text(), QStringLiteral("监  听"));
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(text);
        // 同上：等**关闭行真的落到视图里**再断言，不去赌按钮与状态行同拍。
        QTRY_VERIFY2(text->document()->lastBlock().text().contains(QStringLiteral("已关闭 · 用户停止")),
            qPrintable(QStringLiteral("关闭行应读出用户停止：")
                + text->document()->lastBlock().text()));
    }

    // C3（QA 补充）：打开成功 → 运行中**对端断开** = errorOccurred + stateChanged(Closed)
    // 成对路径，必须合并成一条，且留下的那条读得出是什么错。走 TCP 客户端真实路径：
    // 连上本地监听、再让对端 abort，客户端应只多出一条 Error 行（含"断开"），且不多出 State 行。
    // 若把合并判断改回"从不合并"，State 行会多出 1 条 → 变红。
    void endpointRuntimeDisconnectAlsoCollapsesToOneLogLine_data()
    {
        QTest::addColumn<bool>("graceful");
        QTest::newRow("server-closes") << true;
        QTest::newRow("server-aborts") << false;
    }

    void endpointRuntimeDisconnectAlsoCollapsesToOneLogLine()
    {
        QFETCH(bool, graceful);
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        auto *model = window.findChild<TrafficModel *>();
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(model && b);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *prefix = window.findChild<QAction *>(QStringLiteral("prefix"));
        QVERIFY(text && prefix);
        prefix->setChecked(false);
        for (const auto *name : {"showInfo", "showWarning", "showError", "showState"}) {
            auto *event = window.findChild<QAction *>(QString::fromLatin1(name));
            QVERIFY(event && !event->isChecked() && !event->isEnabled());
        }
        auto *type = b->findChild<QComboBox *>(QStringLiteral("portType"));
        QVERIFY(type);
        type->setCurrentIndex(PortDraft::TcpClient); // 0 串口 / 1 TCP 客户端 / 2 TCP 服务器 / 3 UDP
        QTest::qWait(20);

        const auto countRows = [model](Traffic::Direction direction) {
            int n = 0;
            for (int i = 0; i < model->rowCount(); ++i)
                if (model->record(i).direction == direction) ++n;
            return n;
        };

        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        auto *host = b->findChild<QComboBox *>(QStringLiteral("client.host"));
        auto *port = b->findChild<QSpinBox *>(QStringLiteral("client.port"));
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(host && port && action);
        host->setCurrentText(QStringLiteral("127.0.0.1"));
        port->setValue(listener.serverPort());

        action->click(); // 连接
        QTRY_COMPARE(action->text(), QStringLiteral("断  开")); // TCP 客户端打开态文案
        QTRY_VERIFY2(listener.hasPendingConnections(), "客户端应已连上本地监听");
        auto *peer = listener.nextPendingConnection();
        QVERIFY(peer);

        const int errorsBefore = countRows(Traffic::Direction::Error);
        const int statesBefore = countRows(Traffic::Direction::State);

        if (graceful) peer->disconnectFromHost();
        else peer->abort();

        QTRY_COMPARE(countRows(Traffic::Direction::Error), errorsBefore + 1);
        // ★ 成对的 Closed 被并进那条 Error：不得再多出一条 State 行。
        QCOMPARE(countRows(Traffic::Direction::State), statesBefore);
        // 留下的一条读得出是什么错。
        QString lastError;
        for (int i = 0; i < model->rowCount(); ++i)
            if (model->record(i).direction == Traffic::Direction::Error)
                lastError = model->record(i).statusText;
        QVERIFY2(lastError.contains(QStringLiteral("断开")),
            qPrintable(QStringLiteral("错误行应读出断开原因：") + lastError));
        // 端点回到 Closed：动作按钮可再次"连接"。
        QTRY_COMPARE(action->text(), QStringLiteral("连  接"));
        QCOMPARE(text->toPlainText(), QString()); // 纯数据隐藏事件，但模型记录必须还在。
        prefix->setChecked(true);
        QTRY_VERIFY(text->toPlainText().contains(lastError));
        QCOMPARE(countRows(Traffic::Direction::Error), errorsBefore + 1);
        auto *showError = window.findChild<QAction *>(QStringLiteral("showError"));
        showError->setChecked(false);
        QTRY_VERIFY(!text->toPlainText().contains(lastError));
        showError->setChecked(true);
        QTRY_VERIFY(text->toPlainText().contains(lastError));
        for (auto *box : window.findChildren<QMessageBox *>())
            box->close();
    }

    void autoRecoverRetriesAfterUnexpectedDisconnect()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.resize(1280, 720);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *model = window.findChild<TrafficModel *>();
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(model && b);
        b->findChild<QComboBox *>(QStringLiteral("portType"))->setCurrentIndex(PortDraft::TcpClient);
        QTest::qWait(20);
        auto *recover = b->findChild<QCheckBox *>(QStringLiteral("autoRecover"));
        QVERIFY(recover);
        recover->setChecked(true);

        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        b->findChild<QComboBox *>(QStringLiteral("client.host"))->setCurrentText(QStringLiteral("127.0.0.1"));
        b->findChild<QSpinBox *>(QStringLiteral("client.port"))->setValue(listener.serverPort());
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("断  开"));
        QTRY_VERIFY(listener.hasPendingConnections());
        auto *peer = listener.nextPendingConnection();
        QVERIFY(peer);
        const quint16 port = listener.serverPort();
        // 先关掉监听，重连必然失败，等待态才观察得到。本机回连太快，监听还在时
        // 按钮会在同一拍从「停止」回到「断开」。
        listener.close();
        peer->abort();
        peer->deleteLater();

        QTRY_COMPARE_WITH_TIMEOUT(action->text(), QStringLiteral("等待中"), 4000);
        QCOMPARE(action->property("recovering").toBool(), true);
        auto *statusB = window.findChildren<QLabel *>(QStringLiteral("EndpointState")).value(1);
        QVERIFY(statusB);
        QTRY_COMPARE(statusB->text(), QStringLiteral("等待恢复"));
        QVERIFY(!b->findChild<QComboBox *>(QStringLiteral("portType"))->isEnabled());

        const auto countErrors = [model] {
            int n = 0;
            for (int i = 0; i < model->rowCount(); ++i)
                if (model->record(i).direction == Traffic::Direction::Error) ++n;
            return n;
        };
        QTRY_VERIFY(countErrors() >= 1);
        const int errorsWhileWaiting = countErrors();
        auto *alerts = window.findChild<EndpointAlertState *>(QStringLiteral("endpointAlertsB"));
        QVERIFY(alerts);
        QCOMPARE(alerts->level(), EndpointAlertState::Error);
        alerts->acknowledge();
        QCOMPARE(alerts->level(), EndpointAlertState::Normal);
        QTest::qWait(1500);
        QCOMPARE(action->text(), QStringLiteral("等待中"));
        QCOMPARE(statusB->text(), QStringLiteral("等待恢复"));
        // 第一次失败已经说明原因，后续每拍不再追加错误行。
        QCOMPARE(countErrors(), errorsWhileWaiting);
        QCOMPARE(alerts->level(), EndpointAlertState::Normal);
        QVERIFY(!alerts->blinking());

        QVERIFY2(listener.listen(QHostAddress::LocalHost, port),
            qPrintable(listener.errorString()));
        QTRY_VERIFY_WITH_TIMEOUT(listener.hasPendingConnections(), 4000);
        QTRY_COMPARE(action->text(), QStringLiteral("断  开"));
        QVERIFY(b->draft().autoRecover);
        QTRY_VERIFY_WITH_TIMEOUT(!alerts->hasFault(), 3500);
        QCOMPARE(alerts->level(), EndpointAlertState::Normal);

        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("连  接"));
        QVERIFY(b->draft().autoRecover);
        QTest::qWait(1200);
        QCOMPARE(action->text(), QStringLiteral("连  接"));
        QCOMPARE(statusB->text(), QStringLiteral("未连接"));
    }

    void expandingCommandPanelKeepsFilterRowIntact()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *bar = window.findChild<QWidget *>(QStringLiteral("filterBar"));
        auto *toggleExpand = window.findChild<QPushButton *>("commandPanelToggle");
        auto *commandPanel = window.findChild<CommandPanel *>();
        QVERIFY(bar && toggleExpand && commandPanel);

        // 每条按钮记录"它自己声明的最小宽度"，展开后不许跌破。
        // 图标按钮（重置）没有文字，尺寸由 iconSize + QSS 内边距决定。
        QList<QPushButton *> barButtons = bar->findChildren<QPushButton *>();
        QVERIFY(!barButtons.isEmpty());
        QHash<QPushButton *, int> minimums;
        for (auto *button : barButtons) minimums.insert(button, button->minimumSizeHint().width());

        // 初次启动默认收起。先展开再收起，走一遍完整的状态切换。
        toggleExpand->setChecked(true);
        QTest::qWait(30);
        toggleExpand->setChecked(false);
        QTest::qWait(30);
        QVERIFY2(!bar->findChild<QPushButton *>("filterRebuild")->isVisibleTo(&window) == false,
            "过滤条本身不随指令面板收起");
        toggleExpand->setChecked(true);
        QTest::qWait(30);
        QVERIFY2(commandPanel->isVisibleTo(&window), "指令面板应展开");

        // 展开后：过滤条按钮不能被压到最小宽度以下。
        for (auto *button : barButtons) {
            QVERIFY2(button->width() >= minimums.value(button),
                qPrintable(QStringLiteral("过滤条按钮被挤压：%1 宽 %2 < 最小 %3")
                               .arg(button->objectName())
                               .arg(button->width())
                               .arg(minimums.value(button))));
        }
        // 第二行仍必须一行排得下（不重叠、不越右缘）——展开不该破坏这条铁律。
        const auto problem = overlapInRow(bar);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
    }

    void bridgeUsesOneFixedPair()
    {
        clearEndpointDrafts();
        MainWindow window;
        auto *a = window.findChild<PortSelectorWidget *>("endpointA");
        auto *direction = window.findChild<QComboBox *>("routeDirection");
        auto *bridge = window.findChild<QPushButton *>("bridgeButton");
        QVERIFY(a && direction && bridge);
        // The title and the pair label are gone: the direction options name the pair, so all
        // three Forward/Reverse/Bidirectional variants must be selectable.
        QVERIFY(!window.findChild<QLabel *>("BridgeTitle"));
        QVERIFY(!window.findChild<QLabel *>("BridgePair"));
        QCOMPARE(direction->count(), 3);
        QCOMPARE(direction->itemText(0), QStringLiteral("A → B"));
        QCOMPARE(direction->itemText(1), QStringLiteral("B → A"));
        QCOMPARE(direction->itemText(2), QStringLiteral("A ↔ B"));
        QCOMPARE(direction->currentIndex(), 2);
        QVERIFY(!bridge->icon().isNull());
        // Each option carries its own pictogram so the direction stays readable.
        for (int i = 0; i < direction->count(); ++i)
            QVERIFY(!direction->itemIcon(i).isNull());
        // No standing hint line: an unmet precondition shows up as a disabled button.
        QVERIFY(!window.findChild<QLabel *>("BridgeStatus"));
        // A starts as a serial endpoint with no device chosen, so the bridge cannot start.
        QVERIFY(!a->hasValidConfiguration());
        QVERIFY(!bridge->isEnabled());
        a->setSerialPorts({{"COM3", "USB-SERIAL CH340", {}, "COM3"}});
        auto *serial = a->findChild<QComboBox *>("serial.port");
        serial->setCurrentIndex(serial->findData("COM3"));
        QVERIFY(a->hasValidConfiguration());
        QVERIFY(bridge->isEnabled());
        // The full endpoint names live in the tooltip now that the pair label is gone.
        QVERIFY(bridge->toolTip().contains(QStringLiteral("A · 串口")));
        QVERIFY(bridge->toolTip().contains(QStringLiteral("B · TCP 服务器")));
        QCOMPARE(bridge->property("open").toBool(), false);
        bridge->click();
        QCOMPARE(bridge->text(), QStringLiteral("停止桥接"));
        QCOMPARE(bridge->property("open").toBool(), true);
        QCOMPARE(window.findChild<QLabel *>("RouteStatus")->text(), QStringLiteral("已建立"));
        QVERIFY(!direction->isEnabled());
        bridge->click();
        QCOMPARE(bridge->text(), QStringLiteral("建立桥接"));
        QCOMPARE(bridge->property("open").toBool(), false);
        QVERIFY(direction->isEnabled());
        // 桥接一旦建立，改参数或设备暂时不可用都不关闭，只能再点按钮。
        bridge->click();
        QCOMPARE(bridge->text(), QStringLiteral("停止桥接"));
        a->findChild<QComboBox *>("serial.baud")->setEditText("921600");
        QCOMPARE(bridge->text(), QStringLiteral("停止桥接"));
        QCOMPARE(bridge->property("open").toBool(), true);
        QVERIFY(!direction->isEnabled());
        QCOMPARE(window.findChild<QLabel *>("RouteStatus")->text(), QStringLiteral("已建立"));
        a->setSerialPorts({});
        QVERIFY(!a->hasValidConfiguration());
        QCOMPARE(bridge->text(), QStringLiteral("停止桥接"));
        QVERIFY(bridge->isEnabled());
        bridge->click();
        QCOMPARE(bridge->text(), QStringLiteral("建立桥接"));
        QVERIFY(!bridge->isEnabled());
        window.showRoutes(); // The --routes entry point focuses the always-visible action.
        QVERIFY(bridge->hasFocus() || !bridge->isEnabled());

        // 对齐量的是桥接未启动、端口有效时的静止布局。
        a->setSerialPorts({{"COM3", "USB-SERIAL CH340", {}, "COM3"}});
        serial->setCurrentIndex(serial->findData("COM3"));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(direction->width(), bridge->width());
        auto *b = window.findChild<PortSelectorWidget *>("endpointB");
        QVERIFY(b);
        const auto aligned = [](QWidget *left, QWidget *right) {
            return qAbs(left->mapTo(left->window(), QPoint(0, 0)).y()
                        - right->mapTo(right->window(), QPoint(0, 0)).y()) <= 1;
        };
        // 顶边允许差 1px：分割线取整后，桥接行和端点行会差这么多，肉眼可忽略。
        QVERIFY(aligned(direction, a->findChild<QPushButton *>("endpointAction")));
        QVERIFY(aligned(bridge, b->findChild<QPushButton *>("endpointAction")));
    }

    // 建立和停止桥接都要在数据区留下状态行。状态栏「已建立」不算这条日志。
    void bridgeWritesStatusLog()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        auto *a = window.findChild<PortSelectorWidget *>("endpointA");
        auto *bridge = window.findChild<QPushButton *>("bridgeButton");
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(a && bridge && text);
        a->setSerialPorts({{"COM3", "USB-SERIAL CH340", {}, "COM3"}});
        auto *serial = a->findChild<QComboBox *>("serial.port");
        serial->setCurrentIndex(serial->findData("COM3"));
        QVERIFY(bridge->isEnabled());
        bridge->click();
        QTRY_VERIFY_WITH_TIMEOUT(text->toPlainText().contains(QStringLiteral("桥接已建立")), 2000);
        const QString established = text->toPlainText();
        QVERIFY(established.contains(QStringLiteral("A · COM3")));
        QVERIFY(established.contains(QStringLiteral("↔")));
        QVERIFY(established.contains(QStringLiteral("B · 0.0.0.0:9000")));
        bridge->click();
        QTRY_VERIFY_WITH_TIMEOUT(text->toPlainText().contains(QStringLiteral("桥接已停止")), 2000);
    }

    void endpointActionOpensAndCloses()
    {
        // 端到端接线：点击动作按钮 → 运行时监听 → 状态回到按钮文案；
        // 再次点击关闭。全程不需要真实硬件（TCP 服务器走本机监听）。
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        auto *b = window.findChild<PortSelectorWidget *>("endpointB");
        QVERIFY(b);
        QVERIFY(b->hasValidConfiguration()); // TCP 服务器默认监听所有 IPv4 网卡的 9000 端口
        auto *action = b->findChild<QPushButton *>("endpointAction");
        QCOMPARE(action->text(), QStringLiteral("监  听"));
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("停  止"));
        // 打开期间配置被锁定：不能边连边改。
        QVERIFY(!b->findChild<QComboBox *>("portType")->isEnabled());
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("监  听"));
        QVERIFY(b->findChild<QComboBox *>("portType")->isEnabled());

        // TCP 客户端拨号期间按钮改成「正在连接」，类型和参数先锁住；连上之后才变红。
        auto *a = window.findChild<PortSelectorWidget *>("endpointA");
        QVERIFY(a);
        a->findChild<QComboBox *>("portType")->setCurrentIndex(1);
        auto *clientAction = a->findChild<QPushButton *>("endpointAction");
        QCOMPARE(clientAction->text(), QStringLiteral("连  接"));
        QCOMPARE(clientAction->property("open").toBool(), false);
        a->setConnectionOpen(false, true);
        QCOMPARE(clientAction->text(), QStringLiteral("正在连接"));
        QCOMPARE(clientAction->property("open").toBool(), false);
        QVERIFY(!a->findChild<QComboBox *>("portType")->isEnabled());
        a->setConnectionOpen(true);
        QCOMPARE(clientAction->text(), QStringLiteral("断  开"));
        QCOMPARE(clientAction->property("open").toBool(), true);
        a->setConnectionOpen(false);
        QCOMPARE(clientAction->text(), QStringLiteral("连  接"));
        QVERIFY(a->findChild<QComboBox *>("portType")->isEnabled());
    }

    // 连接中改组帧不必断开。结束符还没写完时不能下发，弹窗说明，并退回上次生效的方式。
    void liveFramingRejectsAnIncompleteDelimiter()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(b && action);
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("停  止"));

        b->findChild<QToolButton *>(QStringLiteral("advancedToggle"))->click();
        auto *mode = b->findChild<QComboBox *>(QStringLiteral("framingMode"));
        auto *delimiter = b->findChild<QLineEdit *>(QStringLiteral("framingDelimiter"));
        QVERIFY(mode && delimiter);
        QString notice;
        QTimer::singleShot(0, &window, [&window, &notice] {
            auto *box = window.findChild<QMessageBox *>();
            QVERIFY(box);
            notice = box->text();
            box->accept();
        });
        mode->setCurrentIndex(mode->findData(int(ReceiveFraming::Delimiter)));
        QVERIFY(notice.contains(QStringLiteral("结束符还没写完")));
        QCOMPARE(mode->currentData().toInt(), int(ReceiveFraming::Passthrough));
        QCOMPARE(b->draft().framing.mode, ReceiveFraming::Passthrough);

        delimiter->setText(QStringLiteral("0D 0A"));
        mode->setCurrentIndex(mode->findData(int(ReceiveFraming::Delimiter)));
        QTest::qWait(30);
        QVERIFY(window.findChildren<QMessageBox *>().isEmpty());
        QCOMPARE(b->draft().framing.mode, ReceiveFraming::Delimiter);
        QCOMPARE(b->draft().framing.delimiter, QByteArray::fromHex("0D0A"));

        delimiter->setText(QStringLiteral("0"));
        QTimer::singleShot(0, &window, [&window] {
            if (auto *box = window.findChild<QMessageBox *>()) box->accept();
        });
        QTest::keyClick(delimiter, Qt::Key_Return);
        QCOMPARE(b->draft().framing.mode, ReceiveFraming::Delimiter);
        QVERIFY(!b->canApplyFraming());

        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("监  听"));
    }

    // 连接中只改结束符文本（回车或离开焦点）也要当场下发。
    // 早先这个输入框只在内容不合法时弹窗，合法时什么都不做：模式、开关、空闲超时、
    // 阈值都各自接了事件，唯独结束符没有，于是"改结束符"要断开重连才生效，
    // 与「端口已打开时改结束符不必断开」的承诺相反。
    void liveFramingAppliesDelimiterEdit()
    {
        PortSelectorWidget selectorWidget;
        selectorWidget.show();
        auto *selector = &selectorWidget;
        selector->setSerialPorts({{"COM17", "USB Serial", {}, "COM17"}});
        auto *portField = selector->findChild<QComboBox *>(QStringLiteral("serial.port"));
        portField->setCurrentIndex(portField->findData(QStringLiteral("COM17")));
        selector->findChild<QToolButton *>(QStringLiteral("advancedToggle"))->click();
        QTest::qWait(20);

        auto *mode = selector->findChild<QComboBox *>(QStringLiteral("framingMode"));
        auto *delimiter = selector->findChild<QLineEdit *>(QStringLiteral("framingDelimiter"));
        QVERIFY(mode && delimiter);
        // 已连接是这条规则的前提：未连接时控件照样广播，但"实时生效"说的是运行中。
        selector->setConnectionOpen(true);

        QSignalSpy changes(selector, &PortSelectorWidget::configurationChanged);
        delimiter->setText(QStringLiteral("0D 0A"));
        mode->setCurrentIndex(mode->findData(int(ReceiveFraming::Delimiter)));
        QTest::qWait(20);
        QCOMPARE(selector->draft().framing.delimiter, QByteArray::fromHex("0D0A"));
        QVERIFY(!changes.isEmpty());

        // 只改文本、回车：必须广播一次，并且草稿里换成新结束符。
        changes.clear();
        delimiter->setText(QStringLiteral("0A"));
        QTest::keyClick(delimiter, Qt::Key_Return);
        QTest::qWait(20);
        QCOMPARE(changes.size(), 1);
        QCOMPARE(selector->draft().framing.delimiter, QByteArray::fromHex("0A"));
        QCOMPARE(selector->draft().framing.mode, ReceiveFraming::Delimiter);
        QVERIFY(selectorWidget.findChildren<QMessageBox *>().isEmpty());

        // 内容没变时再回车不重复广播：否则每离开一次焦点都白跑一遍配置同步。
        changes.clear();
        QTest::keyClick(delimiter, Qt::Key_Return);
        QTest::qWait(20);
        QCOMPARE(changes.size(), 0);

        // 半截 HEX 回车：不下发，弹窗说明，运行中的规则保持不变。
        changes.clear();
        delimiter->setText(QStringLiteral("0"));
        QTimer::singleShot(0, &selectorWidget, [&selectorWidget] {
            if (auto *box = selectorWidget.findChild<QMessageBox *>()) box->accept();
        });
        QTest::keyClick(delimiter, Qt::Key_Return);
        QTest::qWait(20);
        QCOMPARE(changes.size(), 0);
        QCOMPARE(selector->draft().framing.mode, ReceiveFraming::Delimiter);
        QVERIFY(!selector->canApplyFraming());
    }


    void liveUdpFramingAppliesControlsWithoutRebind()
    {
        clearEndpointDrafts();
        QUdpSocket probe, sender;
        QVERIFY(probe.bind(QHostAddress::LocalHost, 0, QUdpSocket::DontShareAddress));
        const quint16 port = probe.localPort();
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *selector = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointA"));
        auto *model = window.findChild<TrafficModel *>();
        auto *sendPanel = window.findChild<SendPanel *>();
        QVERIFY(selector && model && sendPanel);
        auto *type = selector->findChild<QComboBox *>(QStringLiteral("portType"));
        QVERIFY(type);
        type->setCurrentIndex(PortDraft::Udp);
        auto *bind = selector->findChild<QComboBox *>(QStringLiteral("udp.address"));
        auto *bindPort = selector->findChild<QSpinBox *>(QStringLiteral("udp.port"));
        auto *target = selector->findChild<QComboBox *>(QStringLiteral("udp.target"));
        auto *targetPort = selector->findChild<QSpinBox *>(QStringLiteral("udp.targetPort"));
        auto *action = selector->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(bind && bindPort && target && targetPort && action);
        if (bind->findData(QStringLiteral("127.0.0.1")) < 0)
            bind->addItem(QStringLiteral("127.0.0.1"), QStringLiteral("127.0.0.1"));
        bind->setCurrentIndex(bind->findData(QStringLiteral("127.0.0.1")));
        bindPort->setValue(port);
        target->setCurrentText(QStringLiteral("127.0.0.1"));
        targetPort->setValue(sender.localPort());
        probe.close(); // 配置完成才释放探测端口；sender 也不会碰巧占用该端口。
        action->click();
        QTRY_COMPARE_WITH_TIMEOUT(action->text(), QStringLiteral("解  绑"), 5000);
        QVERIFY(action->property("open").toBool());
        auto *advanced = selector->findChild<QToolButton *>(QStringLiteral("advancedToggle"));
        QVERIFY(advanced);
        if (!advanced->isChecked()) advanced->click();
        auto *enabled = selector->findChild<QCheckBox *>(QStringLiteral("framingEnabled"));
        auto *mode = selector->findChild<QComboBox *>(QStringLiteral("framingMode"));
        auto *delimiter = selector->findChild<QLineEdit *>(QStringLiteral("framingDelimiter"));
        auto *idle = selector->findChild<QSpinBox *>(QStringLiteral("framingIdle"));
        auto *keep = selector->findChild<QCheckBox *>(QStringLiteral("framingKeepBoundary"));
        QVERIFY(enabled && mode && delimiter && idle && keep);
        QVERIFY(enabled->isEnabled());
        idle->setValue(30000); // 大于所有等待上限，不能把 idle 兜底误认为结束符已生效。
        keep->setChecked(false);
        delimiter->setText(QStringLiteral("0D 0A"));
        enabled->setChecked(true);
        mode->setCurrentIndex(mode->findData(int(ReceiveFraming::Delimiter)));
        QCOMPARE(selector->draft().framing.mode, ReceiveFraming::Delimiter);

        const auto received = [model] {
            QList<Traffic::Message> result;
            for (int row = 0; row < model->rowCount(); ++row) {
                const auto message = model->record(row);
                if (message.direction == Traffic::Direction::Receive && message.source.endpointIndex == 0)
                    result.append(message);
            }
            return result;
        };
        const QList<QByteArray> inputs = {"first\r\n", "second\r\nEND", "native\r\nEND"};
        const QList<QByteArray> expected = {"first", "second\r\n", "native\r\nEND"};
        for (int step = 0; step < inputs.size(); ++step) {
            if (step == 1) {
                // 只改结束符文本并回车，不触碰模式或开关。
                delimiter->setText(QStringLiteral("45 4E 44"));
                QTest::keyClick(delimiter, Qt::Key_Return);
                QCOMPARE(selector->draft().framing.delimiter, QByteArray("END"));
            } else if (step == 2) {
                enabled->setChecked(false);
                QCOMPARE(selector->draft().framing.mode, ReceiveFraming::Passthrough);
            }
            // UI 配置更新和 sendPayload 同由 GUI 排队。收到这条真实 UDP 回包，
            // 证明之前的更新已执行；不用 sleep，也不跨线程读取运行时/socket。
            const QByteArray barrier = QByteArray("framing-barrier-") + QByteArray::number(step);
            sendPanel->sendRequested(barrier, 1, false);
            QTRY_VERIFY_WITH_TIMEOUT(sender.hasPendingDatagrams(), 5000);
            QByteArray reply(sender.pendingDatagramSize(), Qt::Uninitialized);
            QHostAddress replyAddress;
            quint16 replyPort = 0;
            QCOMPARE(sender.readDatagram(reply.data(), reply.size(), &replyAddress, &replyPort),
                qint64(barrier.size()));
            QCOMPARE(reply, barrier);
            QCOMPARE(replyPort, port);
            QCOMPARE(sender.writeDatagram(inputs[step], QHostAddress::LocalHost, port),
                qint64(inputs[step].size()));
            QTRY_COMPARE_WITH_TIMEOUT(received().size(), step + 1, 5000);
            const auto message = received().at(step);
            QCOMPARE(message.raw, expected[step]);
            QCOMPARE(message.framing, step == 2 ? Traffic::Framing::Datagram : Traffic::Framing::Message);
            QVERIFY(!message.hasMoreFragments);
            QCOMPARE(message.source.peerAddress, QStringLiteral("127.0.0.1:%1").arg(sender.localPort()));
            QCOMPARE(message.source.generation, received().first().source.generation);
            QCOMPARE(action->text(), QStringLiteral("解  绑"));
            QVERIFY(action->property("open").toBool());
            QVERIFY(!type->isEnabled());
        }
        QVERIFY(window.findChildren<QMessageBox *>().isEmpty());
        action->click();
        QTRY_COMPARE_WITH_TIMEOUT(action->text(), QStringLiteral("绑  定"), 5000);
        QCOMPARE(received().size(), 3);
    }

    void compactWindowAndExamples()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        window.loadExample();
        auto *text = window.findChild<TrafficTextView *>("trafficText");
        auto *model = window.findChild<TrafficModel *>();
        auto *editor = window.findChild<QPlainTextEdit *>("sendEditor");
        QVERIFY(text && model && editor);
        // 显示格式下拉框逐项来自共享 PayloadCodec，与发送区同一套列表与规则。
        auto *trafficFormat = window.findChild<QComboBox *>("trafficFormat");
        QVERIFY(trafficFormat);
        const auto formats = PayloadCodec::formats();
        QCOMPARE(trafficFormat->count(), formats.size());
        for (int i = 0; i < formats.size(); ++i)
            QCOMPARE(trafficFormat->itemText(i), PayloadCodec::formatName(formats.at(i)));
        QCOMPARE(trafficFormat->currentText(), QStringLiteral("UTF-8"));
        auto *splitterProbe = window.findChild<QSplitter *>("dataSplitter");
        QVERIFY(splitterProbe);
        qInfo("SPLITPROBE sizes=%d/%d mins=%d/%d hints=%d/%d",
              splitterProbe->sizes().value(0), splitterProbe->sizes().value(1),
              splitterProbe->widget(0)->minimumSizeHint().height(),
              splitterProbe->widget(1)->minimumSizeHint().height(),
              splitterProbe->widget(0)->sizeHint().height(),
              splitterProbe->widget(1)->sizeHint().height());
        QCOMPARE(model->rowCount(), 12);
        // 条数摘要搬进了主窗口状态栏：示例加载完成后状态栏同步播报会话累计。
        QCOMPARE(window.findChild<QLabel *>(QStringLiteral("TrafficStatus"))->text(),
                 QStringLiteral("共 12 条"));
        QVERIFY(window.rect().contains(QRect(editor->mapTo(&window, QPoint()), editor->size())));
        // 格式切换是"外观"：按契约立刻作用于**已收到/已显示**的行，新到达的消息也按新格式渲染
        // （r5 起不再是"只影响新行"的控制台语义）。切完先等掉那一刻必然发生的防抖重投影。
        // 已知字节流片段（HEX 01 03 00 00 00 02 C4 0B）渲染为 DEC，并带 ⏎ 续接标记——边界是消息的事实。
        // 始终显示 HEX 缺省关闭。这里再关一次，保证一行一条。
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(false);
        trafficFormat->setCurrentIndex(1); // DEC
        QTest::qWait(AppearanceReprojectWaitMs); // 设计上必然发生的一次重投影，等掉它再断言
        const QUuid endpointId = window.findChild<QPushButton *>("showPortA")
                                     ->property("endpoint").toUuid();
        Traffic::Message dec;
        dec.id = 100000;
        dec.timestamp = QDateTime::currentDateTime();
        dec.direction = Traffic::Direction::Receive;
        dec.source.endpointId = endpointId;
        dec.source.endpointIndex = 0;
        dec.source.endpointKind = PortDraft::Serial;
        dec.framing = Traffic::Framing::Stream;
        dec.hasMoreFragments = true;
        dec.raw = QByteArray::fromHex("010300000002c40b");
        dec.rawLength = dec.raw.size();
        window.findChild<TrafficPanel *>()->appendMessages({dec});
        QTRY_COMPARE(model->rowCount(), 13);
        QCOMPARE(text->lineCount(), 13);
        const QString decLine = text->document()->lastBlock().text();
        QVERIFY2(decLine.endsWith(QStringLiteral("1 3 0 0 0 2 196 11 ⏎")),
                 qPrintable(QStringLiteral("DEC 渲染行不符：") + decLine));
        trafficFormat->setCurrentIndex(0); // 恢复 HEX，后续消息按 HEX 渲染与搜索
        QTest::qWait(AppearanceReprojectWaitMs); // 切格式必然触发的重投影，先等掉
        // 搜索按到达时过滤：命中的记录进文本历史，未命中的只进模型不进显示。
        auto *search = window.findChild<QLineEdit *>("filterKeywords");
        Traffic::Message hit;
        hit.id = 100001;
        hit.timestamp = QDateTime::currentDateTime();
        hit.direction = Traffic::Direction::Receive;
        hit.source.endpointId = endpointId;
        hit.source.endpointIndex = 0;
        hit.source.endpointKind = PortDraft::Serial;
        hit.raw = QByteArray::fromHex("AABB");
        hit.rawLength = hit.raw.size();
        window.findChild<TrafficPanel *>()->appendMessages({hit});
        QTRY_COMPARE(model->rowCount(), 14);
        QCOMPARE(text->lineCount(), 14);
        // 搜索按到达时生效：设一个只命中 HEX "AA BB" 的关键词，后续未命中记录不进显示。
        search->setText(QStringLiteral("aa bb"));
        Traffic::Message miss = hit;
        miss.id = 100002;
        miss.raw = QByteArray::fromHex("CCDD");
        miss.rawLength = miss.raw.size();
        window.findChild<TrafficPanel *>()->appendMessages({miss});
        QTRY_COMPARE(model->rowCount(), 15); // 模型照单全收：导出与记录不依赖显示过滤
        QCOMPARE(text->lineCount(), 14);     // 搜索未命中 → 不进显示
        search->clear();
        window.findChild<QPushButton *>("clearTraffic")->click();
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(text->lineCount(), 1); // 空文档保底的一个空块
    }

    void receiveFramingValueSemantics()
    {
        // 接收组帧是端点配置值类型：默认参数可直接使用，非法组合必须被 isValid 拦下。
        using Framing = ReceiveFraming;
        const Framing defaults;
        QVERIFY(defaults.isValid());
        QCOMPARE(defaults.mode, Framing::Passthrough); // 默认仅兜底输出，不改逐包语义
        QCOMPARE(defaults.idleTimeoutMs, 10);
        QCOMPARE(defaults.bufferThresholdBytes, 2048);
        QVERIFY(!defaults.includeDelimiter);           // 结束符默认剥离

        // Delimiter 模式必须有非空结束符；超时与阈值必须为正。
        Framing delimiter;
        delimiter.mode = Framing::Delimiter;
        QVERIFY(!delimiter.isValid());
        delimiter.delimiter = QByteArray::fromHex("0D0A");
        QVERIFY(delimiter.isValid());
        Framing idle;
        idle.mode = Framing::IdleTimeout;
        QVERIFY(idle.isValid());
        Framing badTimeout = delimiter;
        badTimeout.idleTimeoutMs = 0;
        QVERIFY(!badTimeout.isValid());
        Framing badThreshold = delimiter;
        badThreshold.bufferThresholdBytes = -1;
        QVERIFY(!badThreshold.isValid());

        // 帧内容开关参与相等性：组帧器据此决定输出帧携带什么。
        Framing stripped = delimiter;
        stripped.includeDelimiter = true;
        QVERIFY(stripped != delimiter);        // 唯一差异是开关，必须判为不等
        Framing same = stripped;
        QVERIFY(same == stripped);             // 其余字段逐项相同则相等

        // 与参考实现的分级对齐：警告是独立方向，有自己的显示名，非数据消息不进统计。
        Traffic::Message warning;
        warning.direction = Traffic::Direction::Warning;
        QVERIFY(!warning.isData());
        QCOMPARE(Traffic::directionName(warning.direction), QStringLiteral("W"));
        Traffic::Message info;
        info.direction = Traffic::Direction::Info;
        QCOMPARE(Traffic::directionName(info.direction), QStringLiteral("I"));
        Traffic::Message stateName;
        stateName.direction = Traffic::Direction::State;
        QCOMPARE(Traffic::directionName(stateName.direction), QStringLiteral("S"));
        Traffic::Message errorName;
        errorName.direction = Traffic::Direction::Error;
        QCOMPARE(Traffic::directionName(errorName.direction), QStringLiteral("E"));
    }

    // 文本视图的安全阀触发时同样必须播报：屏幕上看不到的几行，可能是用户唯一想
    // 回看的东西。默认预算是 400 万块（用例填不满），所以显式注入小预算验机制。
    void textViewBudgetReleaseIsReported()
    {
        TrafficTextView view;
        view.setBudget(50, 1024 * 1024);
        QList<Traffic::Message> batch;
        batch.reserve(60);
        for (int i = 0; i < 60; ++i) {
            Traffic::Message message;
            message.id = i + 1;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = QUuid::createUuid();
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray("line one\r\nline two\r\n");
            message.rawLength = message.raw.size();
            batch.append(message);
        }
        view.appendMessages(batch, PayloadCodec::Format::Utf8, TrafficTextView::DisplayOptions());

        QVERIFY2(view.lineCount() <= 50,
            qPrintable(QStringLiteral("行数应被压回预算内：%1").arg(view.lineCount())));
        QVERIFY2(view.releasedBlocks() > 0, "触顶释放必须记账——释放不许无声");
        // 释放只从最旧一端发生：第 1 条已被挤掉，最新一条一定还在。
        QVERIFY2(view.lineNumberAt(0) != 1,
            qPrintable(QStringLiteral("首块应已不是第 1 条：%1").arg(view.lineNumberAt(0))));
        QVERIFY2(view.document()->toPlainText().contains(QStringLiteral("line one")),
                 "最新一条必须还在");
        // 每条是「line one」「line two」再加末尾换行留下的空行，共 3 块。
        // 60 条 180 块，预算 50，裁掉 130 块：第 44 条的首行被裁掉，
        // 留下的第一块是它的续行，槽上留白。完整留下的下一条是第 45 条。
        // 末块是第 60 条以换行收尾的空行，也留白。
        QCOMPARE(view.lineNumberAt(0), quint64(0));
        QCOMPARE(view.lineNumberAt(2), quint64(45));
        QCOMPARE(view.lineNumberAt(view.document()->blockCount() - 3), quint64(60));
        QCOMPARE(view.lineNumberAt(view.document()->blockCount() - 1), quint64(0));

        // 字符预算单独裁掉头部时，行号只能记一次。记两次会让后面的行号变成 0 或跳号。
        TrafficTextView byChars;
        byChars.setBudget(1000, 64);
        QList<Traffic::Message> wide;
        wide.reserve(30);
        for (int i = 0; i < 30; ++i) {
            Traffic::Message message;
            message.id = i + 1;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray("0123456789abcdef");
            message.rawLength = message.raw.size();
            wide.append(message);
        }
        byChars.appendMessages(wide, PayloadCodec::Format::Utf8, TrafficTextView::DisplayOptions());
        const int kept = byChars.document()->blockCount();
        QVERIFY(kept >= 1);
        QVERIFY(byChars.releasedBlocks() > 0);
        QCOMPARE(byChars.lineNumberAt(kept - 1), byChars.lineNumberAt(0) + quint64(kept - 1));

        // 超过行号表一次回收的量之后，行号和端口仍然对齐。回收不能把号跳掉或错位。
        TrafficTextView many;
        many.setBudget(20, 1024 * 1024);
        QList<Traffic::Message> rows;
        rows.reserve(1100);
        for (int i = 0; i < 1100; ++i) {
            Traffic::Message message;
            message.id = i + 1;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointIndex = i % 2;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray("x");
            message.rawLength = 1;
            rows.append(message);
        }
        many.appendMessages(rows, PayloadCodec::Format::Utf8, TrafficTextView::DisplayOptions());
        QCOMPARE(many.document()->blockCount(), 20);
        QCOMPARE(many.lineNumberAt(0), quint64(1081));
        QCOMPARE(many.lineNumberAt(19), quint64(1100));
        QCOMPARE(many.lineEndpointAt(0), 0);
        QCOMPARE(many.lineEndpointAt(19), 1);
    }

    // 行号槽默认按两位留白。空文档、1 位和 2 位都是这个宽度；到 100 才再加一列。
    void lineNumberGutterGrowsWithDigits()
    {
        TrafficTextView view;
        view.resize(640, 240);
        const auto widthFor = [&](int count) {
            view.clearAll();
            if (count > 0) {
                QList<Traffic::Message> rows;
                rows.reserve(count);
                for (int i = 0; i < count; ++i) {
                    Traffic::Message message;
                    message.id = quint64(i + 1);
                    message.timestamp = QDateTime::currentDateTime();
                    message.direction = Traffic::Direction::Receive;
                    message.source.endpointIndex = 0;
                    message.source.endpointKind = PortDraft::Serial;
                    message.raw = QByteArray("x");
                    message.rawLength = 1;
                    rows.append(message);
                }
                view.appendMessages(rows, PayloadCodec::Format::Utf8,
                    TrafficTextView::DisplayOptions());
            }
            return view.gutterWidth();
        };
        const int twoDigits = widthFor(0);
        QVERIFY(twoDigits > 0);
        QCOMPARE(widthFor(9), twoDigits);
        QCOMPARE(widthFor(99), twoDigits);
        QVERIFY2(widthFor(100) > twoDigits,
            qPrintable(QStringLiteral("2位 %1，100 行 %2，末行号 %3，块数 %4")
                .arg(twoDigits)
                .arg(view.gutterWidth())
                .arg(view.lineNumberAt(view.document()->blockCount() - 1))
                .arg(view.document()->blockCount())));
        // 头部淘汰后槽看的是还在的最大行号，不是留下的行数。
        view.setBudget(5, 1024 * 1024);
        const int afterTrim = widthFor(100);
        QVERIFY(afterTrim > twoDigits);
        QCOMPARE(view.lineNumberAt(view.document()->blockCount() - 1), quint64(100));
    }

    // 行号和正文必须落在同一条基线上。垂直居中会让号比正文高出一截。
    void lineNumbersShareTheTextBaseline()
    {
        QFile styleFile(QStringLiteral(":/styles/workbench.qss"));
        QVERIFY(styleFile.open(QIODevice::ReadOnly));
        const QString previousStyle = qApp->styleSheet();
        qApp->setStyleSheet(QString::fromUtf8(styleFile.readAll()));

        QWidget host;
        host.setAttribute(Qt::WA_DontShowOnScreen);
        host.resize(900, 240);
        auto *view = new TrafficTextView(&host);
        view->setGeometry(0, 0, 900, 240);
        host.show();
        QTest::qWait(40);

        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Serial;
        message.raw = QByteArray("22:23:35.574 ABC");
        message.rawLength = message.raw.size();
        view->appendMessages({message}, PayloadCodec::Format::Utf8, TrafficTextView::DisplayOptions());
        QCoreApplication::processEvents();

        const QImage image = host.grab().toImage();
        int contentTop = -1;
        int contentBottom = image.height();
        for (int y = 0; y < image.height(); ++y) {
            bool ink = false;
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).lightness() < 180) {
                    ink = true;
                    break;
                }
            }
            if (ink && contentTop < 0) contentTop = y;
            else if (!ink && contentTop >= 0) {
                contentBottom = y;
                break;
            }
        }
        QVERIFY(contentTop >= 0);

        // 第一行里从左到右是行号、端口色标、正文。两端的墨迹中心必须落在同一高度。
        QVector<int> count(image.width());
        QVector<qint64> ySum(image.width());
        for (int y = contentTop; y < contentBottom; ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).lightness() >= 180) continue;
                ++count[x];
                ySum[x] += y;
            }
        }
        int first = -1;
        int firstEnd = -1;
        int last = -1;
        int lastEnd = -1;
        int run = -1;
        for (int x = 0; x <= image.width(); ++x) {
            const bool ink = x < image.width() && count.at(x) > 0;
            if (ink && run < 0) run = x;
            if (!ink && run >= 0) {
                if (first < 0) {
                    first = run;
                    firstEnd = x;
                }
                last = run;
                lastEnd = x;
                run = -1;
            }
        }
        const auto center = [&](int x0, int x1) {
            qint64 sum = 0;
            int n = 0;
            for (int x = x0; x < x1; ++x) {
                sum += ySum.at(x);
                n += count.at(x);
            }
            return n == 0 ? -1.0 : double(sum) / double(n);
        };
        const double numberY = center(first, firstEnd);
        const double bodyY = center(last, lastEnd);
        qApp->setStyleSheet(previousStyle);
        QVERIFY2(first >= 0 && last > firstEnd,
            qPrintable(QStringLiteral("没分开行号和正文：%1 %2").arg(first).arg(last)));
        QVERIFY2(qAbs(numberY - bodyY) <= 1.5,
            qPrintable(QStringLiteral("行号与正文不在一条线上：号 %1 正文 %2")
                           .arg(numberY, 0, 'f', 2)
                           .arg(bodyY, 0, 'f', 2)));

        // 端口色标是行号右侧第一段墨迹，不能把后面的正文算进去。
        int markX0 = -1;
        int markX1 = -1;
        int markRun = -1;
        for (int x = firstEnd; x <= last; ++x) {
            const bool ink = x < last && count.at(x) > 0;
            if (ink && markRun < 0) markRun = x;
            if (!ink && markRun >= 0) {
                markX0 = markRun;
                markX1 = x;
                break;
            }
        }
        const double markY = center(markX0, markX1);
        QVERIFY2(markX0 >= 0,
            qPrintable(QStringLiteral("行号和正文之间应有端口色标：%1 %2")
                           .arg(firstEnd).arg(last)));
        QVERIFY2(qAbs(markY - bodyY) <= 1.5,
            qPrintable(QStringLiteral("端口色标与正文不在一条线上：标 %1 正文 %2")
                           .arg(markY, 0, 'f', 2)
                           .arg(bodyY, 0, 'f', 2)));

        // 中心对齐还不够：色标若只画到基线，底边会停在字的中下段。上下沿都要贴着行号墨迹。
        const auto edge = [&](int x0, int x1) {
            int top = -1;
            int bottom = -1;
            for (int y = contentTop; y < contentBottom; ++y) {
                bool hit = false;
                for (int x = x0; x < x1; ++x) {
                    if (image.pixelColor(x, y).lightness() >= 180) continue;
                    hit = true;
                    break;
                }
                if (!hit) continue;
                if (top < 0) top = y;
                bottom = y;
            }
            return qMakePair(top, bottom);
        };
        const auto numberEdge = edge(first, firstEnd);
        const auto markEdge = edge(markX0, markX1);
        QVERIFY2(qAbs(markEdge.first - numberEdge.first) <= 1
                     && qAbs(markEdge.second - numberEdge.second) <= 1,
            qPrintable(QStringLiteral("端口色标没有盖住行号：标 %1..%2 号 %3..%4")
                           .arg(markEdge.first)
                           .arg(markEdge.second)
                           .arg(numberEdge.first)
                           .arg(numberEdge.second)));
    }

    // 文本显示模式的三条底线：高速追加后跟随钉底；模型触顶淘汰后文本历史原样保留；
    // 停止跟随滚回顶部，第 1 条仍可回滚显示——这正是 JCom 34 万行不卡的行为基线。
    void textViewFollowsRetainsAndRollsBack()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("HEX")); // 本用例按固定字节内容核对首行。
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *follow = window.findChild<QPushButton *>(QStringLiteral("followTraffic"));
        QVERIFY(text && model && follow);

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        // 模型的安全阀默认 2000 万条，本用例要验的是"模型淘汰、文本原样保留"，
        // 所以显式压小模型预算。文本视图的安全阀比这大两个数量级，一行都不该少。
        const int rowBudget = 5000;
        model->setBudget(64LL * 1024 * 1024, rowBudget);
        const int total = rowBudget + 5000;
        for (int batchStart = 0; batchStart < total; batchStart += 500) {
            QList<Traffic::Message> batch;
            batch.reserve(500);
            for (int i = 0; i < 500; ++i) {
                Traffic::Message message;
                message.id = batchStart + i + 1;
                message.timestamp = QDateTime::currentDateTime();
                message.direction = (batchStart + i) % 2 ? Traffic::Direction::Transmit
                                                         : Traffic::Direction::Receive;
                message.source.endpointId = endpointId;
                message.source.endpointIndex = 0;
                message.source.endpointKind = PortDraft::Serial;
                message.raw = QByteArray::fromHex("0102030405060708");
                message.rawLength = message.raw.size();
                batch.append(message);
            }
            window.findChild<TrafficPanel *>()->appendMessages(batch);
            // 分批推进，模拟实际到达节奏；单批超限另有独立用例。
            QTest::qWait(20);
        }
        // 攒批是 16ms 异步的；空表上 record(-1) 是未定义行为，先等首条落地。
        QTRY_VERIFY(model->rowCount() > 0);
        QTRY_COMPARE(model->record(model->rowCount() - 1).id, quint64(total));

        // 跟随开启：追加后钉在底部。
        QVERIFY(follow->isChecked());
        QCOMPARE(text->verticalScrollBar()->value(), text->verticalScrollBar()->maximum());

        // 模型触顶淘汰，文本历史一行不少：这是"文本模式能看完整历史"的判据。
        QVERIFY2(model->rowCount() <= rowBudget,
            qPrintable(QStringLiteral("模型应已淘汰：当前 %1 条").arg(model->rowCount())));
        QCOMPARE(text->lineCount(), total);
        QCOMPARE(text->document()->blockCount(), total);
        // 淘汰**必须播报**：缓冲区已满要进状态栏摘要，用户才不会以为是自己看错了。
        // 这正是压测时"拖到最前面发现少了"唯一能被察觉的出口。
        auto *status = window.findChild<QLabel *>(QStringLiteral("TrafficStatus"));
        QVERIFY(status);
        QTRY_VERIFY2(status->text().contains(QStringLiteral("缓冲区已满")),
            qPrintable(QStringLiteral("摘要应播报缓冲区已满：") + status->text()));

        // 向上翻阅自动解除跟随。滚回底部再自动打开。
        auto *bar = text->verticalScrollBar();
        bar->triggerAction(QAbstractSlider::SliderPageStepSub);
        QVERIFY(!follow->isChecked());
        bar->triggerAction(QAbstractSlider::SliderToMaximum);
        QVERIFY(follow->isChecked());
        QCOMPARE(bar->value(), bar->maximum());

        // 拖动滑块到底部同样恢复跟随。
        bar->triggerAction(QAbstractSlider::SliderPageStepSub);
        QVERIFY(!follow->isChecked());
        QStyleOptionSlider opt;
        opt.initFrom(bar);
        opt.orientation = bar->orientation();
        opt.minimum = bar->minimum();
        opt.maximum = bar->maximum();
        opt.sliderPosition = bar->sliderPosition();
        opt.sliderValue = bar->value();
        opt.singleStep = bar->singleStep();
        opt.pageStep = bar->pageStep();
        opt.upsideDown = bar->invertedAppearance();
        const QRect grip = bar->style()->subControlRect(
            QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarSlider, bar);
        QVERIFY2(grip.isValid() && grip.height() > 0,
            qPrintable(QStringLiteral("滚动条滑块应可拖：%1x%2").arg(grip.width()).arg(grip.height())));
        const QPoint from = grip.center();
        const QPoint to(from.x(), bar->height() - 3);
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(bar, to);
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, to);
        QVERIFY2(follow->isChecked(),
            qPrintable(QStringLiteral("拖到底部应恢复滚动：位置 %1 / %2")
                           .arg(bar->value()).arg(bar->maximum())));

        // 再往上翻，才能停在顶部看第 1 条。
        bar->triggerAction(QAbstractSlider::SliderPageStepSub);
        QVERIFY(!follow->isChecked());
        bar->setValue(0);
        QCOMPARE(text->verticalScrollBar()->value(), 0);
        const QString firstLine = text->document()->firstBlock().text();
        QVERIFY2(firstLine.contains(QStringLiteral("01 02 03 04 05 06 07 08")),
                 qPrintable(QStringLiteral("第 1 行应是首条记录：") + firstLine));
        QCOMPARE(text->lineNumberAt(0), quint64(1));

        // 重新跟随：一键回底，继续钉住最新记录。
        follow->setChecked(true);
        QCOMPARE(text->verticalScrollBar()->value(), text->verticalScrollBar()->maximum());

        // 清空是会话边界：行号从 1 重新计数。
        window.findChild<QPushButton *>("clearTraffic")->click();
        QCOMPARE(text->lineCount(), 1); // 空文档保底的一个空块
        QList<Traffic::Message> afterClear;
        for (int i = 0; i < 3; ++i) {
            Traffic::Message message;
            message.id = i + 1;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray::fromHex("AA");
            message.rawLength = message.raw.size();
            afterClear.append(message);
        }
        window.findChild<TrafficPanel *>()->appendMessages(afterClear);
        QTRY_COMPARE(text->lineCount(), 3); // 首行复用空文档保底块，3 条即 3 块
        QCOMPARE(text->lineNumberAt(0), quint64(1));
    }

    // 报文里的 \r\n 必须还原成真实换行，而不是转义字面量。
    // 有前缀时一条记录一个行号，续行槽上留白，下一条接着编号。
    void textViewKeepsRealLineBreaks()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(text);
        // 换行语义属于文本类渲染：HEX/DEC 本身没有换行概念，切到 UTF-8 验证。
        // 始终显示 HEX 缺省关闭。这里再关一次，块数才等于报文本身的行数。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))->setCurrentText(QStringLiteral("UTF-8"));
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(false);

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.source.endpointId = endpointId;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Serial;
        message.raw = QByteArray("OK\r\nnext");
        message.rawLength = message.raw.size();
        window.findChild<TrafficPanel *>()->appendMessages({message});

        // QTRY 重算的是内联表达式，局部变量会被冻结——先等块数落地，再取文本。
        QTRY_COMPARE(text->document()->blockCount(), 2);
        const QStringList lines = text->toPlainText().split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 2);
        QVERIFY2(lines.at(0).trimmed().endsWith(QStringLiteral("OK")),
                 qPrintable(QStringLiteral("首行应以 OK 结尾：") + lines.at(0)));
        QCOMPARE(text->lineNumberAt(0), quint64(1));
        QCOMPARE(text->lineNumberAt(1), quint64(0)); // 续行跟这条前缀，槽上留白
        QCOMPARE(lines.at(1), QStringLiteral("next")); // 续行正文不带前缀，行号槽留白

        // 下一条记录接着编号。续行不把号吃掉。
        message.id = 2;
        message.raw = QByteArray("more");
        message.rawLength = message.raw.size();
        window.findChild<TrafficPanel *>()->appendMessages({message});
        QTRY_COMPARE(text->document()->blockCount(), 3);
        QCOMPARE(text->lineNumberAt(2), quint64(2));
        QVERIFY2(!text->toPlainText().contains(QStringLiteral("\\r\\n")),
                 "换行不能再以转义字面量出现");
    }

    // 超长报文在控制台完整显示：单行不再封顶（完整显示是显式需求），
    // 内存由行数与字符两个总预算兜底。模型侧仍按 4096 字节做预览截断
    // （truncated 标记），原始字节事实保留在结构化存储里，导出与记录拿得到。
    void hugePayloadDisplayedFullyInTextView()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        window.findChild<TrafficPanel *>()->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(text && model);

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        // 8KB 可打印 ASCII：模型预览截到 4096 字节，控制台必须一字节不少地展开。
        // 显示格式显式切到 UTF-8，不依赖共享配置里的残留值；HEX 对照行同样关掉，
        // 保证最后一行就是数据行，断言才有稳定锚点。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(false);
        QByteArray raw(8192, Qt::Uninitialized);
        for (int i = 0; i < raw.size(); ++i)
            raw[i] = char(0x20 + (i % 0x5F));
        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.source.endpointId = endpointId;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Serial;
        message.raw = raw;
        message.rawLength = raw.size();
        window.findChild<TrafficPanel *>()->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 1);

        const QString line = text->toPlainText();
        // 控制台完整显示：8192 字节一字节不少（单行封顶已移除）。
        const auto full = PayloadCodec::render(raw, PayloadCodec::Format::Utf8);
        QVERIFY2(line.endsWith(full),
            qPrintable(QStringLiteral("控制台行必须完整展开载荷：实际 %1 字符，载荷 %2 字符")
                           .arg(line.size()).arg(full.size())));
        // 模型侧仍有 4096 字节的预览截断：存储的 raw 被截到上限，rawLength 仍是
        // 真实大小并带 truncated 标记——结构化存储（导出/记录）与显示各有各的预算。
        QCOMPARE(model->record(0).raw.size(), 4096);
        QCOMPARE(model->record(0).displayBytes(), qint64(raw.size()));
        QVERIFY(model->record(0).truncated);
    }

    // 行内容开关按控制台语义生效：只影响新行，已固化的行不重排。
    // 行号恒在。关掉前缀后进入纯数据：已有接收字节连成一段，HEX 不可用。
    void displayTogglesShapeNewLines()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr); // 开关变化不落盘，避免污染共享配置
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(text && model);
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.source.endpointId = endpointId;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Serial;
        message.raw = QByteArray("OK");
        message.rawLength = 2;

        // 关时间戳、开长度与 HEX 对照。端口方向跟着「显示前缀」，不能单独关掉。
        // HEX 独占一行（不是接在数据行尾），便于上下比对字节。
        window.findChild<QAction *>(QStringLiteral("prefixTimestamp"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("prefixLength"))->setChecked(true);
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(true);
        QVERIFY(window.findChild<QAction *>(QStringLiteral("prefix"))->isChecked());
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(text->document()->blockCount(), 2); // 数据行 + HEX 对照行
        // 行号在左侧槽，正文以端口和方向开头。前缀以冒号收尾。长度跟在方向后面。
        QCOMPARE(text->document()->firstBlock().text(), QStringLiteral("[A] RX [2B] <<--: OK"));
        QCOMPARE(text->lineNumberAt(0), quint64(1));
        QCOMPARE(text->lineEndpointAt(0), 0);
        QCOMPARE(text->document()->lastBlock().text(), QStringLiteral("4F 4B"));
        QCOMPARE(text->lineNumberAt(1), quint64(0)); // HEX 对照跟这条记录，不另占号

        // 行号和长度各自独立：关掉后只剩端口方向与数据。
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("prefixLineNumber"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("prefixLength"))->setChecked(false);
        message.id = 2;
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 2);
        // 关掉 HEX 会重画已有行，第一行的对照行消失后只剩两条数据。
        QTRY_COMPARE(text->document()->blockCount(), 2);
        QCOMPARE(text->document()->lastBlock().text(), QStringLiteral("[A] RX <<--: OK"));
        QCOMPARE(text->lineNumberAt(text->document()->blockCount() - 1), quint64(2));

        // 关掉前缀：已有两条 OK 收成一段，HEX 置灰，发送和状态/警告/错误不上屏。
        window.findChild<QAction *>(QStringLiteral("prefix"))->setChecked(false);
        auto *hexAction = window.findChild<QAction *>(QStringLiteral("alwaysShowHex"));
        QVERIFY(!hexAction->isEnabled());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("viewAlwaysShowHex"))->isEnabled());
        QTRY_COMPARE(text->toPlainText(), QStringLiteral("OKOK"));
        // 纯数据正文不再铺端口色。端口在行号槽上，颜色标识才能留在字上。
        QCOMPARE(lastFragmentColor(text->document()->firstBlock()), QColor(0x1C, 0x24, 0x30));
        QCOMPARE(text->lineEndpointAt(0), 0);

        message.id = 3;
        panel->appendMessages({message});
        QTRY_COMPARE(text->toPlainText(), QStringLiteral("OKOKOK"));

        Traffic::Message sent = message;
        sent.id = 4;
        sent.direction = Traffic::Direction::Transmit;
        sent.origin = Traffic::Origin::Manual;
        sent.raw = QByteArray("NO");
        sent.rawLength = 2;
        Traffic::Message state = message;
        state.id = 5;
        state.direction = Traffic::Direction::State;
        state.raw.clear();
        state.rawLength = 0;
        state.statusText = QStringLiteral("up");
        Traffic::Message warn = state;
        warn.id = 6;
        warn.direction = Traffic::Direction::Warning;
        warn.statusText = QStringLiteral("careful");
        Traffic::Message err = state;
        err.id = 7;
        err.direction = Traffic::Direction::Error;
        err.statusText = QStringLiteral("denied");
        panel->appendMessages({sent, state, warn, err});
        QTRY_COMPARE(model->rowCount(), 7);
        QCOMPARE(text->toPlainText(), QStringLiteral("OKOKOK"));

        // 打开前缀后，刚才没画出来的发送和事件按条换行出现。
        window.findChild<QAction *>(QStringLiteral("prefix"))->setChecked(true);
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("NO")));
        QVERIFY(text->toPlainText().contains(QStringLiteral("up")));
        QVERIFY(text->toPlainText().contains(QStringLiteral("careful")));
        QVERIFY(text->toPlainText().contains(QStringLiteral("denied")));
        QVERIFY(hexAction->isEnabled());
        // 还原默认开关，不把本用例的状态带给后续用例。
        window.findChild<QAction *>(QStringLiteral("prefixLineNumber"))->setChecked(true);
        window.findChild<QAction *>(QStringLiteral("prefixTimestamp"))->setChecked(true);
        window.findChild<QAction *>(QStringLiteral("prefixLength"))->setChecked(true);
        hexAction->setChecked(false);
    }

    // 纯数据把接收字节按到达顺序拼在一起。载荷里的换行保留，消息边界不再加一行。
    // A、B 用端口色区分；发送、状态即使带着 HEX 开关也不出现。
    void pureDataStreamsReceivedBytesByPortColor()
    {
        TrafficTextView view;
        TrafficTextView::DisplayOptions options;
        options.prefix = false;
        options.alwaysShowHex = true;

        const auto rx = [](int port, const char *body) {
            Traffic::Message message;
            message.id = 1;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointIndex = port;
            message.raw = QByteArray(body);
            message.rawLength = message.raw.size();
            return message;
        };
        auto tx = rx(0, "NO");
        tx.direction = Traffic::Direction::Transmit;
        auto state = rx(0, "");
        state.direction = Traffic::Direction::State;
        state.raw.clear();
        state.rawLength = 0;
        state.statusText = QStringLiteral("up");

        view.appendMessages({rx(0, "AA"), tx, rx(1, "BB"), state}, PayloadCodec::Format::Utf8, options);
        QCOMPARE(view.toPlainText(), QStringLiteral("AABB"));
        // 两段都是正文色时，文本框会并成一个片段。端口不在字色上，混行记在行号槽。
        QCOMPARE(lastFragmentColor(view.document()->firstBlock()), QColor(0x1C, 0x24, 0x30));
        QCOMPARE(view.lineEndpointAt(0), 0);
        QVERIFY(view.lineMixedAt(0));

        view.clearAll();
        view.appendMessages({rx(0, "uname -a\r\n")}, PayloadCodec::Format::Utf8, options);
        view.appendMessages({rx(1, "uname -a\n")}, PayloadCodec::Format::Utf8, options);
        QCOMPARE(view.toPlainText(), QStringLiteral("uname -a\nuname -a\n"));
        QCOMPARE(lastFragmentColor(view.document()->firstBlock()), QColor(0x1C, 0x24, 0x30));
        QCOMPARE(view.lineEndpointAt(0), 0);
        QCOMPARE(view.lineEndpointAt(1), 1);
        QVERIFY(!view.lineMixedAt(0));
        QVERIFY(!view.lineMixedAt(1));
    }

    // 显示设置是"每行长什么样"的视图属性，不是"只对新数据生效"的到达时快照。
    // 切开关必须立刻作用到**已经收到**的那批行上（用户明确要求：包括已经接收到的数据）。
    // 这条用例同时钉住两半：外观改了就重排已有行；而行号在重投影后仍从 1 连续，
    // 以及 HEX 对照行比数据行浅——它是附注，不是主体。
    void displaySettingsApplyToAlreadyReceivedRows()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr); // 开关变化不落盘，避免污染共享配置
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *clock = window.findChild<QAction *>(QStringLiteral("prefixTimestamp"));
        auto *hexEcho = window.findChild<QAction *>(QStringLiteral("alwaysShowHex"));
        QVERIFY(text && model && clock && hexEcho);
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        QVERIFY2(clock->isChecked(), "默认应带时间戳，否则本用例的前提不成立");
        // 初次启动不带 HEX 对照。本用例要验证关掉对照后的行数，先打开再关掉。
        hexEcho->setChecked(true);
        hexEcho->setChecked(false);

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        constexpr int Count = 5;
        QList<Traffic::Message> batch;
        for (int i = 0; i < Count; ++i) {
            Traffic::Message message;
            message.id = i + 1;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray("OK");
            message.rawLength = 2;
            batch.append(message);
        }
        panel->appendMessages(batch);
        QTRY_COMPARE(model->rowCount(), Count);
        QTRY_COMPARE(text->document()->blockCount(), Count);
        const QRegularExpression stamp(QStringLiteral(R"(\d{2}:\d{2}:\d{2}\.\d{3})"));
        QVERIFY2(text->document()->firstBlock().text().contains(stamp),
            qPrintable(text->document()->firstBlock().text()));

        // ① 关掉时间戳：**已经在这儿的 5 行**必须一起失去时间戳。
        //    这一条正是"显示设置只对新数据生效"那个缺陷的反面。
        clock->setChecked(false);
        QTRY_VERIFY2(!text->document()->toPlainText().contains(stamp),
            qPrintable(QStringLiteral("已有行没有跟随显示设置重排：")
                + text->document()->firstBlock().text()));
        QCOMPARE(text->document()->blockCount(), Count); // 只改样子，行数不变
        QCOMPARE(text->lineNumberAt(0), quint64(1));

        // ② 打开 HEX 对照：已有行要长出对照行——它们不是"新数据"，是重新渲染出来的。
        hexEcho->setChecked(true);
        QTRY_COMPARE(text->document()->blockCount(), Count * 2);
        // 每条数据后面跟一行 HEX。对照行留白，第 5 条数据仍是第 5 号。
        QCOMPARE(text->lineNumberAt(Count * 2 - 2), quint64(5));
        QCOMPARE(text->lineNumberAt(Count * 2 - 1), quint64(0));
        // HEX 对照行是附注而不是数据本身，必须比数据行浅：上下两行一眼能分出主次。
        const QColor dataLine = lastFragmentColor(text->document()->firstBlock());
        const QColor hexLine = lastFragmentColor(text->document()->firstBlock().next());
        QVERIFY2(hexLine.value() > dataLine.value(),
            qPrintable(QStringLiteral("HEX 对照行应当比数据行浅：对照 %1 / 数据 %2")
                           .arg(hexLine.value())
                           .arg(dataLine.value())));

        // 还原默认开关，不把本用例的状态带给后续用例。
        hexEcho->setChecked(false);
        clock->setChecked(true);
    }

    // 显示格式为 HEX 时不重复显示 HEX 对照行：数据本身就是 HEX，再来一行一模一样。
    // 编码格式是"外观"：切换会立刻作用于**已显示的行**（r5 契约，旧行也重画），
    // 所以下面的断言都建立在"等掉那次必然发生的防抖重投影"之上，而不是某一条的到达状态。
    void hexEchoSuppressedInHexFormat()    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(text && model);
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("HEX"));
        // 关掉时间戳和数据长度，行内容只剩行号、端口方向和数据。
        window.findChild<QAction *>(QStringLiteral("prefixTimestamp"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("prefixLength"))->setChecked(false);
        QVERIFY(window.findChild<QAction *>(QStringLiteral("prefix"))->isChecked());
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(true);
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.source.endpointId = endpointId;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Serial;
        message.raw = QByteArray("OK");
        message.rawLength = 2;
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(text->document()->blockCount(), 1); // 只有数据行，没有 HEX 对照行
        QCOMPARE(text->document()->lastBlock().text(), QStringLiteral("[A] RX <<--: 4F 4B"));
        QCOMPARE(text->lineNumberAt(0), quint64(1));
        // 切 UTF-8：对照行恢复出现，且**已显示的那一行**也要一起重画为 UTF-8 + 对照行。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        // 首次建窗时字体扫描可能占住界面线程，固定 90ms 会在防抖重投影之前返回。
        QTRY_COMPARE(text->document()->blockCount(), 2); // 旧行已重画为：数据行 + HEX 对照行
        message.id = 2;
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 2);
        QCOMPARE(text->document()->blockCount(), 4); // 两行各带一个 HEX 对照行
        QCOMPARE(text->document()->lastBlock().text(), QStringLiteral("4F 4B"));

        // 对照是原始字节，不是把屏幕上的字再编码。非法 UTF-8 正文会显示成替换字符，
        // 但对照不能变成 EF BF BD。流片段末尾的 ⏎ 同样只是续接记号，不能变成 E2 8F 8E。
        message.id = 3;
        message.framing = Traffic::Framing::Stream;
        message.hasMoreFragments = true;
        message.raw = QByteArray::fromHex("5AA501FF1A200000CA1A");
        message.rawLength = message.raw.size();
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 3);
        QVERIFY(text->document()->toPlainText().contains(QStringLiteral("\u23CE")));
        QVERIFY(text->document()->toPlainText().contains(
            QStringLiteral("5A A5 01 FF 1A 20 00 00 CA 1A")));
        QVERIFY(!text->document()->toPlainText().contains(QStringLiteral("EF BF BD")));
        QVERIFY(!text->document()->toPlainText().contains(QStringLiteral("E2 8F 8E")));

        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(false);
        window.findChild<QAction *>(QStringLiteral("prefixTimestamp"))->setChecked(true);
        window.findChild<QAction *>(QStringLiteral("prefixLength"))->setChecked(true);
    }

    // 错误/警告日志的独立开关：关闭后事件不进显示、数据不受影响；开启后恢复。
    void errorEventsFollowTheirOwnToggle()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(text && model);
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        const auto makeMessage = [endpointId](quint64 id, Traffic::Direction direction, const char *text) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = direction;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray(text);
            message.rawLength = message.raw.size();
            if (direction == Traffic::Direction::Error)
                message.statusText = QStringLiteral("写入失败");
            return message;
        };
        // 先建立一条可见数据行。
        panel->appendMessages({makeMessage(1, Traffic::Direction::Receive, "data")});
        QTRY_COMPARE(model->rowCount(), 1);
        // 外观（编码格式）改动会立刻作用于已收到的行（r5 契约）：切 UTF-8 后旧的 "data"
        // 行也应从 HEX 变为 UTF-8。这一步必然触发一次防抖重投影，显式等掉它再取基线——
        // 不能把基线建立在"重投影还没发生"上（旧断言正是这么过期的：它赌的是 HEX 还没重画）。
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        // 始终显示 HEX 缺省关闭。这里再关一次，保证一条消息一行。
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(false);
        // 等防抖重投影把已显示的 HEX 行换成 UTF-8。用轮询而不是固定 90ms：
        // 首次创建窗口时字体扫描可能占住界面线程，固定等待会在重投影之前返回。
        QTRY_VERIFY(text->document()->lastBlock().text().endsWith(QStringLiteral("data")));
        const auto baseline = text->document()->lastBlock().text();
        // 关闭错误日志：错误事件进模型不进显示，数据行纹丝不动。
        // 事件开关住在显示设置菜单里（QAction），不在顶栏。
        window.findChild<QAction *>(QStringLiteral("showError"))->setChecked(false);
        panel->appendMessages({makeMessage(2, Traffic::Direction::Error, "boom")});
        QTRY_COMPARE(model->rowCount(), 2);
        QCOMPARE(text->lineCount(), 1);
        QCOMPARE(text->document()->lastBlock().text(), baseline);
        // 重新开启：历史错误与后续错误均恢复显示，等待防抖重投影完成。
        window.findChild<QAction *>(QStringLiteral("showError"))->setChecked(true);
        panel->appendMessages({makeMessage(3, Traffic::Direction::Error, "boom2")});
        QTRY_COMPARE(text->lineCount(), 3);
        QVERIFY2(text->document()->lastBlock().text().endsWith(QStringLiteral("boom2")),
            qPrintable(QStringLiteral("错误行实况：")
                + text->document()->lastBlock().text()));
        // 已经显示的错误行必须跟着开关立刻收起，不能等「应用过滤」。
        auto *apply = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        window.findChild<QAction *>(QStringLiteral("showError"))->setChecked(false);
        QTRY_VERIFY(!text->document()->toPlainText().contains(QStringLiteral("boom2")));
        QVERIFY(text->document()->toPlainText().contains(QStringLiteral("data")));
        if (apply)
            QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
        window.findChild<QAction *>(QStringLiteral("showError"))->setChecked(true);
        QTRY_VERIFY(text->document()->toPlainText().contains(QStringLiteral("boom2")));

        // 状态迁移、警告和错误同属显示设置，已有行都要立刻跟随。
        const auto makeEvent = [endpointId](quint64 id, Traffic::Direction direction, const QString &text) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = direction;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.statusText = text;
            message.rawLength = 0;
            return message;
        };
        panel->appendMessages({
            makeEvent(4, Traffic::Direction::State, QStringLiteral("已打开")),
            makeEvent(5, Traffic::Direction::Warning, QStringLiteral("重连")),
        });
        QTRY_VERIFY(text->document()->toPlainText().contains(QStringLiteral("已打开")));
        QVERIFY(text->document()->toPlainText().contains(QStringLiteral("重连")));
        window.findChild<QAction *>(QStringLiteral("showState"))->setChecked(false);
        QTRY_VERIFY(!text->document()->toPlainText().contains(QStringLiteral("已打开")));
        QVERIFY(text->document()->toPlainText().contains(QStringLiteral("重连")));
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
        window.findChild<QAction *>(QStringLiteral("showWarning"))->setChecked(false);
        QTRY_VERIFY(!text->document()->toPlainText().contains(QStringLiteral("重连")));
        window.findChild<QAction *>(QStringLiteral("showState"))->setChecked(true);
        QTRY_VERIFY(text->document()->toPlainText().contains(QStringLiteral("已打开")));
        window.findChild<QAction *>(QStringLiteral("showWarning"))->setChecked(true);
        window.findChild<QAction *>(QStringLiteral("showError"))->setChecked(true);

        // 顶栏 A/B、RX/TX、转发一点就作用到已有行，不能等「应用过滤」。
        panel->appendMessages({
            makeMessage(6, Traffic::Direction::Transmit, "sent"),
            makeMessage(7, Traffic::Direction::Receive, "got"),
        });
        QTRY_VERIFY(text->document()->toPlainText().contains(QStringLiteral("sent")));
        auto *rx = window.findChild<QPushButton *>(QStringLiteral("filterRx"));
        auto *tx = window.findChild<QPushButton *>(QStringLiteral("filterTx"));
        QVERIFY(rx && tx && apply);
        rx->setChecked(false);
        QTRY_VERIFY(!text->document()->toPlainText().contains(QStringLiteral("got")));
        QVERIFY(text->document()->toPlainText().contains(QStringLiteral("sent")));
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
        tx->setChecked(false);
        QTRY_VERIFY(!text->document()->toPlainText().contains(QStringLiteral("sent")));
        rx->setChecked(true);
        tx->setChecked(true);
        QTRY_VERIFY(text->document()->toPlainText().contains(QStringLiteral("got")));
        QVERIFY(text->document()->toPlainText().contains(QStringLiteral("sent")));

        auto *portA = window.findChild<QPushButton *>(QStringLiteral("showPortA"));
        portA->setChecked(false);
        QTRY_VERIFY(!text->document()->toPlainText().contains(QStringLiteral("data")));
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
        portA->setChecked(true);
        QTRY_VERIFY(text->document()->toPlainText().contains(QStringLiteral("data")));
    }

    // 外观重投影不得改变"哪些行在屏幕上"（设计稿 §5 决策表）：改显示设置 / 编码格式只换
    // 每行长什么样，未点「应用过滤」的关键词绝不能被顺手套到历史行上——否则屏幕会莫名变少、
    // 过滤输入框支持回车应用（用户钦定）：焦点在关键词/排除框里按 Enter，等同于点
    // 「应用过滤」。两条入口必须指向同一个出口——按钮禁用时回车同样无效、
    // 待应用记号与"重复重放"的判断都只在一处维护，否则两条路会各自漂移。
    void filterInputAppliesOnReturn()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *keywords = window.findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        auto *excludes = window.findChild<QLineEdit *>(QStringLiteral("filterExcludes"));
        auto *apply = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        QVERIFY(text && model && keywords && excludes && apply);
        // 提示文案（用户要求"给出回车生效的提示"）：placeholder 与 tooltip 都要提到回车。
        // 两者覆盖不同时机——placeholder 在空框时可见（输入后消失），tooltip 在悬停时可见；
        // 用断言锁住，免得以后调文案时把这条提示弄丢。
        for (auto *input : {keywords, excludes}) {
            QVERIFY2(input->placeholderText().contains(QStringLiteral("回车")),
                qPrintable(QStringLiteral("placeholder 应提示回车：") + input->placeholderText()));
            QVERIFY2(input->toolTip().contains(QStringLiteral("回车")),
                qPrintable(QStringLiteral("tooltip 应提示回车：") + input->toolTip()));
        }
        // 注意断言对象是**视图行数**不是模型行数：过滤是"投影"，模型始终保留全部记录，
        // 过滤只决定哪些行进视图（见 r5 契约）。拿 model->rowCount() 去断言会永远看到 6。
        const auto visibleRows = [text] { return text->lineCount(); };

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        const auto makeMessage = [endpointId](quint64 id, const char *hex) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray::fromHex(hex);
            message.rawLength = message.raw.size();
            return message;
        };
        QList<Traffic::Message> batch;
        for (quint64 i = 0; i < 3; ++i) {
            batch.append(makeMessage(i * 2 + 1, "AABB"));
            batch.append(makeMessage(i * 2 + 2, "CCDD"));
        }
        panel->appendMessages(batch);
        QTRY_COMPARE(visibleRows(), 6);

        // 打上关键词但**不按回车**：视图不动（"条件不自动回溯"的既有契约仍然成立）。
        keywords->setText(QStringLiteral("aa bb"));
        QCOMPARE(visibleRows(), 6);

        // ★ 回车 = 应用：只有命中的 3 条留下。
        QTest::keyClick(keywords, Qt::Key_Return);
        QTRY_COMPARE(visibleRows(), 3);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤")); // 应用后待应用记号消失

        // ★ 排除框同样支持回车，且排除优先于关键词：两个条件都命中同一批 → 全部隐藏
        //（全隐藏时视图仍是保底的一个空块，所以是 1 而不是 0）。
        excludes->setText(QStringLiteral("aa bb"));
        QCOMPARE(visibleRows(), 3); // 未回车前不动
        QTest::keyClick(excludes, Qt::Key_Return);
        QTRY_COMPARE(visibleRows(), 1);

        // 按钮禁用时（清空后无记录）回车也必须无效——两条入口行为一致，不能一条绕开。
        keywords->clear();
        excludes->clear();
        window.findChild<QPushButton *>(QStringLiteral("clearTraffic"))->click();
        QTRY_COMPARE(model->rowCount(), 0);
        QTRY_VERIFY(!apply->isEnabled());
        keywords->setText(QStringLiteral("aa"));
        QTest::keyClick(keywords, Qt::Key_Return);
        QTest::qWait(30); // 给可能被误触发的重放留出窗口
        QCOMPARE(visibleRows(), 1); // 仍是保底空块，没有被"凭空"重放出内容
    }

    // 没有保存过展开状态时，扩展区收起，并且一开始就不在分栏里。
    void extensionPanelStartsCollapsed()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        auto *toggle = window.findChild<QPushButton *>(QStringLiteral("commandPanelToggle"));
        auto *panel = window.findChild<CommandPanel *>();
        auto *splitter = window.findChild<QSplitter *>(QStringLiteral("horizontalSplitter"));
        QVERIFY(toggle && panel && splitter);
        QVERIFY(!panel->isExpanded());
        QVERIFY(!toggle->isChecked());
        QCOMPARE(splitter->count(), 1);
        QCOMPARE(splitter->indexOf(panel), -1);
        QVERIFY(!splitter->handle(1));
        QCOMPARE(splitter->width() - splitter->widget(0)->width(), 0);
    }

    // 窗口几何与「扩展」按钮状态都要记忆（用户钦定）：退出时落盘，下次启动恢复。
    // 两者必须**自洽**——上次是"面板收起 + 窄窗口"，重开就不能是"面板收起 + 宽窗口"。
    void windowGeometryAndPanelStateAreRemembered()
    {
        int savedWidth = 0;
        int savedHeight = 0;
        {
            MainWindow window;
            window.setAttribute(Qt::WA_DontShowOnScreen);
            window.resize(1320, 800);
            window.show();
            QTest::qWait(30);
            auto *toggle = window.findChild<QPushButton *>(QStringLiteral("commandPanelToggle"));
            QVERIFY(toggle);
            // 初次启动已经是收起。先展开再收起，退出时记下的才是用户选过的收起状态。
            toggle->setChecked(true);
            QTest::qWait(30);
            toggle->setChecked(false); // 收起指令面板（会顺带把窗口就地收窄）
            QTest::qWait(50);
            savedWidth = window.width();
            savedHeight = window.height();
            QVERIFY(savedWidth > 0 && savedHeight > 0);
            auto *pin = window.findChild<QToolButton *>(QStringLiteral("stayOnTop"));
            QVERIFY(pin);
            QVERIFY(pin->text().isEmpty());
            QVERIFY(!pin->icon().isNull());
            QCOMPARE(pin->toolButtonStyle(), Qt::ToolButtonIconOnly);
            QCOMPARE(pin->iconSize(), QSize(12, 12));
            auto *actions = window.findChild<QWidget *>(QStringLiteral("StatusActions"));
            auto *cliChip = window.findChild<QWidget *>(QStringLiteral("StatusCli"));
            auto *schemeChip = window.findChild<QWidget *>(QStringLiteral("StatusScheme"));
            auto *pinChip = window.findChild<QWidget *>(QStringLiteral("StatusPin"));
            auto *chip = window.findChild<QWidget *>(QStringLiteral("EndpointChip"));
            QVERIFY(actions && cliChip && schemeChip && pinChip && chip);
            QCOMPARE(actions->height(), chip->height());
            QCOMPARE(cliChip->height(), chip->height());
            QCOMPARE(schemeChip->height(), chip->height());
            QCOMPARE(pinChip->height(), chip->height());
            const int cliGap = cliChip->mapTo(&window, QPoint(0, 0)).x()
                - actions->mapTo(&window, QPoint(actions->width(), 0)).x();
            QCOMPARE(cliGap, 6);
            const int gap = schemeChip->mapTo(&window, QPoint(0, 0)).x()
                - cliChip->mapTo(&window, QPoint(cliChip->width(), 0)).x();
            QCOMPARE(gap, cliGap);
            const int pinGap = pinChip->mapTo(&window, QPoint(0, 0)).x()
                - schemeChip->mapTo(&window, QPoint(schemeChip->width(), 0)).x();
            QCOMPARE(pinGap, cliGap);
            const int pinMid = pin->mapTo(&window, QPoint(0, pin->height() / 2)).y();
            const int chipMid = chip->mapTo(&window, QPoint(0, chip->height() / 2)).y();
            QVERIFY2(qAbs(pinMid - chipMid) <= 1,
                qPrintable(QStringLiteral("置顶与端点状态中线差 %1").arg(pinMid - chipMid)));
            QVERIFY(!pin->isChecked());
            QVERIFY(!window.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
            const int widthBeforePin = window.width();
            const int heightBeforePin = window.height();
            pin->click();
            QVERIFY(pin->isChecked());
            QVERIFY(window.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
            // 置顶会重建原生窗口，大小必须留在原地。
            QCOMPARE(window.width(), widthBeforePin);
            QCOMPARE(window.height(), heightBeforePin);
            window.close(); // 触发 closeEvent → 几何落盘
            QTest::qWait(30);
        }

        // 新建窗口：应当恢复上次的几何，并保持"收起"这个状态。
        MainWindow again;
        again.setAttribute(Qt::WA_DontShowOnScreen);
        again.show();
        QTest::qWait(50);
        auto *toggle = again.findChild<QPushButton *>(QStringLiteral("commandPanelToggle"));
        auto *panel = again.findChild<CommandPanel *>();
        QVERIFY(toggle && panel);
        QVERIFY2(!panel->isExpanded(),
            qPrintable(QStringLiteral("上次是收起状态，重开应仍是收起；实际 expanded=")
                + QString::number(panel->isExpanded())));
        QVERIFY(!toggle->isChecked());
        auto *splitter = again.findChild<QSplitter *>(QStringLiteral("horizontalSplitter"));
        QVERIFY(splitter);
        QCOMPARE(splitter->indexOf(panel), -1);
        QVERIFY2(!splitter->handle(1),
            qPrintable(QStringLiteral("启动时扩展区已收起，左右拖动条应不存在")));
        QCOMPARE(splitter->width() - splitter->widget(0)->width(), 0);
        auto *pinAgain = again.findChild<QToolButton *>(QStringLiteral("stayOnTop"));
        QVERIFY(pinAgain && pinAgain->isChecked());
        QVERIFY(again.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        auto *schemeAgain = again.findChild<QToolButton *>(QStringLiteral("colorScheme"));
        QVERIFY(schemeAgain);
        // 这份配置没存过外观。重开仍按测试钉住的亮色勾选，不能改成跟随系统。
        auto *lightAgain = again.findChild<QAction *>(QStringLiteral("colorScheme_light"));
        QVERIFY(lightAgain && lightAgain->isChecked());
        QCOMPARE(AppTheme::mode(), AppTheme::Mode::Light);
        // 默认尺寸是 1280×860，且不再有 900×720 的下限；恢复后必须贴近"上次"，
        // 而不是退回默认——这是本用例要守的东西。
        //
        // 容差不一致是**故意的**：宽度逐像素（实测只差 2px），高度给 ±32。
        // 原因是离屏窗口（WA_DontShowOnScreen）没有真实标题栏：saveGeometry 保存时框架高度
        // 算作 0，restoreGeometry 恢复时却按"有标题栏"折算，于是客户区差出约 26px
        //（实测 800 → 774，正是标题栏那一段）。这是测试环境的固有换算差异，不是功能缺陷
        // ——同一份几何用 saveGeometry 再存一遍，只有 1px 出入（1222/798 vs 1223/799）。
        // 默认高度 860 与恢复后的 774 差 86，大于 ±32，这个容差不会把"根本没恢复"放过去。
        QVERIFY2(qAbs(again.width() - savedWidth) <= 4,
            qPrintable(QStringLiteral("窗口宽度应恢复到上次：实际 %1，上次 %2")
                           .arg(again.width()).arg(savedWidth)));
        QVERIFY2(qAbs(again.height() - savedHeight) <= 32,
            qPrintable(QStringLiteral("窗口高度应恢复到上次：实际 %1，上次 %2")
                           .arg(again.height()).arg(savedHeight)));
    }

    // 外观菜单可以在亮色、暗色、跟随系统之间切换，并把选择记下来。
    void colorSchemeCanBeSwitchedAndRemembered()
    {
        const auto surface = [](QWidget *widget) {
            return widget->palette().color(QPalette::Window);
        };
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        auto *button = window.findChild<QToolButton *>(QStringLiteral("colorScheme"));
        auto *light = window.findChild<QAction *>(QStringLiteral("colorScheme_light"));
        auto *dark = window.findChild<QAction *>(QStringLiteral("colorScheme_dark"));
        auto *system = window.findChild<QAction *>(QStringLiteral("colorScheme_system"));
        auto *bar = window.findChild<QStatusBar *>();
        QVERIFY(button && light && dark && system && bar);
        const auto saveStatusPreview = [&](const QString &scheme) {
            const auto directory = qEnvironmentVariable("QQCOM_UI_ARTIFACT_DIR");
            if (directory.isEmpty()) return true;
            auto *controls = window.findChild<QWidget *>(QStringLiteral("StatusControls"));
            return controls && QDir().mkpath(directory)
                && bar->grab().save(directory + "/statusbar-" + scheme + ".png")
                && controls->grab().save(directory + "/status-controls-" + scheme + ".png");
        };
        QVERIFY(button->menu());
        QCOMPARE(button->text(), QString());
        QVERIFY(!button->icon().isNull());
        // 测试进程钉在亮色上，配置里还没有选择。窗口按当前模式勾亮色，不能改去跟随系统。
        QVERIFY(light->isChecked());
        QVERIFY(!dark->isChecked());
        QVERIFY(!system->isChecked());
        QCOMPARE(AppTheme::mode(), AppTheme::Mode::Light);
        QCOMPARE(qApp->styleHints()->colorScheme(), Qt::ColorScheme::Light);

        light->trigger();
        QVERIFY(light->isChecked());
        QVERIFY(!dark->isChecked());
        QVERIFY(!system->isChecked());
        QCOMPARE(AppTheme::mode(), AppTheme::Mode::Light);
        QVERIFY(!AppTheme::isDark());
        QCOMPARE(qApp->styleHints()->colorScheme(), Qt::ColorScheme::Light);
        const QColor lightSurface = surface(bar);
        QVERIFY(saveStatusPreview(QStringLiteral("light")));
        QVERIFY2(lightSurface.lightness() > 200,
            qPrintable(QStringLiteral("亮色状态栏应是浅底：%1").arg(lightSurface.name())));

        dark->trigger();
        QVERIFY(dark->isChecked());
        QCOMPARE(AppTheme::mode(), AppTheme::Mode::Dark);
        QVERIFY(AppTheme::isDark());
        QCOMPARE(qApp->styleHints()->colorScheme(), Qt::ColorScheme::Dark);
        const QColor darkSurface = surface(bar);
        QVERIFY(saveStatusPreview(QStringLiteral("dark")));
        QVERIFY2(darkSurface.lightness() < 80,
            qPrintable(QStringLiteral("暗色状态栏应是深底：%1").arg(darkSurface.name())));
        QVERIFY(darkSurface != lightSurface);
        window.close();
        QTest::qWait(30);

        MainWindow again;
        again.setAttribute(Qt::WA_DontShowOnScreen);
        again.show();
        QTest::qWait(30);
        auto *darkAgain = again.findChild<QAction *>(QStringLiteral("colorScheme_dark"));
        QVERIFY(darkAgain && darkAgain->isChecked());
        QVERIFY(AppTheme::isDark());
        auto *barAgain = again.findChild<QStatusBar *>();
        QVERIFY(barAgain);
        QCOMPARE(surface(barAgain).lightness(), darkSurface.lightness());

        auto *systemAgain = again.findChild<QAction *>(QStringLiteral("colorScheme_system"));
        QVERIFY(systemAgain);
        systemAgain->trigger();
        QVERIFY(systemAgain->isChecked());
        QCOMPARE(AppTheme::mode(), AppTheme::Mode::System);
        // 手动钉过暗色之后放开，必须回到系统当前这一档，不能停在暗色上。
        // 这台机器若正开着系统暗色，读回来就是暗色；Qt 报 Unknown 时按浅色处理。
        const Qt::ColorScheme os = qApp->styleHints()->colorScheme();
        const Qt::ColorScheme resolved = os == Qt::ColorScheme::Unknown ? Qt::ColorScheme::Light : os;
        QCOMPARE(qApp->styleHints()->colorScheme(), resolved);
        QCOMPARE(AppTheme::isDark(), resolved == Qt::ColorScheme::Dark);
        // 后面的用例按浅色像素断言。这里已经放开到系统档，测完立刻钉回亮色，
        // 不能等 cleanup：cleanup 在本用例返回之后才跑。
        AppTheme::setMode(AppTheme::Mode::Light);
    }

    // 三条用户反馈的契约：
    //   ① 定时发送的间隔框**始终可输入**（不必先勾选），且**只收数字**；
    //   ② 「清除过滤」按钮**带文案**（不再是个只有图标的按钮）；
    //   ③ 「清除过滤」只**停用过滤**、不丢用户敲的条件 —— 输入框内容保留并被持久化，
    //      下次启动还在（用户反馈"关键词过滤与排除是否可以记忆……又得重新输入，很麻烦"）。
    void filterResetKeepsConditionsAndTimerAcceptsInput()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        // ---------- ① 定时间隔框 ----------
        auto *repeat = window.findChild<QCheckBox *>(QStringLiteral("repeatSending"));
        auto *interval = window.findChild<QSpinBox *>(QStringLiteral("repeatInterval"));
        QVERIFY(repeat && interval);
        QVERIFY2(!repeat->isChecked(), "默认不勾选定时发送");
        QVERIFY2(interval->isEnabled(),
            "未勾选定时发送时间隔框也必须可输入（用户钦定：方便先设好再勾）");
        // 只收数字：它是 QSpinBox，非数字键会被自带校验器挡掉。
        // 单位是框内后缀，不进入数值。（键盘事件直接发给 spinbox
        // —— 它内部的 lineEdit 是 protected，测试拿不到。）
        QCOMPARE(interval->suffix(), QStringLiteral(" ms"));
        interval->clear();
        QTest::keyClicks(interval, QStringLiteral("abc"));
        QString numeric = interval->text();
        if (numeric.endsWith(interval->suffix()))
            numeric.chop(interval->suffix().size());
        numeric = numeric.trimmed();
        for (const QChar &ch : numeric) {
            QVERIFY2(ch.isDigit(),
                qPrintable(QStringLiteral("间隔框数值只应含数字，实际是「%1」").arg(interval->text())));
        }
        interval->clear();
        QTest::keyClicks(interval, QStringLiteral("250"));
        QVERIFY2(interval->value() == 250,
            qPrintable(QStringLiteral("数字应能输入进去：value=%1 text=%2")
                           .arg(interval->value()).arg(interval->text())));
        // 勾上之后**不**改成禁用（这正是用户抱怨的那一点）。
        repeat->setChecked(true);
        QVERIFY2(interval->isEnabled(), "勾选定时发送后间隔框不应被禁用");
        repeat->setChecked(false);

        // ---------- ② 清除按钮带文案 ----------
        auto *reset = window.findChild<QPushButton *>(QStringLiteral("filterReset"));
        QVERIFY(reset);
        QVERIFY2(!reset->text().isEmpty(), "「清除过滤」按钮必须有文案（用户要求）");

        // ---------- ③ 清除过滤只停用、不丢条件 ----------
        auto *panel = window.findChild<TrafficPanel *>();
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *keywords = window.findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        auto *excludes = window.findChild<QLineEdit *>(QStringLiteral("filterExcludes"));
        auto *apply = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        QVERIFY(panel && text && keywords && excludes && apply);
        panel->setPreferences(nullptr); // 不碰共享 ini

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        const auto makeMessage = [endpointId](quint64 id, const char *hex) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray::fromHex(hex);
            message.rawLength = message.raw.size();
            return message;
        };
        QList<Traffic::Message> batch;
        for (quint64 i = 0; i < 3; ++i) {
            batch.append(makeMessage(i * 2 + 1, "AABB"));
            batch.append(makeMessage(i * 2 + 2, "CCDD"));
        }
        panel->appendMessages(batch);
        QTRY_COMPARE(text->lineCount(), 6);

        keywords->setText(QStringLiteral("aa bb"));
        apply->click(); // 应用过滤 → 只剩命中的 3 条
        QTRY_COMPARE(text->lineCount(), 3);

        reset->click(); // 清除过滤
        QTRY_COMPARE(text->lineCount(), 6); // 过滤停用、显示全部
        // ★ 条件**原样保留**：用户不用重新敲。
        QCOMPARE(keywords->text(), QStringLiteral("aa bb"));
        // 而且没被丢掉——再点「应用过滤」立刻回到 3 条（若被清空，这里会是 6）。
        apply->click();
        QTRY_COMPARE(text->lineCount(), 3);
    }

    // 状态栏里所有文字必须**同一个字号**（用户反馈"底部状态栏文字大小似乎没有统一"）：
    // 原来只有右侧提示行（StatusNote）写了 12px、其余标签（RX/TX/条数/桥接）继承默认字号，
    // 并排看就是大小不一。这里直接量每个标签的 font().pixelSize()——QSS 用 px 写 font-size 时
    // 它就等于那个值；若某个标签没有明确字号，会是与 12px 不同的值。
    void statusBarTextSizeIsUniform()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *bar = window.findChild<QStatusBar *>();
        QVERIFY(bar);
        const auto labels = bar->findChildren<QLabel *>();
        QVERIFY2(labels.size() >= 4,
            qPrintable(QStringLiteral("状态栏应有多个标签，实际 ") + QString::number(labels.size())));

        int expected = -1;
        for (auto *label : labels) {
            const int px = label->font().pixelSize();
            if (expected < 0) expected = px;
            QVERIFY2(px == expected,
                qPrintable(QStringLiteral("状态栏字号不一致：%1 是 %2px，其余是 %3px（应为同一条 QSS 规则）")
                               .arg(label->objectName()).arg(px).arg(expected)));
        }
        // 字族也要一致：RX/TX 原来硬写了 `font-family: "Segoe UI"`，换系统字体或切换语言时
        // 会与旁边标签不一路，同样是"看着没统一"的来源。
        const QString family = labels.first()->font().family();
        for (auto *label : labels) {
            QVERIFY2(label->font().family() == family,
                qPrintable(QStringLiteral("状态栏字族不一致：%1 是 %2，其余是 %3")
                               .arg(label->objectName(), label->font().family(), family)));
        }
    }

    // HEX / 字节数据的显示字体必须统一（用户反馈"hex 数据显示的字体似乎没有统一"）：
    // 全局 QSS 给的是比例字体（Microsoft YaHei UI），而监视区、发送区正文各自设了等宽；
    // **发送区预览行漏设**，于是同一串字节在两处写法不同、宽窄也对不齐。
    // 这里锁住"字节显示一律等宽"：同一面板内三处必须完全一致，跨面板必须同族。
    void hexDisplayFontIsUniform()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("sendEditor"));
        auto *preview = window.findChild<QLabel *>(QStringLiteral("payloadPreview"));
        auto *trafficText = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(editor && preview && trafficText);
        // 字节数与预览**合成一个标签**之后，"ByteCount" 这个控件不该再存在
        //（曾是两个控件，底边在分数 DPR 下会错开 1 物理像素）。
        QVERIFY2(!window.findChild<QLabel *>(QStringLiteral("ByteCount")),
            "字节数应与预览合并在同一个标签里，不该再有独立的 ByteCount 控件");

        // 四个控件（发送区正文 / 字节数 / 预览、监视区）的字体必须**完全一致**：
        // 字族、像素大小、字重三者都比。
        // ★ 不能只比 family：全局 QSS 会把所有控件的 family 都设成同一个值，那样"同族"是
        //   恒真的、什么也守不住。真正的差异（也是用户看到的）在**字重**——字节数曾是
        //   DemiBold，与同一行的 HEX 挨着显示就像两种字体。
        const QFont ref = editor->font();
        for (auto *w : {static_cast<QWidget *>(editor), static_cast<QWidget *>(preview),
                 static_cast<QWidget *>(trafficText)}) {
            QVERIFY2(w->font().family() == ref.family(),
                qPrintable(QStringLiteral("字体族不一致：%1 是 %2，基准是 %3")
                               .arg(w->objectName(), w->font().family(), ref.family())));
            QCOMPARE(w->font().pixelSize(), ref.pixelSize());
            QVERIFY2(w->font().weight() == ref.weight(),
                qPrintable(QStringLiteral("字重不一致：%1 是 %2，基准是 %3")
                               .arg(w->objectName())
                               .arg(int(w->font().weight()))
                               .arg(int(ref.weight()))));
        }
        // 而且这些控件确实**用上了字体**：pixelSize 有效（不是 -1 那种"没设"的中间态）。
        QVERIFY2(ref.pixelSize() > 0,
            qPrintable(QStringLiteral("字节显示控件的字号应当有确定值，实际 pixelSize=%1")
                           .arg(ref.pixelSize())));

        // 字节数（标签文本里 span 内那一段）必须是**纯 ASCII、且不加空格**（形如 "25B"）：
        //   · ASCII —— 汉字的字面框天生比数字高（实测「字节」段 16px、纯 HEX 段 12px），
        //     与同一行的 HEX 永远对不齐；
        //   · 无空格 —— "25" 只有两个字符，后面再跟空格会让 B 显得孤零零飘在外面。
        const QString html = preview->text();
        const int s0 = html.indexOf(QLatin1Char('>')) + 1;
        const int s1 = html.indexOf(QStringLiteral("</span>"));
        QVERIFY2(s0 > 0 && s1 > s0,
            qPrintable(QStringLiteral("预览标签应含字节数 span：") + html));
        const QString countText = html.mid(s0, s1 - s0);
        QVERIFY2(!countText.isEmpty(), qPrintable(QStringLiteral("字节数不应为空：") + html));
        for (const QChar &ch : countText) {
            QVERIFY2(ch.unicode() < 128,
                qPrintable(QStringLiteral("字节数应为纯 ASCII，实际是「%1」").arg(countText)));
        }
        QVERIFY2(!countText.contains(QLatin1Char(' ')),
            qPrintable(QStringLiteral("字节数不应含空格（B 要紧贴数字）：「%1」").arg(countText)));
    }

    // 待应用提示同时消失。这条用例把该契约钉死。
    void appearanceReprojectKeepsRowSetAndPendingHint()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *keywords = window.findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        auto *apply = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        auto *timestamp = window.findChild<QAction *>(QStringLiteral("prefixTimestamp"));
        QVERIFY(text && model && keywords && apply && timestamp);
        QVERIFY(timestamp->isChecked()); // 默认显示时间戳，下面用它当"外观"开关

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        const auto makeMessage = [endpointId](quint64 id, const char *hex) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray::fromHex(hex);
            message.rawLength = message.raw.size();
            return message;
        };
        // 6 条记录：3 条命中关键词 "aa bb"，3 条不命中。先全部显示（此时还没有关键词）。
        QList<Traffic::Message> batch;
        for (quint64 i = 0; i < 3; ++i) {
            batch.append(makeMessage(i * 2 + 1, "AABB"));
            batch.append(makeMessage(i * 2 + 2, "CCDD"));
        }
        panel->appendMessages(batch);
        QTRY_COMPARE(model->rowCount(), 6);
        QTRY_COMPARE(text->lineCount(), 6);

        // 敲一个只命中部分记录的关键词，**不点**应用过滤：行集不动，只是提示待应用。
        keywords->setText(QStringLiteral("aa bb"));
        QCOMPARE(text->lineCount(), 6);
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"),
            qPrintable(QStringLiteral("改条件后应显示待应用提示：") + apply->text()));

        // 改外观（关掉时间戳）：只换每行长什么样，行集必须一字不变，待应用提示保持。
        timestamp->setChecked(false);
        QTest::qWait(AppearanceReprojectWaitMs); // 设计上必然发生的一次外观重投影，等掉它
        QCOMPARE(text->lineCount(), 6); // 未应用的关键词没有被顺手应用到历史行上
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"),
            qPrintable(QStringLiteral("外观重投影后待应用提示必须仍在：") + apply->text()));

        // 点「应用过滤」：这才把关键词补到历史行上，行集按关键词收窄（只有 3 条命中）。
        apply->click();
        QCOMPARE(text->lineCount(), 3);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤")); // 应用后提示消失

        // ---- 清空之后的同类不变量：清空是会话边界，以当前条件为新会话的已应用基线。----
        keywords->clear(); // 回到无内容条件，再清空
        window.findChild<QPushButton *>(QStringLiteral("clearTraffic"))->click();
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(text->lineCount(), 1); // 空文档保底的一个空块
        // 清空 + 首批数据到达后，按钮文字必须是干净的「应用过滤」：这批数据本就是按当前
        // 条件收的，不该被判成"待应用"（旧实现靠 !m_everProjected 会把 pending 误判为真）。
        QList<Traffic::Message> again;
        for (quint64 i = 0; i < 3; ++i) {
            again.append(makeMessage(100 + i * 2 + 1, "AABB"));
            again.append(makeMessage(100 + i * 2 + 2, "CCDD"));
        }
        panel->appendMessages(again);
        QTRY_COMPARE(model->rowCount(), 6);
        QTRY_COMPARE(text->lineCount(), 6);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤")); // 首批数据不该触发待应用提示

        // 敲一个只命中部分记录的关键词，**不点**应用；再改外观：行集必须一字不变、提示保持
        // ——清空后的会话同样走这条不变量（外观路径用已应用基线重放，不写 m_projectedOptions）。
        keywords->setText(QStringLiteral("aa bb"));
        QCOMPARE(text->lineCount(), 6);
        timestamp->setChecked(true); // 换个外观值（上一段关掉了时间戳）
        QTest::qWait(AppearanceReprojectWaitMs); // 设计上必然发生的一次外观重投影，等掉它
        QCOMPARE(text->lineCount(), 6); // 未应用的关键词没有被顺手应用
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"),
            qPrintable(QStringLiteral("清空后的外观重投影仍应保持待应用提示：") + apply->text()));

        // 点「应用过滤」：行集按关键词收窄。
        apply->click();
        QCOMPARE(text->lineCount(), 3);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
    }

    // QA 独立验收（第二轮）。与工程师的 appearanceReprojectKeepsRowSetAndPendingHint 互补：
    // 这里专门盯"外观路径绝不写 m_projectedOptions"这一条实现约束，逐组合给出
    // "若实现退回旧语义，这条断言会怎么挂"。
    // 关键词选 "aa bb"：hex 紧凑/带空格候选与编码格式无关，用来把变量隔离在"行集"上。
    void appearanceReprojectIndependenceQa()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *keywords = window.findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        auto *apply = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        auto *timestamp = window.findChild<QAction *>(QStringLiteral("prefixTimestamp"));
        auto *format = window.findChild<QComboBox *>(QStringLiteral("trafficFormat"));
        auto *clear = window.findChild<QPushButton *>(QStringLiteral("clearTraffic"));
        QVERIFY(text && model && keywords && apply && timestamp && format && clear);

        // 归一化到确定起点，避免被同一次运行里更早的用例留下的共享偏好影响。
        // 始终显示 HEX 缺省关闭。这里再关一次，保证一行一条。
        keywords->clear();
        format->setCurrentText(QStringLiteral("HEX"));
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(false);
        timestamp->setChecked(true);
        window.findChild<QPushButton *>(QStringLiteral("showPortA"))->setChecked(true);
        if (auto *rx = window.findChild<QPushButton *>(QStringLiteral("filterRx")))
            rx->setChecked(true);
        clear->click();
        QCOMPARE(model->rowCount(), 0);

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        const auto makeMessage = [endpointId](quint64 id, const char *hex) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray::fromHex(hex);
            message.rawLength = message.raw.size();
            return message;
        };
        // 6 条：3 条含 "AA BB"，3 条不含。起点无关键词 → 全显示。
        QList<Traffic::Message> batch;
        for (quint64 i = 0; i < 3; ++i) {
            batch.append(makeMessage(i * 2 + 1, "AABB"));
            batch.append(makeMessage(i * 2 + 2, "CCDD"));
        }
        panel->appendMessages(batch);
        QTRY_COMPARE(model->rowCount(), 6);
        QTRY_COMPARE(text->lineCount(), 6);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤")); // 起点：没有待应用

        // ---- C1：敲关键词不应用 + 改外观 → 行集不变、提示仍在 ----
        keywords->setText(QStringLiteral("aa bb"));
        QCOMPARE(text->lineCount(), 6); // 只改条件没点应用，屏幕不动
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"), "改条件后应出现待应用提示");
        timestamp->setChecked(false); // 外观变化：必然触发一次防抖重投影
        QTest::qWait(AppearanceReprojectWaitMs);
        QCOMPARE(text->lineCount(), 6); // 未应用的关键词没有被外观重投影顺手应用
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"), "外观重投影后提示必须仍在");
        // 旧语义下：外观路径用当前条件过滤 → 行集 6→3；且 markReprojected 把
        // m_projectedOptions 记成当前条件 → 提示被清成"应用过滤"。两条断言都会挂。

        // ---- C2：连点两次外观开关（时间戳 + 编码格式同一轮）→ 行集与提示都不变 ----
        timestamp->setChecked(true);                     // 第一次外观变更
        format->setCurrentText(QStringLiteral("UTF-8")); // 第二次外观变更（防抖窗口内）
        QTest::qWait(AppearanceReprojectWaitMs);
        QCOMPARE(text->lineCount(), 6);
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"), "两次外观变更后提示仍应在");
        // 这一条专门证明**外观路径不写 m_projectedOptions**：若它写了，第一次重投影
        // 之后提示就会被清掉，断言即挂。

        // ---- C3：点应用过滤 → 行集按关键词收窄、提示回到干净；之后再改外观稳定 ----
        apply->click();
        QCOMPARE(text->lineCount(), 3);                       // 只有 3 条命中 "aa bb"
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));   // 已应用 → 提示清干净
        timestamp->setChecked(false); // 再次改外观（当前为 true）
        QTest::qWait(AppearanceReprojectWaitMs);
        QCOMPARE(text->lineCount(), 3);                       // 已应用基线已含关键词 → 仍 3
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));   // 提示保持干净
        // 若 apply 没更新 m_projectedOptions，或外观路径用当前条件却又不更新签名，
        // 会出现"行集对但提示错"或反之，两条断言至少挂一条。

        // ---- C4：清空 + 首批到达 → 按钮文字干净；随后"清空→敲词不应用→改外观" ----
        keywords->clear();
        clear->click();
        QCOMPARE(model->rowCount(), 0);
        QList<Traffic::Message> again;
        for (quint64 i = 0; i < 3; ++i) {
            again.append(makeMessage(100 + i * 2 + 1, "AABB"));
            again.append(makeMessage(100 + i * 2 + 2, "CCDD"));
        }
        panel->appendMessages(again);
        QTRY_COMPARE(model->rowCount(), 6);
        QTRY_COMPARE(text->lineCount(), 6);
        // 清空是会话边界，以当前条件为新基线：首批数据不该被判成"待应用"。
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
        // 旧实现（清空后 m_everProjected=false）会让 refreshApplyHint 判 pending=true
        // → 文字变成"应用过滤 •"，此处断言挂。

        keywords->setText(QStringLiteral("aa bb"));
        QCOMPARE(text->lineCount(), 6);
        timestamp->setChecked(true); // 换外观值（当前为 false）
        QTest::qWait(AppearanceReprojectWaitMs);
        QCOMPARE(text->lineCount(), 6); // 清空后的会话同样不允许外观重投影套上未应用条件
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"), "清空后的外观重投影仍应保留提示");
        // 旧实现的关键回归点：清空后无基线（m_everProjected=false），外观路径会退回
        // "用当前条件"过滤 → 行集 6→3，此条挂。

        // ---- D1：证实"已知残余"与 trafficpanel.cpp:1027-1031 注释一致 ----
        apply->click(); // 建立"已应用基线 = aa bb"
        QCOMPARE(text->lineCount(), 3);
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));
        keywords->setText(QStringLiteral("cc dd")); // 改条件，不应用（已应用基线仍是 aa bb）
        QCOMPARE(text->lineCount(), 3);              // 改条件不影响已有行
        QVERIFY(apply->text() != QStringLiteral("应用过滤"));
        // 新行到达：增量路径用**当前**条件（cc dd）收流，于是这 2 条 CCDD 先显示出来。
        panel->appendMessages({makeMessage(200, "CCDD"), makeMessage(201, "CCDD")});
        QTRY_COMPARE(model->rowCount(), 8);
        QTRY_COMPARE(text->lineCount(), 5); // 只命中新条件的行先显示——注释所述现象
        // 一次外观重投影（按已应用基线 aa bb 重放）把它们**收回**，与注释一致。
        timestamp->setChecked(false);
        QTest::qWait(AppearanceReprojectWaitMs);
        QCOMPARE(text->lineCount(), 3);
        QVERIFY2(apply->text() != QStringLiteral("应用过滤"), "提示仍在（条件尚未应用）");
    }

    // QA 独立验收（第二轮）D2：外观路径的已应用判定器用**当前编码格式**重建
    // （Matcher(options, currentDisplayFormat())），而候选视图含"按当前格式渲染的文本"。
    // 这里实测"只在 DEC 渲染视图里命中"的条件在切到 HEX 后行集是否变化，并固化判定。
    void appearanceReprojectFormatDependentCandidatesQa()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        auto *keywords = window.findChild<QLineEdit *>(QStringLiteral("filterKeywords"));
        auto *apply = window.findChild<QPushButton *>(QStringLiteral("filterRebuild"));
        auto *format = window.findChild<QComboBox *>(QStringLiteral("trafficFormat"));
        auto *clear = window.findChild<QPushButton *>(QStringLiteral("clearTraffic"));
        QVERIFY(text && model && keywords && apply && format && clear);

        keywords->clear();
        clear->click();
        format->setCurrentText(QStringLiteral("DEC")); // DEC 渲染 = 十进制字节值，如 "0 255"
        // 始终显示 HEX 缺省关闭。这里再关一次，保证一行一条。
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(false);
        window.findChild<QPushButton *>(QStringLiteral("showPortA"))->setChecked(true);
        if (auto *rx = window.findChild<QPushButton *>(QStringLiteral("filterRx")))
            rx->setChecked(true);
        QCOMPARE(model->rowCount(), 0);

        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        const auto makeMessage = [endpointId](quint64 id, const char *hex) {
            Traffic::Message message;
            message.id = id;
            message.timestamp = QDateTime::currentDateTime();
            message.direction = Traffic::Direction::Receive;
            message.source.endpointId = endpointId;
            message.source.endpointIndex = 0;
            message.source.endpointKind = PortDraft::Serial;
            message.raw = QByteArray::fromHex(hex);
            message.rawLength = message.raw.size();
            return message;
        };
        // 3 条都含 0xFF：DEC 渲染（"0 255"/"255 1"/"2 255"）都有 "255"；
        // HEX 渲染（"00 FF" 等）与 hex 候选里都没有 "255"。
        panel->appendMessages({makeMessage(1, "00FF"), makeMessage(2, "FF01"), makeMessage(3, "02FF")});
        QTRY_COMPARE(model->rowCount(), 3);
        QTRY_COMPARE(text->lineCount(), 3);

        keywords->setText(QStringLiteral("255")); // 只在 DEC 渲染视图里命中
        apply->click();
        QCOMPARE(text->lineCount(), 3);                     // 应用后 3 条都命中
        QCOMPARE(apply->text(), QStringLiteral("应用过滤"));

        // 切到 HEX：已应用条件 "255" 不变，但外观路径按**当前格式 HEX** 重建判定器，
        // DEC 渲染视图不再是候选。若"外观不改行集"严格成立，这里应仍是 3 行。
        format->setCurrentText(QStringLiteral("HEX"));
        // 首次建窗后的字体扫描可能占住界面线程，固定 90ms 会在重投影之前返回。
        QTRY_COMPARE(text->lineCount(), 1);
        // 实测固化：格式相关候选（DEC 渲染）消失后，3 条全部不再命中 → 只剩空文档保底块。
        // 这与"未应用条件被套上"不同：条件 "255" 早已 apply（m_projectedOptions 就是它），
        // 提示也干净，属于**同一条件在新渲染下的重新求值**。
        QCOMPARE(apply->text(), QStringLiteral("应用过滤")); // 提示干净：不是待应用机制在动行集
    }

    // ANSI 颜色标识：SGR 序列渲染为前景色分片，转义序列本身不进显示文本，
    // 换行还原保持既有语义。命令行式的彩色日志因此原样可读。
    void ansiColorsRenderStripped()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *panel = window.findChild<TrafficPanel *>();
        panel->setPreferences(nullptr);
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        auto *model = window.findChild<TrafficModel *>();
        QVERIFY(text && model);
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));
        // 始终显示 HEX 缺省关闭。这里再关一次，只看报文本身的两块。
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(false);
        const QUuid endpointId = window.findChild<QPushButton *>(QStringLiteral("showPortA"))
                                     ->property("endpoint").toUuid();
        Traffic::Message message;
        message.id = 1;
        message.timestamp = QDateTime::currentDateTime();
        message.direction = Traffic::Direction::Receive;
        message.source.endpointId = endpointId;
        message.source.endpointIndex = 0;
        message.source.endpointKind = PortDraft::Serial;
        message.raw = QByteArray("\x1b[31mRED\x1b[0m plain\r\nnext \x1b[92mgreen");
        message.rawLength = message.raw.size();
        panel->appendMessages({message});
        QTRY_COMPARE(model->rowCount(), 1);
        QCOMPARE(text->document()->blockCount(), 2);
        const auto first = text->document()->firstBlock();
        QVERIFY2(first.text().endsWith(QStringLiteral("RED plain")),
                 qPrintable(QStringLiteral("转义序列应被剥离：") + first.text()));
        QVERIFY2(text->document()->lastBlock().text() == QStringLiteral("next green"),
                 qPrintable(QStringLiteral("续行应无残留序列：")
                     + text->document()->lastBlock().text()));
        // "RED" 分片按 SGR 31 的前景色渲染。
        bool foundRed = false;
        for (auto it = first.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.text() == QStringLiteral("RED"))
                foundRed = fragment.charFormat().foreground().color() == QColor(0xC6, 0x28, 0x28);
        }
        QVERIFY(foundRed);

        // 设备把颜色和普通字重写在同一段：ESC[36;22m。22 只取消加粗，不能把刚设上的青色清掉。
        // 256 色里的标准青（38;5;6）同样要上色。正文不因端口再涂一层。
        text->clearAll();
        message.raw = QByteArray("\x1b[36;22mI/main\x1b[0m plain\x1b[38;5;6mcyan");
        message.rawLength = message.raw.size();
        panel->appendMessages({message});
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("I/main plaincyan")));
        QVERIFY(!text->toPlainText().contains(QStringLiteral("\x1b")));
        bool foundInfo = false;
        bool foundCyan = false;
        const QTextBlock colored = text->document()->lastBlock();
        for (auto it = colored.begin(); !it.atEnd(); ++it) {
            const QColor ink = it.fragment().charFormat().foreground().color();
            if (it.fragment().text() == QStringLiteral("I/main"))
                foundInfo = ink == QColor(0x00, 0x85, 0x77);
            if (it.fragment().text() == QStringLiteral("cyan"))
                foundCyan = ink == QColor(0x00, 0x85, 0x77);
        }
        QVERIFY(foundInfo);
        QVERIFY(foundCyan);
        QCOMPARE(text->lineEndpointAt(text->document()->blockCount() - 1), 0);

        // 串口会把 ESC[36;22m 切在任意字节上。前一包末尾的半截转义不能画成正文，
        // 下一包接上后，I/panel 仍按青色显示。
        text->clearAll();
        message.id = 3;
        message.raw = QByteArray("boot\n\x1b[36;");
        message.rawLength = message.raw.size();
        panel->appendMessages({message});
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("boot")));
        QVERIFY2(!text->toPlainText().contains(QStringLiteral("[36")),
            qPrintable(QStringLiteral("半截转义不该进正文：") + text->toPlainText()));
        QCOMPARE(text->pendingAnsi(), QStringLiteral("\x1b[36;"));
        message.id = 4;
        message.raw = QByteArray("22mI/panel    normal display");
        message.rawLength = message.raw.size();
        panel->appendMessages({message});
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("I/panel    normal display")));
        QVERIFY(!text->toPlainText().contains(QStringLiteral("22m")));
        QVERIFY(text->pendingAnsi().isEmpty());
        bool foundPanel = false;
        for (auto block = text->document()->firstBlock(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                if (it.fragment().text().contains(QStringLiteral("I/panel")))
                    foundPanel = it.fragment().charFormat().foreground().color()
                        == QColor(0x00, 0x85, 0x77);
            }
        }
        QVERIFY(foundPanel);

        // 尾包一直不来时，半截转义不能一直藏着。已经画出来的 boot 仍在，超时后原文补上。
        text->clearAll();
        message.id = 5;
        message.raw = QByteArray("boot\n\x1b[36;");
        message.rawLength = message.raw.size();
        panel->appendMessages({message});
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("boot")));
        QVERIFY(!text->toPlainText().contains(QStringLiteral("[36")));
        QCOMPARE(text->pendingAnsi(), QStringLiteral("\x1b[36;"));
        QTRY_VERIFY_WITH_TIMEOUT(text->toPlainText().contains(QStringLiteral("[36;")), 500);
        QVERIFY(text->pendingAnsi().isEmpty());
        QVERIFY(text->toPlainText().contains(QStringLiteral("boot")));

        // 「显示颜色」关掉后，颜色前后缀按原文出现，正文不再上色。再勾上则按颜色重画。
        text->clearAll();
        auto *showAnsi = window.findChild<QAction *>(QStringLiteral("showAnsi"));
        QVERIFY(showAnsi && showAnsi->isChecked());
        showAnsi->setChecked(false);
        message.id = 6;
        message.raw = QByteArray("\x1b[36;22mI/panel    normal display");
        message.rawLength = message.raw.size();
        panel->appendMessages({message});
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("[36;22m")));
        QVERIFY(text->toPlainText().contains(QStringLiteral("I/panel    normal display")));
        bool coloredWhileOff = false;
        for (auto block = text->document()->firstBlock(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                if (it.fragment().text().contains(QStringLiteral("I/panel"))
                    && it.fragment().charFormat().foreground().color() == QColor(0x00, 0x85, 0x77))
                    coloredWhileOff = true;
            }
        }
        QVERIFY(!coloredWhileOff);

        // HEX 对照是原始字节。颜色关掉时正文能看见前后缀，对照里也必须有这些字节。
        window.findChild<QAction *>(QStringLiteral("alwaysShowHex"))->setChecked(true);
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("1B 5B 33 36 3B 32 32 6D")));
        QVERIFY(text->toPlainText().contains(QStringLiteral("[36;22m")));
        showAnsi->setChecked(true);
        // 关掉时正文里已经有 I/panel。要等重投影把颜色前后缀拿掉，不能只等这几个字出现。
        QTRY_VERIFY(!text->toPlainText().contains(QStringLiteral("[36;22m")));
        QVERIFY(text->toPlainText().contains(QStringLiteral("I/panel    normal display")));
        bool recolored = false;
        for (auto block = text->document()->firstBlock(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                if (it.fragment().text().contains(QStringLiteral("I/panel")))
                    recolored = it.fragment().charFormat().foreground().color()
                        == QColor(0x00, 0x85, 0x77);
            }
        }
        QVERIFY(recolored);
        // 重新着色后正文藏起前后缀，对照仍是原始字节，转义不能因为没画出来就消失。
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("1B 5B 33 36 3B 32 32 6D")));
        QVERIFY(text->toPlainText().contains(QStringLiteral("49 2F 70 61 6E 65 6C")));
    }

    // 接收组帧是端点配置：模式联动显隐、结束符可校验、开关随草稿下发到运行时。
    void receiveFramingControlsFeedTheDraft()
    {
        // 独立选择器即可：组帧是端点配置，不依赖主窗口，也避开串口扫描的时序。
        PortSelectorWidget selectorWidget;
        selectorWidget.show();
        auto *selector = &selectorWidget;
        // 先给串口选个设备：未选设备本身就不合法，会盖住组帧的校验结果。
        selector->setSerialPorts({{"COM17", "USB Serial", {}, "COM17"}});
        selector->findChild<QComboBox *>(QStringLiteral("serial.port"))
            ->setCurrentIndex(selector->findChild<QComboBox *>(QStringLiteral("serial.port"))
                                  ->findData(QStringLiteral("COM17")));
        auto *advanced = selector->findChild<QToolButton *>(QStringLiteral("advancedToggle"));
        QVERIFY(advanced);
        advanced->click(); // 展开高级参数：组帧行挂在类型专属参数之下
        QTest::qWait(20);
        auto *mode = selector->findChild<QComboBox *>(QStringLiteral("framingMode"));
        auto *delimiter = selector->findChild<QLineEdit *>(QStringLiteral("framingDelimiter"));
        auto *idle = selector->findChild<QSpinBox *>(QStringLiteral("framingIdle"));
        auto *threshold = selector->findChild<QSpinBox *>(QStringLiteral("framingThreshold"));
        auto *keepBoundary = selector->findChild<QCheckBox *>(QStringLiteral("framingKeepBoundary"));
        auto *logHints = selector->findChild<QCheckBox *>(QStringLiteral("framingLogHints"));
        QVERIFY(mode && delimiter && idle && threshold && keepBoundary && logHints);
        QCOMPARE(threshold->minimum(), 16);
        QCOMPARE(threshold->maximum(), 65536);
        QCOMPARE(threshold->value(), 2048);
        QCOMPARE(selector->draft().framing.bufferThresholdBytes, 2048);
        QCOMPARE(selector->findChild<QLabel *>(QStringLiteral("framingThresholdUnit"))->text(),
                 QStringLiteral("字节"));
        // 组帧设置收成一行：开关、判定、兜底参数共用同一个父控件。
        auto *framingControls = selector->findChild<QWidget *>(QStringLiteral("framingControls"));
        QVERIFY(framingControls);
        QCOMPARE(mode->parentWidget(), framingControls);
        QCOMPARE(idle->parentWidget(), framingControls);
        QCOMPARE(threshold->parentWidget(), framingControls);
        QCOMPARE(logHints->parentWidget(), framingControls);
        QCOMPARE(keepBoundary->parentWidget(), framingControls);
        // 标签与输入框同一字号。Muted 是 11px，组帧行不能再用它。
        QFile framingStyle(QStringLiteral(":/styles/workbench.qss"));
        QVERIFY(framingStyle.open(QIODevice::ReadOnly));
        const QString previousFramingStyle = qApp->styleSheet();
        qApp->setStyleSheet(QString::fromUtf8(framingStyle.readAll()));
        const auto framingLabels = selector->findChild<QWidget *>(QStringLiteral("framingPanel"))
                                       ->findChildren<QLabel *>();
        QVERIFY(!framingLabels.isEmpty());
        for (auto *widget : selector->findChildren<QWidget *>())
            widget->ensurePolished();
        const int framingPx = idle->font().pixelSize();
        QVERIFY(framingPx > 0);
        for (auto *framingLabel : framingLabels) {
            QVERIFY2(framingLabel->font().pixelSize() == framingPx,
                qPrintable(QStringLiteral("%1 是 %2px，输入框是 %3px")
                               .arg(framingLabel->objectName())
                               .arg(framingLabel->font().pixelSize())
                               .arg(framingPx)));
        }
        qApp->setStyleSheet(previousFramingStyle);
        auto *enabled = selector->findChild<QCheckBox *>(QStringLiteral("framingEnabled"));
        auto *recover = selector->findChild<QCheckBox *>(QStringLiteral("autoRecover"));
        QVERIFY(enabled);
        QVERIFY(recover);
        // 自动恢复在主参数行，和类型、动作按钮同一行，不跟组帧控件藏在高级区。
        auto *typeBox = selector->findChild<QComboBox *>(QStringLiteral("portType"));
        auto *action = selector->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(typeBox && action);
        const int recoverY = recover->mapTo(selector, QPoint(0, 0)).y();
        QVERIFY(qAbs(recoverY - typeBox->mapTo(selector, QPoint(0, 0)).y()) <= 8);
        QVERIFY(recover->mapTo(selector, QPoint(0, 0)).x() > typeBox->mapTo(selector, QPoint(0, 0)).x());
        QVERIFY(recover->mapTo(selector, QPoint(0, 0)).x() < action->mapTo(selector, QPoint(0, 0)).x());
        QVERIFY(recoverY < framingControls->mapTo(selector, QPoint(0, 0)).y());
        QVERIFY(!recover->isChecked());
        recover->setChecked(true);
        QCOMPARE(selector->draft().autoRecover, true);
        // TCP 服务器不提供自动恢复：勾选隐藏，草稿也不再带这个标志。
        typeBox->setCurrentIndex(PortDraft::TcpServer);
        QTest::qWait(20);
        QVERIFY(!recover->isVisible());
        QVERIFY(!selector->draft().autoRecover);
        typeBox->setCurrentIndex(PortDraft::Serial);
        QTest::qWait(20);
        QVERIFY(recover->isVisible());
        QVERIFY(!recover->isChecked());
        QVERIFY(mode->findData(int(ReceiveFraming::IdleTimeout)) >= 0);
        QCOMPARE(idle->value(), 10);
        mode->setCurrentIndex(mode->findData(int(ReceiveFraming::IdleTimeout)));
        QTest::qWait(20);
        QVERIFY(enabled->isChecked());
        QVERIFY(idle->isEnabled());
        QVERIFY(threshold->isEnabled());
        QVERIFY(!keepBoundary->isVisible());
        QVERIFY(selector->hasValidConfiguration());
        const auto valid = [selector](const QString &stage) {
            QVERIFY2(selector->hasValidConfiguration(),
                qPrintable(stage + QStringLiteral("：") + selector->validationError()));
        };
        // 空闲超时模式不需要结束符，输入因此不出现。
        QVERIFY(!delimiter->isVisible());
        valid(QStringLiteral("默认模式"));

        // 结束符模式：结束符必填（HEX），缺失即校验失败。
        mode->setCurrentIndex(mode->findData(int(ReceiveFraming::Delimiter)));
        QTest::qWait(20);
        QVERIFY(delimiter->isVisible());
        QVERIFY2(!selector->hasValidConfiguration(), qPrintable(selector->validationError()));
        delimiter->setText(QStringLiteral("0D 0A"));
        valid(QStringLiteral("填好结束符"));
        idle->setValue(250);
        logHints->setChecked(true);
        const auto draft = selector->draft();
        QCOMPARE(draft.framing.mode, ReceiveFraming::Delimiter);
        QCOMPARE(draft.framing.delimiter, QByteArray::fromHex("0D0A"));
        QCOMPARE(draft.framing.idleTimeoutMs, 250);
        QVERIFY(draft.framing.logHints);

        // 端口未打开时，空结束符只让打开按钮失效，不弹窗打断输入。
        delimiter->clear();
        QTest::qWait(20);
        QVERIFY(selectorWidget.findChildren<QMessageBox *>().isEmpty());
        QVERIFY(!selector->hasValidConfiguration());

        // 截图产物：结束符模式下的组帧行（模式 + 结束符 + 兜底设置 + 说明文字），
        // 供目视检查排版与文案清晰度。
        delimiter->setText(QStringLiteral("0D 0A"));
        mode->setCurrentIndex(mode->findData(int(ReceiveFraming::Delimiter)));
        QTest::qWait(20);
        const auto artifactDir = qEnvironmentVariable("QQCOM_UI_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QDir().mkpath(artifactDir);
            QVERIFY(selector->grab().save(artifactDir + "/framing-row.png"));
        }
    }

    // 自动换行开关：勾选后按视口宽度折行（横向滚动随之失效），取消后恢复水平滚动。
    void wrapToggleSwitchesLineWrapMode()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(text);
        QCOMPARE(text->lineWrapMode(), QPlainTextEdit::WidgetWidth);
        window.findChild<QAction *>(QStringLiteral("autoWrap"))->setChecked(false);
        QCOMPARE(text->lineWrapMode(), QPlainTextEdit::NoWrap);
        window.findChild<QAction *>(QStringLiteral("autoWrap"))->setChecked(true);
        QCOMPARE(text->lineWrapMode(), QPlainTextEdit::WidgetWidth);
    }

    // 回车发送：默认回车仍换行。勾上后回车走发送按钮同一条路，换行改成 Alt+回车。
    void enterToSendUsesAltEnterForNewLine()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("sendEditor"));
        auto *enterToSend = window.findChild<QPushButton *>(QStringLiteral("enterToSend"));
        auto *send = window.findChild<QPushButton *>(QStringLiteral("PrimaryButton"));
        auto *model = window.findChild<TrafficModel *>();
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        auto *targetA = window.findChild<QPushButton *>(QStringLiteral("sendTargetA"));
        auto *targetB = window.findChild<QPushButton *>(QStringLiteral("sendTargetB"));
        QVERIFY(editor && enterToSend && send && model && b && targetA && targetB);

        // offscreen 下 setFocus 不一定生效。事件直接交给输入框，和焦点在框内时同一条过滤路径。
        const auto pressEnter = [editor](Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            QTest::keyClick(editor, Qt::Key_Return, modifiers);
        };
        auto cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::End);
        editor->setTextCursor(cursor);
        const QString before = editor->toPlainText();
        pressEnter();
        QCOMPARE(editor->toPlainText(), before + QLatin1Char('\n'));
        // 把默认回车插进去的换行清掉，后面只断言 Alt+回车。
        editor->setPlainText(QStringLiteral("AT"));

        if (!targetB->isChecked()) targetB->click();
        if (targetA->isChecked()) targetA->click();
        QUdpSocket receiver;
        QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
        PortDraft udp;
        udp.kind = PortDraft::Udp;
        udp.values = {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
                      {"udp.target", "127.0.0.1"}, {"udp.targetPort", receiver.localPort()}};
        b->setDraft(udp);
        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QVERIFY(action);
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("解  绑"));
        auto *format = window.findChild<QComboBox *>(QStringLiteral("sendFormat"));
        auto *suffix = window.findChild<QComboBox *>(QStringLiteral("sendSuffix"));
        QVERIFY(format && suffix);
        format->setCurrentText(QStringLiteral("UTF-8"));
        suffix->setCurrentIndex(0);
        QTRY_VERIFY(send->isEnabled());

        enterToSend->setChecked(true);
        const int rowsBefore = model->rowCount();
        pressEnter();
        QTRY_VERIFY(model->rowCount() > rowsBefore);
        QCOMPARE(editor->toPlainText(), QStringLiteral("AT"));
        QCOMPARE(model->record(model->rowCount() - 1).raw, QByteArray("AT"));
        QTRY_VERIFY(receiver.hasPendingDatagrams());
        QByteArray received(int(receiver.pendingDatagramSize()), Qt::Uninitialized);
        QCOMPARE(receiver.readDatagram(received.data(), received.size()), qint64(received.size()));
        QCOMPARE(received, QByteArray("AT"));

        cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::End);
        editor->setTextCursor(cursor);
        pressEnter(Qt::AltModifier);
        QCOMPARE(editor->toPlainText(), QStringLiteral("AT\n"));
        // Alt+回车只换行，不再多发一拍。
        QCOMPARE(model->rowCount(), rowsBefore + 1);

        auto *clearOnEnter = window.findChild<QPushButton *>(QStringLiteral("clearOnEnter"));
        QVERIFY(clearOnEnter);
        QVERIFY(!clearOnEnter->isChecked());
        clearOnEnter->setChecked(true);
        editor->setPlainText(QStringLiteral("AT"));
        const int rowsBeforeClear = model->rowCount();
        pressEnter();
        QTRY_VERIFY(model->rowCount() > rowsBeforeClear);
        QCOMPARE(editor->toPlainText(), QString());
        QCOMPARE(model->record(model->rowCount() - 1).raw, QByteArray("AT"));

        // 点「发送」和 Alt+回车都不清空。
        editor->setPlainText(QStringLiteral("AT"));
        const int rowsBeforeButton = model->rowCount();
        send->click();
        QTRY_VERIFY(model->rowCount() > rowsBeforeButton);
        QCOMPARE(editor->toPlainText(), QStringLiteral("AT"));
        cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::End);
        editor->setTextCursor(cursor);
        pressEnter(Qt::AltModifier);
        QCOMPARE(editor->toPlainText(), QStringLiteral("AT\n"));
        QCOMPARE(model->rowCount(), rowsBeforeButton + 1);

        // 内容发不出去时，回车也不清空。
        format->setCurrentText(QStringLiteral("HEX"));
        editor->setPlainText(QStringLiteral("GG"));
        QTRY_VERIFY(!send->isEnabled());
        pressEnter();
        QCOMPARE(editor->toPlainText(), QStringLiteral("GG"));

        enterToSend->setChecked(false);
        QVERIFY(!clearOnEnter->isEnabled());
        QVERIFY(clearOnEnter->isChecked());
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("绑  定"));
    }

    // 串口被占用时勾选自动恢复再打开：按钮进入「等待中」，再点一次必须停住，
    // 不能被已经排队的重试失败重新点亮。用绑定冲突的 UDP 代替真实串口占用。
    void occupiedOpenEntersWaitingAndSecondClickCancels()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);

        const auto originalIcon = window.windowIcon().cacheKey();
        auto *alerts = window.findChild<EndpointAlertState *>(QStringLiteral("endpointAlertsB"));
        QVERIFY(alerts);

        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(b);
        b->findChild<QComboBox *>(QStringLiteral("portType"))->setCurrentIndex(PortDraft::Udp);
        QTest::qWait(20);
        auto *recover = b->findChild<QCheckBox *>(QStringLiteral("autoRecover"));
        QVERIFY(recover);
        recover->setChecked(true);

        QUdpSocket held;
        QVERIFY(held.bind(QHostAddress::LocalHost, 0, QUdpSocket::DontShareAddress));
        const quint16 port = held.localPort();
        auto *bind = b->findChild<QComboBox *>(QStringLiteral("udp.address"));
        QVERIFY(bind);
        if (bind->findData(QStringLiteral("127.0.0.1")) < 0)
            bind->addItem(QStringLiteral("127.0.0.1"), QStringLiteral("127.0.0.1"));
        bind->setCurrentIndex(bind->findData(QStringLiteral("127.0.0.1")));
        b->findChild<QSpinBox *>(QStringLiteral("udp.port"))->setValue(port);
        b->findChild<QComboBox *>(QStringLiteral("udp.target"))->setCurrentText(QString());

        auto *action = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        QCOMPARE(action->text(), QStringLiteral("绑  定"));
        action->click();
        QTRY_COMPARE_WITH_TIMEOUT(action->text(), QStringLiteral("等待中"), 4000);
        QCOMPARE(action->property("recovering").toBool(), true);
        QCOMPARE(action->property("open").toBool(), false);
        auto *statusB = window.findChildren<QLabel *>(QStringLiteral("EndpointState")).value(1);
        QVERIFY(statusB);
        QTRY_COMPARE(statusB->text(), QStringLiteral("等待恢复"));
        QCOMPARE(alerts->level(), EndpointAlertState::Error);

        // 真正渲染两套主题，避免属性正确却仍画成蓝色或橙色。
        for (const auto mode : {AppTheme::Mode::Light, AppTheme::Mode::Dark}) {
            AppTheme::setMode(mode);
            const QImage shot = action->grab().toImage();
            int yellow = 0;
            for (int y = 0; y < shot.height(); ++y)
                for (int x = 0; x < shot.width(); ++x) {
                    const auto c = shot.pixelColor(x, y);
                    if (c.red() > 200 && c.green() > 180 && c.blue() < 90) ++yellow;
                }
            QVERIFY2(yellow > shot.width() * shot.height() / 4, "等待中按钮应为黄色");
            const auto dir = qEnvironmentVariable("QQCOM_UI_ARTIFACT_DIR");
            if (!dir.isEmpty()) {
                QVERIFY(QDir().mkpath(dir));
                QVERIFY(window.grab().save(dir + (mode == AppTheme::Mode::Dark
                    ? QStringLiteral("/waiting-dark.png") : QStringLiteral("/waiting-light.png"))));
            }
        }
        AppTheme::setMode(AppTheme::Mode::Light);
        action->click();
        QTRY_COMPARE(action->text(), QStringLiteral("绑  定"));
        QCOMPARE(action->property("recovering").toBool(), false);
        QTRY_COMPARE(statusB->text(), QStringLiteral("未连接"));
        // 占用还在。若取消挡不住在途重试，按钮会在下一拍又回到「等待中」。
        QTest::qWait(1500);
        QCOMPARE(action->text(), QStringLiteral("绑  定"));
        QCOMPARE(statusB->text(), QStringLiteral("未连接"));
        QVERIFY(b->draft().autoRecover);
        QCOMPARE(alerts->level(), EndpointAlertState::Normal);
        QCOMPARE(window.windowIcon().cacheKey(), originalIcon);
    }

    // 桥接转发的日志必须先记本端收到，再记对端转发写出。
    void forwardLogFollowsReceive()
    {
        clearEndpointDrafts();
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 720);
        window.show();
        QTest::qWait(30);
        window.findChild<QComboBox *>(QStringLiteral("trafficFormat"))
            ->setCurrentText(QStringLiteral("UTF-8"));

        auto *a = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointA"));
        auto *b = window.findChild<PortSelectorWidget *>(QStringLiteral("endpointB"));
        QVERIFY(a && b);
        a->findChild<QComboBox *>(QStringLiteral("portType"))->setCurrentIndex(PortDraft::Udp);
        b->findChild<QComboBox *>(QStringLiteral("portType"))->setCurrentIndex(PortDraft::Udp);
        QTest::qWait(20);

        const auto bindLoopback = [](PortSelectorWidget *selector) {
            auto *bind = selector->findChild<QComboBox *>(QStringLiteral("udp.address"));
            if (bind->findData(QStringLiteral("127.0.0.1")) < 0)
                bind->addItem(QStringLiteral("127.0.0.1"), QStringLiteral("127.0.0.1"));
            bind->setCurrentIndex(bind->findData(QStringLiteral("127.0.0.1")));
            selector->findChild<QComboBox *>(QStringLiteral("udp.target"))->setCurrentText(QString());
        };
        bindLoopback(a);
        bindLoopback(b);
        a->findChild<QSpinBox *>(QStringLiteral("udp.port"))->setValue(0);
        b->findChild<QSpinBox *>(QStringLiteral("udp.port"))->setValue(0);
        b->findChild<QComboBox *>(QStringLiteral("udp.target"))->setCurrentText(QStringLiteral("127.0.0.1"));
        b->findChild<QSpinBox *>(QStringLiteral("udp.targetPort"))->setValue(9);

        auto *actionA = a->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        auto *actionB = b->findChild<QPushButton *>(QStringLiteral("endpointAction"));
        actionA->click();
        actionB->click();
        QTRY_COMPARE(actionA->text(), QStringLiteral("解  绑"));
        QTRY_COMPARE(actionB->text(), QStringLiteral("解  绑"));

        auto *text = window.findChild<TrafficTextView *>(QStringLiteral("trafficText"));
        QVERIFY(text);
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("已打开")));
        const QRegularExpression bound(QStringLiteral("127\\.0\\.0\\.1:(\\d+)"));
        const auto boundMatch = bound.match(text->toPlainText());
        QVERIFY2(boundMatch.hasMatch(), qPrintable(text->toPlainText()));
        const quint16 portA = quint16(boundMatch.captured(1).toUInt());
        QVERIFY(portA > 0);

        auto *direction = window.findChild<QComboBox *>(QStringLiteral("routeDirection"));
        auto *bridge = window.findChild<QPushButton *>(QStringLiteral("bridgeButton"));
        QVERIFY(direction && bridge);
        direction->setCurrentIndex(0); // A → B
        bridge->click();
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("桥接已建立")));

        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        QCOMPARE(sender.writeDatagram("ls", QHostAddress::LocalHost, portA), qint64(2));
        QTRY_VERIFY(text->toPlainText().contains(QStringLiteral("TX+FWD")));
        const QString log = text->toPlainText();
        const int rx = log.indexOf(QStringLiteral("[A] RX"));
        const int fwd = log.indexOf(QStringLiteral("[B] TX+FWD"));
        QVERIFY2(rx >= 0 && fwd > rx,
            qPrintable(QStringLiteral("应先收到再转发：\n") + log));
    }
};

int main(int argc, char **argv)
{
    if (argc > 1 && QByteArray(argv[1]) == "--cli") {
        QCoreApplication app(argc, argv);
        app.setOrganizationName("QQComTests");
        app.setApplicationName("QQComUiTests");
        QStandardPaths::setTestModeEnabled(true);
        return Cli::run(app.arguments().mid(2), "test");
    }
    // UI smoke tests must run outside Qt Creator and on headless CI agents.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setOrganizationName("QQComTests");
    app.setApplicationName("QQComUiTests");
    QStandardPaths::setTestModeEnabled(true);
    if (qEnvironmentVariable("QQCOM_CLI_STARTUP_FIXTURE") == "1") {
        MainWindow window;
        QTimer::singleShot(2500, &app, &QCoreApplication::quit);
        return app.exec();
    }
    UiSmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui.moc"
