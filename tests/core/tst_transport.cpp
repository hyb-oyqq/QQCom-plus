#include <QtTest>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <limits>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include "automation/clirunner.h"
#include "automation/commandautomation.h"
#include "automation/capabilitycatalog.h"
#include "app/endpointruntime.h"
#include "app/receiveframer.h"
#include "app/commandmanager.h"
#include "app/commandsendscheduler.h"
#include "app/filetransferservice.h"
#include "persistence/preferencesstore.h"
#include "transport/serialtransport.h"
#include "transport/tcpclienttransport.h"
#include "transport/tcpservertransport.h"
#include "transport/transportfactory.h"
#include "transport/udptransport.h"

using namespace Qt::StringLiterals;

namespace {

class LoopbackRuntime final : public EndpointRuntime
{
    Q_OBJECT

public:
    explicit LoopbackRuntime(QObject *parent = nullptr) : EndpointRuntime(parent) {}

    FileTransferService *sink = nullptr;
    QList<QByteArray> sent;
    qint64 acceptLimit = -1;

public slots:
    qint64 sendPayload(int index, const QByteArray &payload, int origin) override
    {
        Q_UNUSED(origin);
        const auto accepted = acceptLimit < 0 ? payload.size() : qMin<qint64>(acceptLimit, payload.size());
        const auto bytes = payload.first(accepted);
        sent.append(bytes);
        if (sink) sink->rawDataReceived(index, Traffic::Source{}, bytes);
        return accepted;
    }
};

// 传输层无 UI 核心测试：默认走本机回环，不需要真实串口硬件。
// Windows 控制线回归可用 QQCOM_TEST_SERIAL_PORT 指定专用测试/虚拟串口。
// 覆盖四种端点的打开/关闭/收发语义，以及 EndpointRuntime 的消息装配。

constexpr int PortScanBegin = 45450;
constexpr int PortScanEnd = 45500;

PortDraft makeDraft(int kind, const QVariantMap &values)
{
    PortDraft draft;
    draft.kind = kind;
    draft.values = values;
    return draft;
}

int lastState(const QSignalSpy &spy)
{
    return spy.isEmpty() ? -1 : spy.last().at(1).toInt();
}

int peerCount(const QSignalSpy &peerSpy)
{
    return peerSpy.isEmpty() ? 0
        : peerSpy.last().at(1).value<QList<Transport::PeerInfo>>().size();
}

QUuid firstPeerId(const QSignalSpy &peerSpy)
{
    if (peerSpy.isEmpty()) return {};
    const auto peers = peerSpy.last().at(1).value<QList<Transport::PeerInfo>>();
    return peers.isEmpty() ? QUuid() : peers.constFirst().id;
}

// 回环端口存在被其他进程占用的可能：从候选区间里挑一个 listen/bind
// 成功的。start() 的结果是同步发布的，逐个试到成功即可。
template <typename StartFn>
int findFreePort(StartFn &&start)
{
    for (int candidate = PortScanBegin; candidate <= PortScanEnd; ++candidate)
        if (start(candidate)) return candidate;
    return 0;
}

} // namespace

class TransportTest : public QObject
{
    Q_OBJECT

private slots:
    void serialReleasesInheritedControlLines_data()
    {
        QTest::addColumn<QString>("flow");
        QTest::newRow("default") << QString();
        QTest::newRow("none") << QStringLiteral("None");
        QTest::newRow("software") << QStringLiteral("XON/XOFF");
        QTest::newRow("hardware") << QStringLiteral("RTS/CTS");
    }

    void serialReleasesInheritedControlLines()
    {
#ifdef Q_OS_WIN
        const auto portName = qEnvironmentVariable("QQCOM_TEST_SERIAL_PORT");
        if (portName.isEmpty())
            QSKIP("Set QQCOM_TEST_SERIAL_PORT to an unused test/virtual serial port.");
        const auto device = QStringLiteral("\\\\.\\%1").arg(portName);
        const auto openNative = [&device] {
            return ::CreateFileW(reinterpret_cast<LPCWSTR>(device.utf16()),
                GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        };
        HANDLE seed = openNative();
        QVERIFY2(seed != INVALID_HANDLE_VALUE, qPrintable(QString::number(::GetLastError())));
        const auto closeSeed = qScopeGuard([&] {
            if (seed != INVALID_HANDLE_VALUE) ::CloseHandle(seed);
        });
        DCB original{};
        original.DCBlength = sizeof(original);
        QVERIFY(::GetCommState(seed, &original));
        const auto restore = qScopeGuard([&] {
            const auto handle = seed != INVALID_HANDLE_VALUE ? seed : openNative();
            if (handle != INVALID_HANDLE_VALUE) {
                ::SetCommState(handle, &original);
                if (handle != seed) ::CloseHandle(handle);
            }
        });
        // 模拟上一个工具遗留的 DTR 有效、RTS/CTS 握手配置，检查真实驱动 DCB，
        // 不能只比较 QSerialPort 缓存的 flowControl() 属性。
        DCB inherited = original;
        inherited.fDtrControl = DTR_CONTROL_ENABLE;
        inherited.fRtsControl = RTS_CONTROL_HANDSHAKE;
        inherited.fOutxCtsFlow = TRUE;
        inherited.fOutxDsrFlow = TRUE;
        inherited.fDsrSensitivity = TRUE;
        inherited.fInX = FALSE;
        inherited.fOutX = FALSE;
        QVERIFY(::SetCommState(seed, &inherited));
        ::CloseHandle(seed);
        seed = INVALID_HANDLE_VALUE;

        SerialTransport transport(0);
        QSignalSpy states(&transport, &AbstractTransport::stateChanged);
        QSignalSpy errors(&transport, &AbstractTransport::errorOccurred);
        auto draft = makeDraft(PortDraft::Serial, {{"serial.port", portName}});
        QFETCH(QString, flow);
        if (!flow.isEmpty()) draft.values.insert("serial.flow", flow);
        transport.start(draft, 1);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().at(1).toString()));
        QCOMPARE(lastState(states), int(Transport::State::Open));
        auto *port = transport.findChild<QSerialPort *>();
        QVERIFY(port && port->isOpen());
        DCB actual{};
        actual.DCBlength = sizeof(actual);
        QVERIFY(::GetCommState(port->handle(), &actual));
        QCOMPARE(DWORD(actual.fDtrControl), DWORD(DTR_CONTROL_DISABLE));
        QVERIFY(!actual.fOutxDsrFlow);
        QVERIFY(!actual.fDsrSensitivity);
        const bool hardware = flow == QLatin1String("RTS/CTS");
        const bool software = flow == QLatin1String("XON/XOFF");
        QCOMPARE(bool(actual.fOutxCtsFlow), hardware);
        QCOMPARE(DWORD(actual.fRtsControl), DWORD(hardware ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_DISABLE));
        QCOMPARE(bool(actual.fInX), software);
        QCOMPARE(bool(actual.fOutX), software);

        transport.stop();
        seed = openNative();
        QVERIFY(seed != INVALID_HANDLE_VALUE);
        QVERIFY(::GetCommState(seed, &actual));
        // 关闭时也不能恢复打开前的 DTR 有效状态，否则设备会再次受控制线影响。
        QCOMPARE(DWORD(actual.fDtrControl), DWORD(DTR_CONTROL_DISABLE));
        QCOMPARE(bool(actual.fOutxCtsFlow), hardware);
        QVERIFY(!actual.fOutxDsrFlow);
        QVERIFY(!actual.fDsrSensitivity);
        QCOMPARE(DWORD(actual.fRtsControl), DWORD(hardware ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_DISABLE));
        QCOMPARE(bool(actual.fInX), software);
        QCOMPARE(bool(actual.fOutX), software);
#else
        QSKIP("Windows driver control-line regression test.");
#endif
    }

    void cliParsesTypedParametersAndJsonFiles()
    {
        Cli::Command command;
        QString error;
        QVERIFY2(Cli::parse({"configure", "A", "--type", "serial", "--param", "serial.baud=230400",
            "--param", "serial.bits=8", "--auto-recover", "false", "--instance", "123", "--no-start"}, command, error), qPrintable(error));
        QCOMPARE(command.tool, QString("qqcom_configure_endpoint"));
        const auto parameters = command.arguments.value("parameters").toObject();
        QVERIFY(parameters.value("serial.baud").isDouble());
        QCOMPARE(parameters.value("serial.baud").toInt(), 230400);
        QCOMPARE(parameters.value("serial.bits").toString(), QString("8"));
        QCOMPARE(command.instance, QString("123"));
        QVERIFY(!command.autoStart);
        QVERIFY(!command.arguments.value("autoRecover").toBool());

        QTemporaryDir directory;
        QFile file(directory.filePath("arguments.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QJsonObject arguments{{"endpoint", "B"}, {"format", "utf8"}, {"data", QString::fromUtf8("中文 \"$()`\\\n")}};
        file.write(QJsonDocument(arguments).toJson());
        file.close();
        QVERIFY2(Cli::parse({"call", "qqcom_send", "--input-file", file.fileName()}, command, error), qPrintable(error));
        QCOMPARE(command.arguments, arguments);
    }

    void cliRejectsInvalidInput_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::newRow("unknown-command") << QStringList{"missing"};
        QTest::newRow("missing-endpoint") << QStringList{"open"};
        QTest::newRow("wrong-endpoint") << QStringList{"open", "C"};
        QTest::newRow("extra-position") << QStringList{"state", "A"};
        QTest::newRow("irrelevant-option") << QStringList{"state", "--data", "hello"};
        QTest::newRow("missing-payload") << QStringList{"send", "A", "--format", "hex"};
        QTest::newRow("invalid-encoding") << QStringList{"send", "A", "--data", "hello", "--format", "jpeg"};
        QTest::newRow("unknown-parameter") << QStringList{"configure", "A", "--param", "baud=100"};
        QTest::newRow("fractional-integer") << QStringList{"configure", "A", "--param", "client.port=12.3"};
        QTest::newRow("out-of-range") << QStringList{"configure", "A", "--param", "client.port=65536"};
        QTest::newRow("invalid-bool") << QStringList{"configure", "A", "--auto-recover", "yes"};
        QTest::newRow("invalid-timeout") << QStringList{"state", "--timeout", "0"};
        QTest::newRow("invalid-limit") << QStringList{"traffic", "--limit", "201"};
        QTest::newRow("invalid-json") << QStringList{"call", "qqcom_get_state", "--json", "[]"};
        QTest::newRow("conflicting-json") << QStringList{"call", "qqcom_get_state", "--json", "{}", "--input-file", "missing.json"};
        QTest::newRow("missing-file") << QStringList{"call", "qqcom_get_state", "--input-file", "missing.json"};
        QTest::newRow("unknown-tool") << QStringList{"call", "unknown-tool"};
        QTest::newRow("missing-group-name") << QStringList{"commands", "create-group"};
        QTest::newRow("missing-command-content") << QStringList{"commands", "add", "123"};
        QTest::newRow("unknown-command-operation") << QStringList{"commands", "remove-all"};
        QTest::newRow("missing-command-id") << QStringList{"commands", "update", "123"};
        QTest::newRow("invalid-command-format") << QStringList{"commands", "add", "123", "--content", "hello", "--format", "gbk"};
        QTest::newRow("negative-command-position") << QStringList{"commands", "update", "123", "456", "--position", "-1"};
        QTest::newRow("invalid-command-delay") << QStringList{"commands", "add", "123", "--content", "hello", "--delay", "10001"};
        QTest::newRow("invalid-command-enabled") << QStringList{"commands", "add", "123", "--content", "hello", "--enabled", "yes"};
        QTest::newRow("invalid-command-option") << QStringList{"commands", "delete-group", "123", "--name", "unexpected"};
    }

    void cliRejectsInvalidInput()
    {
        QFETCH(QStringList, arguments);
        Cli::Command command;
        QString error;
        QVERIFY(!Cli::parse(arguments, command, error));
        QVERIFY(!error.isEmpty());
    }

    void cliCommandEditingParsesTypedPatches()
    {
        Cli::Command command;
        QString error;
        QVERIFY2(Cli::parse({"commands", "add", "group-id", "--content", "AA 55", "--format", "hex",
            "--line-ending", "crlf", "--delay", "120", "--enabled", "false", "--packed", "true", "--position", "0"}, command, error), qPrintable(error));
        QCOMPARE(command.tool, QString("qqcom_add_command"));
        QCOMPARE(command.arguments.value("delayMs").toInt(), 120);
        QVERIFY(command.arguments.value("enabled").isBool());
        QVERIFY(!command.arguments.value("enabled").toBool());
        QVERIFY(command.arguments.value("packed").toBool());
        QCOMPARE(command.arguments.value("position").toInt(-1), 0);
        QVERIFY2(Cli::parse({"commands", "update", "group-id", "command-id", "--description", "new label"}, command, error), qPrintable(error));
        QCOMPARE(command.arguments.size(), 3);
        QVERIFY(!command.arguments.contains("content"));
        QVERIFY2(Cli::parse({"commands", "list"}, command, error), qPrintable(error));
        QCOMPARE(command.tool, QString("qqcom_list_commands"));
        QVERIFY2(Cli::parse({"commands", "get", "group-id", "--offset", "4", "--limit", "10"}, command, error), qPrintable(error));
        QCOMPARE(command.tool, QString("qqcom_get_command_group"));
        QCOMPARE(command.arguments.value("offset").toInt(), 4);
    }

    void commandMovesAreAtomicAndInvalidateSerializedGroups()
    {
        QTemporaryDir directory;
        PreferencesStore backup(nullptr, directory.filePath("backup.ini"));
        auto *manager = CommandManager::instance();
        manager->save(&backup);
        const auto restore = qScopeGuard([&] { manager->load(&backup); });
        PreferencesStore store(nullptr, directory.filePath("move.ini"));
        manager->load(&store);
        auto source = manager->allSets().first();
        for (int i = 0; i < 4; ++i) {
            CommandItem command;
            command.content = QString::number(i);
            source.commands.append(command);
        }
        manager->syncSet(source.id, source);
        CommandSet target("target");
        manager->addSet(target);
        manager->save(&store); // 先建立序列化缓存。
        QSignalSpy changed(manager, &CommandManager::setsChanged);
        QVERIFY(!manager->moveCommands(source.id, target.id, {0, 99}));
        QVERIFY(!manager->moveCommands(source.id, source.id, {0}));
        QVERIFY(!manager->moveCommands(source.id, QUuid::createUuid(), {0}));
        QVERIFY(!manager->moveCommands(source.id, target.id, {}));
        QCOMPARE(changed.size(), 0);
        QVERIFY(manager->moveCommands(source.id, target.id, {2, 0, 2}));
        QCOMPARE(changed.size(), 1);
        const auto moved = manager->getSet(target.id);
        QCOMPARE(moved.commands, QList<CommandItem>({source.commands.at(0), source.commands.at(2)}));
        QCOMPARE(manager->getSet(source.id).commands,
                 QList<CommandItem>({source.commands.at(1), source.commands.at(3)}));
        manager->save(&store);
        const auto serialized = store.value("commandSets").toJsonArray();
        QCOMPARE(serialized.at(0).toObject(), manager->getSet(source.id).toJson());
        QCOMPARE(serialized.at(1).toObject(), moved.toJson());
        manager->undo();
        manager->save(&store);
        QCOMPARE(store.value("commandSets").toJsonArray().first().toObject(), source.toJson());
        manager->redo();
        manager->save(&store);
        QCOMPARE(store.value("commandSets").toJsonArray(), serialized);
        auto edited = moved;
        edited.commands[0].content = "edited";
        manager->syncSet(edited.id, edited);
        manager->save(&store);
        QCOMPARE(store.value("commandSets").toJsonArray().at(1).toObject(), edited.toJson());
        manager->removeSet(edited.id);
        manager->save(&store);
        QCOMPARE(store.value("commandSets").toJsonArray().size(), 1);
    }

    void commandSerializationPreservesDuplicateLegacyIds()
    {
        QTemporaryDir directory;
        auto *manager = CommandManager::instance();
        PreferencesStore backup(nullptr, directory.filePath("backup.ini"));
        manager->save(&backup);
        const auto restore = qScopeGuard([&] { manager->load(&backup); });
        CommandSet first("first");
        CommandSet second("second");
        second.id = first.id;
        PreferencesStore store(nullptr, directory.filePath("duplicates.ini"));
        const QJsonArray groups{first.toJson(), second.toJson()};
        store.setValue("commandSets", groups);
        manager->load(&store);
        manager->save(&store);
        QCOMPARE(store.value("commandSets").toJsonArray(), groups);
        manager->save(&store);
        QCOMPARE(store.value("commandSets").toJsonArray(), groups);
        QVERIFY(manager->getSet(QUuid::createUuid()).id.isNull());
    }

    void commandSchedulerOrdersStopsAndBoundsTimers()
    {
        CommandSendScheduler scheduler;
        QSignalSpy ready(&scheduler, &CommandSendScheduler::commandReady);
        QSignalSpy finished(&scheduler, &CommandSendScheduler::finished);
        CommandItem first;
        first.delayMs = 0;
        CommandItem second;
        second.delayMs = 25;
        scheduler.start({4, 1}, {first, second}, false);
        QTRY_COMPARE(ready.size(), 2);
        QCOMPARE(ready.at(0).at(0).toInt(), 4);
        QCOMPARE(ready.at(1).at(0).toInt(), 1);
        QTRY_VERIFY(!scheduler.isActive());
        QCOMPARE(finished.size(), 1);
        CommandItem slow;
        slow.delayMs = std::numeric_limits<int>::max();
        QList<int> rows;
        QList<CommandItem> commands;
        for (int i = 0; i < 10000; ++i) { rows.append(i); commands.append(slow); }
        scheduler.start(rows, commands, true);
        QCOMPARE(scheduler.findChildren<QTimer *>().size(), 1);
        QVERIFY(scheduler.isActive());
        scheduler.stop();
        QVERIFY(!scheduler.isActive());
        QVERIFY(!scheduler.findChild<QTimer *>()->isActive());
        QCOMPARE(ready.size(), 2);
        // 停止后重启，不得执行旧批次；信号回调内也允许立即停止。
        connect(&scheduler, &CommandSendScheduler::commandReady, &scheduler,
                [&scheduler] { scheduler.stop(); });
        scheduler.start({8, 9}, {first, first}, true);
        QTRY_COMPARE(ready.size(), 3);
        QTest::qWait(80);
        QCOMPARE(ready.size(), 3);
        QVERIFY(!scheduler.isActive());
        scheduler.start({0}, {}, false);
        QVERIFY(!scheduler.isActive());
    }

    void commandSchedulerCanRestartInsideSendCallback()
    {
        CommandSendScheduler scheduler;
        CommandItem command;
        command.delayMs = 0;
        QList<int> sentRows;
        connect(&scheduler, &CommandSendScheduler::commandReady, &scheduler,
                [&](int row) {
                    sentRows.append(row);
                    if (row == 0) scheduler.start({5}, {command}, false);
                    else scheduler.stop();
                });
        scheduler.start({0, 1}, {command, command}, true);
        QTRY_COMPARE(sentRows, QList<int>({0, 5}));
        QTest::qWait(80);
        QCOMPARE(sentRows, QList<int>({0, 5}));
        QVERIFY(!scheduler.isActive());
    }

    void commandAutomationPatchesOrderUndoAndPersistence()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        PreferencesStore store(nullptr, path);
        auto *manager = CommandManager::instance();
        manager->load(&store);
        const auto call = [&](const QString &tool, const QJsonObject &arguments) {
            return CommandAutomation::execute(manager, tool, arguments);
        };
        const auto created = call("qqcom_create_command_group", {{"name", " Device "}, {"notes", "keep notes"}});
        const auto groupId = created.value("groupId").toString();
        QVERIFY(!QUuid(groupId).isNull());
        const auto first = call("qqcom_add_command", {{"groupId", groupId}, {"content", QString(6000, 'x')},
            {"description", "first"}, {"lineEnding", "crlf"}, {"delayMs", 120}, {"enabled", false}});
        const auto firstId = first.value("commandId").toString();
        const auto secondId = call("qqcom_add_command", {{"groupId", groupId}, {"content", "AA 55"}, {"format", "hex"}}).value("commandId").toString();
        const auto page = call("qqcom_get_command_group", {{"groupId", groupId}, {"limit", 1}});
        QVERIFY(page.value("hasMore").toBool());
        QCOMPARE(page.value("commands").toArray().first().toObject().value("content").toString().size(), 6000);
        QCOMPARE(call("qqcom_get_command_group", {{"groupId", groupId}, {"offset", page.value("nextOffset")}})
            .value("commands").toArray().first().toObject().value("id").toString(), secondId);
        QVERIFY(call("qqcom_update_command", {{"groupId", groupId}, {"commandId", firstId}, {"description", "renamed"}}).value("updated").toBool());
        auto group = manager->getSet(QUuid(groupId));
        QCOMPARE(group.commands.first().content.size(), 6000);
        QCOMPARE(group.commands.first().delayMs, 120);
        QCOMPARE(group.commands.first().tailBytes(), QByteArray("\r\n"));
        QVERIFY(!group.commands.first().enabled);
        QVERIFY(call("qqcom_update_command", {{"groupId", groupId}, {"commandId", secondId}, {"position", 0}, {"packed", true}}).value("updated").toBool());
        group = manager->getSet(QUuid(groupId));
        QCOMPARE(group.commands.first().id, QUuid(secondId));
        QVERIFY(group.commands.first().isHex);
        QCOMPARE(group.commands.first().packMode, QStringLiteral("组包"));
        manager->undo();
        QCOMPARE(manager->getSet(QUuid(groupId)).commands.first().id, QUuid(firstId));
        manager->redo();
        QCOMPARE(manager->getSet(QUuid(groupId)).commands.first().id, QUuid(secondId));
        QVERIFY(call("qqcom_update_command_group", {{"groupId", groupId}, {"name", "renamed group"}}).value("updated").toBool());
        QCOMPARE(manager->getSet(QUuid(groupId)).notes, QString("keep notes"));
        manager->save(&store);
        QVERIFY(store.sync());
        PreferencesStore reloaded(nullptr, path);
        manager->load(&reloaded);
        QCOMPARE(manager->getSet(QUuid(groupId)).name, QString("renamed group"));
        QCOMPARE(manager->getSet(QUuid(groupId)).commands.at(1).id, QUuid(firstId));
        QCOMPARE(manager->getSet(QUuid(groupId)).commands.at(1).content.size(), 6000);
        QVERIFY(call("qqcom_delete_command", {{"groupId", groupId}, {"commandId", firstId}}).value("deleted").toBool());
        QCOMPARE(manager->getSet(QUuid(groupId)).commands.size(), 1);
        QVERIFY(call("qqcom_delete_command_group", {{"groupId", groupId}}).value("deleted").toBool());
        QCOMPARE(manager->setCount(), 1);
    }

    void commandAutomationRejectsInvalidEditsAtomically()
    {
        QTemporaryDir directory;
        PreferencesStore store(nullptr, directory.filePath("preferences.ini"));
        auto *manager = CommandManager::instance();
        manager->load(&store);
        auto group = manager->allSets().first();
        CommandItem command;
        command.content = "unchanged";
        group.commands.append(command);
        manager->syncSet(group.id, group);
        const auto groupId = group.id.toString();
        const auto commandId = command.id.toString();
        const auto before = group.toJson();
        QSignalSpy changed(manager, &CommandManager::setsChanged);
        const QList<QPair<QString, QJsonObject>> edits{
            {"qqcom_delete_command_group", {{"groupId", groupId}}},
            {"qqcom_update_command_group", {{"groupId", groupId}, {"name", "   "}}},
            {"qqcom_update_command_group", {{"groupId", groupId}}},
            {"qqcom_update_command_group", {{"groupId", QUuid::createUuid().toString()}, {"name", "missing"}}},
            {"qqcom_delete_command_group", {{"groupId", "not-a-uuid"}}},
            {"qqcom_add_command", {{"groupId", groupId}, {"content", "new"}, {"position", 2}}},
            {"qqcom_update_command", {{"groupId", groupId}, {"commandId", commandId}, {"content", "new"}, {"position", 1}}},
            {"qqcom_update_command", {{"groupId", groupId}, {"commandId", commandId}, {"delayMs", 10001}}},
            {"qqcom_update_command", {{"groupId", groupId}, {"commandId", commandId}, {"enabled", "false"}}},
            {"qqcom_update_command", {{"groupId", groupId}, {"commandId", commandId}}},
            {"qqcom_delete_command", {{"groupId", groupId}, {"commandId", QUuid::createUuid().toString()}}},
            {"qqcom_update_command", {{"groupId", groupId}, {"commandId", commandId}, {"id", QUuid::createUuid().toString()}}}};
        for (const auto &edit : edits) {
            const auto result = CommandAutomation::execute(manager, edit.first, edit.second);
            QVERIFY2(result.contains("error"), qPrintable(edit.first));
            QCOMPARE(manager->getSet(group.id).toJson(), before);
            QVERIFY(!manager->canUndo());
        }
        QCOMPARE(changed.size(), 0);
    }

    void recoveryCountsIssuesWithoutRepeatingErrorLogs()
    {
        EndpointRuntime runtime;
        QSignalSpy issues(&runtime, &EndpointRuntime::endpointIssueReported);
        QList<Traffic::Message> errors;
        connect(&runtime, &EndpointRuntime::messageReceived, this, [&](const Traffic::Message &message) {
            if (message.direction == Traffic::Direction::Error) errors.append(message);
        });
        auto draft = makeDraft(PortDraft::Serial,
            {{"serial.port", "COM_ALERT_TEST_MISSING"}, {"serial.baud", 115200}});
        draft.autoRecover = true;
        runtime.openEndpoint(0, draft);
        runtime.recoverEndpoint(0, draft, true);
        runtime.recoverEndpoint(0, draft, true);
        QCOMPARE(issues.size(), 3);
        QCOMPARE(issues.first().at(1).toInt(), int(Traffic::Issue::EndpointFailure));
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().issue, Traffic::Issue::EndpointFailure);
        runtime.suppressRecover(0);
        runtime.recoverEndpoint(0, draft, true);
        QCOMPARE(issues.size(), 3);
    }

    void preferencesMigrationPreservesBothFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto source = dir.filePath("old/preferences.ini");
        const auto target = dir.filePath("new/preferences.ini");
        QString error;
        QVERIFY(PreferencesStore::migrateLegacyFile(source, target, &error));
        QVERIFY(!QFileInfo::exists(target));
        {
            PreferencesStore oldStore(nullptr, source);
            oldStore.setValue("commands/sets", QByteArray("legacy-command-data"));
            oldStore.setValue("serial/baud", 2000000);
        }
        QFile original(source);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto originalBytes = original.readAll();
        original.close();
        QVERIFY2(PreferencesStore::migrateLegacyFile(source, target, &error), qPrintable(error));
        {
            PreferencesStore newStore(nullptr, target);
            QCOMPARE(newStore.value("commands/sets").toByteArray(), QByteArray("legacy-command-data"));
            QCOMPARE(newStore.value("serial/baud").toInt(), 2000000);
            newStore.setValue("serial/baud", 115200);
        }
        QVERIFY(PreferencesStore::migrateLegacyFile(source, target, &error));
        PreferencesStore newStore(nullptr, target);
        QCOMPARE(newStore.value("serial/baud").toInt(), 115200);
        QVERIFY(!PreferencesStore::migrateLegacyFile(source, source + "/blocked.ini", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), originalBytes);
    }

    void runtimeRetriesOnlyTheNamedSerialPort()
    {
        EndpointRuntime runtime;
        QList<int> states;
        connect(&runtime, &EndpointRuntime::endpointStateChanged, this,
            [&states](int, int state, const QString &) { states.append(state); });

        PortDraft missing;
        missing.kind = PortDraft::Serial;
        missing.autoRecover = true;
        missing.values.insert(QStringLiteral("serial.port"), QStringLiteral("COM_MISSING"));
        missing.values.insert(QStringLiteral("serial.baud"), QStringLiteral("115200"));
        runtime.recoverEndpoint(0, missing, false);
        QCOMPARE(states.size(), 0);
        QVERIFY(runtime.findChildren<AbstractTransport *>().isEmpty());

        runtime.recoverEndpoint(0, missing, true);
        QTRY_VERIFY(!states.isEmpty());
        QCOMPARE(states.last(), int(Transport::State::Closed));
    }

    void framerHandlesFragmentedBoundariesAndTinyThreshold()
    {
        ReceiveFraming config;
        config.mode = ReceiveFraming::Delimiter;
        config.delimiter = "aba";
        config.bufferThresholdBytes = 100000;
        ReceiveFramer framer(config);
        QVERIFY(framer.feed("xxab").isEmpty());
        const auto frames = framer.feed("aabaYYab");
        QCOMPARE(frames.size(), 2);
        QCOMPARE(frames[0].bytes, QByteArray("xx"));
        QCOMPARE(frames[1].bytes, QByteArray());
        QCOMPARE(framer.feed("a").first().bytes, QByteArray("YY"));
        QVERIFY(framer.flush(ReceiveFramer::Closed).isEmpty());

        config.mode = ReceiveFraming::IdleTimeout;
        config.bufferThresholdBytes = 1;
        framer.setConfig(config);
        const auto partial = framer.feed(QByteArray(1, '\x02'));
        QCOMPARE(partial.size(), 1);
        QCOMPARE(partial.first().trigger, ReceiveFramer::Threshold);
        QCOMPARE(framer.pendingBytes(), 0);
    }

    void runtimeSeparatesTcpPeerFramesAndFlushesDisconnect()
    {
        QList<Traffic::Message> received;
        EndpointRuntime runtime;
        auto draft = makeDraft(PortDraft::TcpServer,
            {{"server.address", "127.0.0.1"}, {"server.port", 0}});
        draft.framing.mode = ReceiveFraming::Delimiter;
        draft.framing.delimiter = "\n";
        draft.framing.idleTimeoutMs = 5000;
        connect(&runtime, &EndpointRuntime::messageReceived, this,
            [&received](const Traffic::Message &message) {
                if (message.direction == Traffic::Direction::Receive) received.append(message);
            });
        runtime.openEndpoint(0, draft);
        auto *listener = runtime.findChild<QTcpServer *>();
        QVERIFY(listener && listener->isListening());
        auto *transport = runtime.findChild<AbstractTransport *>();
        QSignalSpy chunks(transport, &AbstractTransport::dataReceived);
        QTcpSocket first, second;
        first.connectToHost(QHostAddress::LocalHost, listener->serverPort());
        second.connectToHost(QHostAddress::LocalHost, listener->serverPort());
        QTRY_COMPARE(first.state(), QAbstractSocket::ConnectedState);
        QTRY_COMPARE(second.state(), QAbstractSocket::ConnectedState);
        first.write("first-tail");
        QTRY_COMPARE(chunks.size(), 1);
        second.write("second\n");
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(received[0].raw, QByteArray("second"));
        QVERIFY(received[0].source.peerAddress.endsWith(QString(":%1").arg(second.localPort())));
        const auto firstSource = chunks.first()[1].value<Traffic::Source>();
        first.disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(received.size(), 2, 1000);
        QCOMPARE(received[1].raw, QByteArray("first-tail"));
        QCOMPARE(received[1].source.peerId, firstSource.peerId);
        QCOMPARE(received[1].source.peerAddress, firstSource.peerAddress);
        second.write("shutdown-tail");
        QTRY_COMPARE(chunks.size(), 3);
        runtime.closeEndpoint(0);
        QCOMPARE(received.size(), 3);
        QCOMPARE(received.last().raw, QByteArray("shutdown-tail"));
        QCOMPARE(received.last().source.peerId, received.first().source.peerId);
    }

    void runtimeIdleRestartsAfterEveryArrival()
    {
        QList<Traffic::Message> received;
        EndpointRuntime runtime;
        auto draft = makeDraft(PortDraft::TcpServer,
            {{"server.address", "127.0.0.1"}, {"server.port", 0}});
        draft.framing.mode = ReceiveFraming::IdleTimeout;
        draft.framing.idleTimeoutMs = 240;
        connect(&runtime, &EndpointRuntime::messageReceived, this,
            [&received](const Traffic::Message &message) {
                if (message.direction == Traffic::Direction::Receive) received.append(message);
            });
        runtime.openEndpoint(0, draft);
        auto *listener = runtime.findChild<QTcpServer *>();
        QVERIFY(listener && listener->isListening());
        QSignalSpy chunks(runtime.findChild<AbstractTransport *>(), &AbstractTransport::dataReceived);
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, listener->serverPort());
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        client.write("one");
        QTRY_COMPARE(chunks.size(), 1);
        QTest::qWait(140);
        client.write("two");
        QTRY_COMPARE(chunks.size(), 2);
        QTest::qWait(120); // 已过首次输入的期限，但距最后输入尚不足 240 ms。
        QVERIFY(received.isEmpty());
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(received.first().raw, QByteArray("onetwo"));
        QVERIFY(!received.first().source.peerId.isNull());
        runtime.closeEndpoint(0);
    }

    void tcpServerStopsMultipleActivePeers()
    {
        TcpServerTransport server(0);
        server.start(makeDraft(PortDraft::TcpServer,
            {{"server.address", "127.0.0.1"}, {"server.port", 0}}), 1);
        auto *listener = server.findChild<QTcpServer *>();
        QVERIFY(listener && listener->isListening());
        QSignalSpy peers(&server, &AbstractTransport::peerListChanged);
        QTcpSocket first, second;
        first.connectToHost(QHostAddress::LocalHost, listener->serverPort());
        second.connectToHost(QHostAddress::LocalHost, listener->serverPort());
        QTRY_COMPARE(peerCount(peers), 2);
        server.stop();
        server.stop(); // 幂等关闭。
        QTRY_COMPARE(first.state(), QAbstractSocket::UnconnectedState);
        QTRY_COMPARE(second.state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(server.write("closed", {}), qint64(-1));
    }

    void commandManagerUsesSelectedFileFormat()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PreferencesStore store(nullptr, directory.filePath("preferences.ini"));
        auto *manager = CommandManager::instance();
        manager->load(&store);
        CommandSet group(QStringLiteral("设备指令"));
        CommandItem command;
        command.content = "01 AB FF";
        command.isHex = true;
        command.description = "probe";
        command.tail = "CR+LF";
        group.commands.append(command);
        for (const auto format : CommandFormats::allFormats()) {
            const auto path = directory.filePath("commands." + CommandFormats::suffix(format));
            QString error, detectedLabel;
            QVERIFY2(manager->exportToFile(path, format, {group}, error), qPrintable(error));
            QList<CommandSet> parsed;
            CommandFormats::Format detected;
            QVERIFY(CommandFormats::parseFile(path, parsed, error, &detected));
            QCOMPARE(detected, format);
            QCOMPARE(parsed.size(), 1);
            QCOMPARE(parsed.first().commands.first().content, command.content);
            const auto before = manager->setCount();
            QVERIFY2(manager->importFromFile(path, error, &detectedLabel), qPrintable(error));
            QCOMPARE(manager->setCount(), before + 1);
            QCOMPARE(detectedLabel, CommandFormats::label(format));
            QVERIFY(manager->allSets().last().id != group.id);
            manager->undo();
            QCOMPARE(manager->setCount(), before);
        }
        QString error;
        QVERIFY(!manager->exportToFile(directory.filePath("missing/commands.json"),
            CommandFormats::Format::QQCom, {group}, error));
        QVERIFY(!error.isEmpty());
    }

    // VOFA+：外壳 ctx 不建分组；第一级名称是分组；更深的分组只是表里的标题行。
    void commandFormatsImportVofaTree()
    {
        QVERIFY(!CommandFormats::allFormats().contains(CommandFormats::Format::Vofa));

        const char *fixture = R"({
            "type": "cmds",
            "ctx": {
                "name": "group",
                "is_group": true,
                "subCmds": [
                    {
                        "name": "MG指令",
                        "is_group": true,
                        "subCmds": [
                            {
                                "name": "控制指令",
                                "is_group": true,
                                "subCmds": [
                                    {
                                        "name": "复位",
                                        "hex_on": true,
                                        "is_group": false,
                                        "loop_ms": 3000,
                                        "cmd_hex": "5A A5 01 ",
                                        "subCmds": []
                                    }
                                ]
                            }
                        ]
                    },
                    {
                        "name": "知象转台",
                        "is_group": true,
                        "subCmds": [
                            {
                                "name": "芯片复位",
                                "hex_on": false,
                                "is_group": false,
                                "cmd_hex": "2B 43 52 2C 52 45 53 45 54 3B",
                                "subCmds": []
                            },
                            {
                                "name": "航向控制",
                                "is_group": false,
                                "subCmds": [
                                    {
                                        "name": "航向回零",
                                        "hex_on": false,
                                        "is_group": false,
                                        "cmd_hex": "2B 43 54 2C 54 4F 5A 45 52 4F 3B",
                                        "subCmds": []
                                    }
                                ]
                            }
                        ]
                    },
                    {
                        "name": "裸指令",
                        "hex_on": false,
                        "is_group": false,
                        "cmd_hex": "41 42",
                        "subCmds": []
                    }
                ]
            }
        })";

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("vofa.cmds.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(file.write(fixture) > 0);
        file.close();

        QList<CommandSet> sets;
        QString error;
        CommandFormats::Format detected;
        QVERIFY2(CommandFormats::parseFile(path, sets, error, &detected), qPrintable(error));
        QCOMPARE(detected, CommandFormats::Format::Vofa);
        QCOMPARE(CommandFormats::label(detected), QStringLiteral("VOFA+ (*.json)"));
        QCOMPARE(sets.size(), 3);
        QCOMPARE(sets.at(0).name, QStringLiteral("MG指令"));
        QCOMPARE(sets.at(1).name, QStringLiteral("知象转台"));
        QCOMPARE(sets.at(2).name, QStringLiteral("未命名分组"));
        for (const auto &set : sets)
            QVERIFY(set.name != QStringLiteral("group"));

        const auto &mg = sets.at(0).commands;
        QCOMPARE(mg.size(), 2);
        QCOMPARE(mg.at(0).description, QStringLiteral("控制指令"));
        QCOMPARE(mg.at(0).content, QStringLiteral("控制指令"));
        QCOMPARE(mg.at(0).enabled, false);
        QCOMPARE(mg.at(0).isHex, false);
        QCOMPARE(mg.at(1).description, QStringLiteral("复位"));
        QCOMPARE(mg.at(1).isHex, true);
        QCOMPARE(mg.at(1).content, QStringLiteral("5A A5 01"));
        QCOMPARE(mg.at(1).delayMs, 50); // loop_ms 不进延时
        QCOMPARE(mg.at(1).enabled, true);

        const auto &turn = sets.at(1).commands;
        QCOMPARE(turn.size(), 3);
        QCOMPARE(turn.at(0).description, QStringLiteral("芯片复位"));
        QCOMPARE(turn.at(0).isHex, false);
        QCOMPARE(turn.at(0).content, QStringLiteral("+CR,RESET;"));
        QCOMPARE(turn.at(1).description, QStringLiteral("航向控制"));
        QCOMPARE(turn.at(1).content, QStringLiteral("航向控制"));
        QCOMPARE(turn.at(1).enabled, false);
        QCOMPARE(turn.at(2).content, QStringLiteral("+CT,TOZERO;"));

        QCOMPARE(sets.at(2).commands.at(0).content, QStringLiteral("AB"));

        const CommandItem roundTrip = CommandItem::fromJson(mg.at(0).toJson());
        QCOMPARE(roundTrip.content, QStringLiteral("控制指令"));
        QCOMPARE(roundTrip.description, QStringLiteral("控制指令"));
        QCOMPARE(roundTrip.enabled, false);

        QString exportError;
        QVERIFY(!CommandFormats::writeFile(directory.filePath("out.json"),
            CommandFormats::Format::Vofa, sets, exportError));
        QVERIFY(exportError.contains(QStringLiteral("VOFA+")));

        const QString emptyPath = directory.filePath("empty.json");
        QFile emptyFile(emptyPath);
        QVERIFY(emptyFile.open(QIODevice::WriteOnly | QIODevice::Text));
        emptyFile.write(R"({"type":"cmds","ctx":{"name":"group","subCmds":[]}})");
        emptyFile.close();
        QList<CommandSet> emptySets;
        QVERIFY(!CommandFormats::parseFile(emptyPath, emptySets, error, &detected));
        QVERIFY(error.contains(QStringLiteral("VOFA+")));

        // 用户给的样本：四组，外壳 "group" 不出现；子级分组只是标题。
        const QString sample = QStringLiteral("C:/Users/qinjm/OneDrive/Desktop/vofa+.cmds.json");
        if (QFile::exists(sample)) {
            QList<CommandSet> real;
            QVERIFY2(CommandFormats::parseFile(sample, real, error, &detected), qPrintable(error));
            QCOMPARE(detected, CommandFormats::Format::Vofa);
            QCOMPARE(real.size(), 4);
            QCOMPARE(real.at(0).name, QStringLiteral("MG指令"));
            QCOMPARE(real.at(1).name, QStringLiteral("知象转台"));
            QCOMPARE(real.at(2).name, QStringLiteral("SharkProtocol"));
            QCOMPARE(real.at(3).name, QStringLiteral("命令组3"));
            const QStringList mgTitles = {
                QStringLiteral("控制指令"), QStringLiteral("波形指令"),
                QStringLiteral("校准指令"), QStringLiteral("测试指令")};
            int titles = 0;
            for (const auto &cmd : real.at(0).commands) {
                if (!mgTitles.contains(cmd.description)) continue;
                QCOMPARE(cmd.content, cmd.description);
                QCOMPARE(cmd.enabled, false);
                ++titles;
            }
            QCOMPARE(titles, mgTitles.size());
            bool reset = false;
            bool heading = false;
            for (const auto &cmd : real.at(1).commands) {
                if (cmd.description == QStringLiteral("芯片复位")) {
                    QCOMPARE(cmd.content, QStringLiteral("+CR,RESET;"));
                    QCOMPARE(cmd.isHex, false);
                    reset = true;
                }
                if (cmd.description == QStringLiteral("航向控制")) {
                    QCOMPARE(cmd.content, QStringLiteral("航向控制"));
                    QCOMPARE(cmd.enabled, false);
                    heading = true;
                }
            }
            QVERIFY(reset);
            QVERIFY(heading);
            QVERIFY(!real.at(2).commands.isEmpty());
            for (const auto &cmd : real.at(2).commands)
                QVERIFY(!cmd.content.trimmed().isEmpty());
        }
    }

    // 接收组帧器：纯逻辑，不需要端口与事件循环——把字节流片段喂进去，
    // 看它按定界/长度/兜底三种规则吐出什么。
    void framerSplitsOnDelimiter()
    {
        ReceiveFraming config;
        config.mode = ReceiveFraming::Delimiter;
        config.delimiter = QByteArray::fromHex("0D0A");
        config.bufferThresholdBytes = 1000;
        ReceiveFramer framer(config);

        // 半帧先到：缓冲里没有结束符，一条都不该输出。
        QVERIFY(framer.feed(QByteArray("abc")).isEmpty());
        QCOMPARE(framer.pendingBytes(), 3);
        // 结束符补齐：出一帧，默认剥离结束符。
        auto frames = framer.feed(QByteArray("\r\n"));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).bytes, QByteArray("abc"));
        QVERIFY(frames.at(0).complete);
        QCOMPARE(framer.pendingBytes(), 0);

        // 一次收到两帧（粘包）+ 下一帧的前半段：两帧即时输出，半个留下。
        frames = framer.feed(QByteArray("one\r\ntwo\r\nthr"));
        QCOMPARE(frames.size(), 2);
        QCOMPARE(frames.at(0).bytes, QByteArray("one"));
        QCOMPARE(frames.at(1).bytes, QByteArray("two"));
        QCOMPARE(framer.pendingBytes(), 3);
    }

    void framerSplitsOnIdleTimeout()
    {
        ReceiveFraming config;
        config.mode = ReceiveFraming::IdleTimeout;
        config.idleTimeoutMs = 10;
        config.bufferThresholdBytes = 8;
        ReceiveFramer framer(config);

        QVERIFY(framer.feed(QByteArray("abc")).isEmpty());
        QCOMPARE(framer.pendingBytes(), 3);
        auto frames = framer.flush(ReceiveFramer::Idle);
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).bytes, QByteArray("abc"));
        QVERIFY(!frames.at(0).complete);
        QCOMPARE(frames.at(0).trigger, ReceiveFramer::Idle);
        QCOMPARE(framer.pendingBytes(), 0);

        // 空闲模式仍受长度兜底限制，单次输入达到阈值立即输出。
        frames = framer.feed(QByteArray(8, 'x'));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).trigger, ReceiveFramer::Threshold);
        QCOMPARE(frames.at(0).bytes, QByteArray(8, 'x'));
    }

    void framerFallbacksBoundBuffer()
    {
        // 缓冲阈值：慢速/无定界协议不能无限滞留，也不能无界增长。
        ReceiveFraming config;
        config.mode = ReceiveFraming::Delimiter;
        config.delimiter = QByteArray::fromHex("0D0A");
        config.bufferThresholdBytes = 8;
        config.logHints = true;
        ReceiveFramer framer(config);

        auto frames = framer.feed(QByteArray(8, 'x'));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0).bytes, QByteArray(8, 'x'));
        QVERIFY(!frames.at(0).complete); // 兜底切出来的仍是流片段
        QCOMPARE(frames.at(0).trigger, ReceiveFramer::Threshold);
        QVERIFY(!frames.at(0).hint.isEmpty());
        QCOMPARE(framer.pendingBytes(), 0);

        // 空闲超时：flush 把残留输出，空缓冲则什么也不发。
        framer.feed(QByteArray("tail"));
        QCOMPARE(framer.pendingBytes(), 4);
        auto idle = framer.flush(ReceiveFramer::Idle);
        QCOMPARE(idle.size(), 1);
        QCOMPARE(idle.at(0).bytes, QByteArray("tail"));
        QCOMPARE(idle.at(0).trigger, ReceiveFramer::Idle);
        QVERIFY(framer.flush(ReceiveFramer::Idle).isEmpty());

        // 低于阈值的残留才会留在缓冲区，关闭时应由 Closed 收尾输出。
        framer.feed(QByteArray("end"));
        auto closed = framer.flush(ReceiveFramer::Closed);
        QCOMPARE(closed.size(), 1);
        QCOMPARE(closed.at(0).bytes, QByteArray("end"));
        QCOMPARE(closed.at(0).trigger, ReceiveFramer::Closed);

        // 直通模式：收到什么出什么，不做任何拆分。
        ReceiveFramer passthrough{ReceiveFraming()};
        auto out = passthrough.feed(QByteArray("a\r\nb"));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).bytes, QByteArray("a\r\nb"));
        QVERIFY(out.at(0).complete);
    }

    // 组帧端到端：字节流经 EndpointRuntime 的组帧器后，界面看到的是帧而不是片段。
    // 引擎的单测只覆盖 feed/flush，这里覆盖"传输事件 → 组帧 → 消息装配"这条链路。
    void runtimeFramesByteStreamByDelimiter()
    {
        EndpointRuntime runtime;
        QSignalSpy states(&runtime, &EndpointRuntime::endpointStateChanged);
        QSignalSpy messages(&runtime, &EndpointRuntime::messageReceived);

        // 先借一个空闲端口：运行时反复开同一端点（扫描候选端口）会与传输对象的
        // deleteLater 收尾打架，这里只开一次。
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();

        PortDraft draft = makeDraft(PortDraft::TcpServer,
            {{"server.address", "127.0.0.1"}, {"server.port", int(port)}, {"server.clients", 4}});
        ReceiveFraming framing;
        framing.mode = ReceiveFraming::Delimiter;
        framing.delimiter = QByteArray::fromHex("0D0A");
        framing.idleTimeoutMs = 120;
        framing.bufferThresholdBytes = 300;
        framing.logHints = true; // 兜底输出要发一条信息
        draft.framing = framing;

        runtime.openEndpoint(0, draft);
        QTRY_COMPARE(lastState(states), int(Transport::State::Open));

        // 只看接收消息：状态/错误事件混在同一个信号里，断言前先按方向过滤。
        const auto received = [&messages] {
            QList<Traffic::Message> rx;
            for (const auto &params : messages) {
                const auto message = params.at(0).value<Traffic::Message>();
                if (message.direction == Traffic::Direction::Receive) rx.append(message);
            }
            return rx;
        };

        // 空闲超时从“收到第一段数据”开始计算；端口刚打开时没有数据，
        // 等待一个超时时间也不应产生空的接收消息。
        QTest::qWait(160);
        QCOMPARE(received().size(), 0);

        QTcpSocket client;
        client.connectToHost(QStringLiteral("127.0.0.1"), port);
        QVERIFY(client.waitForConnected(3000));

        const auto waitReceived = [&](int count) { QTRY_VERIFY(received().size() >= count); };

        // 半帧先到：定界未到，只应出"one"这一帧，"t"留在缓冲里。
        client.write(QByteArray("one\r\nt"));
        QVERIFY(client.waitForBytesWritten(1000));
        waitReceived(1);
        // 半帧留在缓冲里：此刻只有"one"一条（再等下去空闲超时会兜底把"t"也输出）。
        QCOMPARE(received().size(), 1);
        QCOMPARE(received().at(0).raw, QByteArray("one"));
        // 按定界收齐的是完整帧。
        QCOMPARE(received().at(0).framing, Traffic::Framing::Message);

        // 补齐第二帧：跨包到达的半帧在这里收齐。
        client.write(QByteArray("wo\r\n"));
        QVERIFY(client.waitForBytesWritten(1000));
        waitReceived(2);
        QCOMPARE(received().size(), 2);
        QCOMPARE(received().at(1).raw, QByteArray("two"));

        // 不带结束符的残留：空闲超时兜底输出，仍是流片段，并给出提示日志。
        client.write(QByteArray("tail"));
        QVERIFY(client.waitForBytesWritten(1000));
        waitReceived(3);
        const auto tail = received().at(2);
        QCOMPARE(tail.raw, QByteArray("tail"));
        QCOMPARE(tail.framing, Traffic::Framing::Stream);
        QVERIFY(tail.hasMoreFragments);
        QTRY_VERIFY([&messages] {
            for (const auto &params : messages) {
                const auto message = params.at(0).value<Traffic::Message>();
                if (message.direction == Traffic::Direction::Info) return true;
            }
            return false;
        }());

        client.disconnectFromHost();
        QTest::qWait(60);
        runtime.closeEndpoint(0);
    }

    // 空闲超时同样经传输事件进组帧器：连续写入合并成一条，静默满超时后才输出。
    void runtimeFramesByteStreamByIdleTimeout()
    {
        EndpointRuntime runtime;
        QSignalSpy states(&runtime, &EndpointRuntime::endpointStateChanged);
        QSignalSpy messages(&runtime, &EndpointRuntime::messageReceived);

        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();

        PortDraft draft = makeDraft(PortDraft::TcpServer,
            {{"server.address", "127.0.0.1"}, {"server.port", int(port)}, {"server.clients", 4}});
        ReceiveFraming framing;
        framing.mode = ReceiveFraming::IdleTimeout;
        framing.idleTimeoutMs = 180;
        framing.bufferThresholdBytes = 300;
        draft.framing = framing;

        runtime.openEndpoint(0, draft);
        QTRY_COMPARE(lastState(states), int(Transport::State::Open));

        const auto received = [&messages] {
            QList<Traffic::Message> rx;
            for (const auto &params : messages) {
                const auto message = params.at(0).value<Traffic::Message>();
                if (message.direction == Traffic::Direction::Receive) rx.append(message);
            }
            return rx;
        };

        QTcpSocket client;
        client.connectToHost(QStringLiteral("127.0.0.1"), port);
        QVERIFY(client.waitForConnected(3000));

        client.write(QByteArray("ab"));
        QVERIFY(client.waitForBytesWritten(1000));
        QTest::qWait(60);
        client.write(QByteArray("cd"));
        QVERIFY(client.waitForBytesWritten(1000));
        QTest::qWait(60);
        QCOMPARE(received().size(), 0);

        QTRY_COMPARE(received().size(), 1);
        QCOMPARE(received().at(0).raw, QByteArray("abcd"));
        QCOMPARE(received().at(0).framing, Traffic::Framing::Stream);
        QVERIFY(received().at(0).hasMoreFragments);

        client.disconnectFromHost();
        runtime.closeEndpoint(0);
    }

    // 连接中改组帧立刻生效。空闲模式先把 "tail" 留在缓冲里；改成结束符时
    // 这半包按旧规则吐出，后面的 "next" 才按新规则切。
    void runtimeAppliesFramingChangeWithoutReconnect()
    {
        EndpointRuntime runtime;
        QSignalSpy states(&runtime, &EndpointRuntime::endpointStateChanged);
        QSignalSpy messages(&runtime, &EndpointRuntime::messageReceived);

        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();

        PortDraft draft = makeDraft(PortDraft::TcpServer,
            {{"server.address", "127.0.0.1"}, {"server.port", int(port)}, {"server.clients", 4}});
        draft.framing.mode = ReceiveFraming::IdleTimeout;
        draft.framing.idleTimeoutMs = 5000;
        runtime.openEndpoint(0, draft);
        QTRY_COMPARE(lastState(states), int(Transport::State::Open));
        auto *transport = runtime.findChild<AbstractTransport *>();
        QVERIFY(transport);
        QSignalSpy chunks(transport, &AbstractTransport::dataReceived);

        const auto received = [&messages] {
            QList<Traffic::Message> rx;
            for (const auto &params : messages) {
                const auto message = params.at(0).value<Traffic::Message>();
                if (message.direction == Traffic::Direction::Receive) rx.append(message);
            }
            return rx;
        };

        QTcpSocket client;
        client.connectToHost(QStringLiteral("127.0.0.1"), port);
        QVERIFY(client.waitForConnected(3000));
        client.write(QByteArray("tail"));
        QVERIFY(client.waitForBytesWritten(1000));
        QTRY_COMPARE(chunks.size(), 1);
        QCOMPARE(received().size(), 0);
        ReceiveFraming framing;
        framing.mode = ReceiveFraming::Delimiter;
        framing.delimiter = QByteArray::fromHex("0D0A");
        framing.idleTimeoutMs = 5000;
        runtime.updateFraming(0, framing);
        QTRY_COMPARE(received().size(), 1);
        QCOMPARE(received().at(0).raw, QByteArray("tail"));

        client.write(QByteArray("next\r\nmore"));
        QVERIFY(client.waitForBytesWritten(1000));
        QTRY_COMPARE(received().size(), 2);
        QCOMPARE(received().at(1).raw, QByteArray("next"));
        QTest::qWait(40);
        QCOMPARE(received().size(), 2);

        client.disconnectFromHost();
        runtime.closeEndpoint(0);
    }

    // 空结束符不是合法规则。送进来也不能拿空序列去查找，否则组帧循环不前进。
    void runtimeDropsAnEmptyDelimiter()
    {
        EndpointRuntime runtime;
        QSignalSpy states(&runtime, &EndpointRuntime::endpointStateChanged);
        QSignalSpy messages(&runtime, &EndpointRuntime::messageReceived);

        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();

        runtime.openEndpoint(0, makeDraft(PortDraft::TcpServer,
            {{"server.address", "127.0.0.1"}, {"server.port", int(port)}, {"server.clients", 4}}));
        QTRY_COMPARE(lastState(states), int(Transport::State::Open));

        ReceiveFraming broken;
        broken.mode = ReceiveFraming::Delimiter;
        runtime.updateFraming(0, broken);

        QTcpSocket client;
        client.connectToHost(QStringLiteral("127.0.0.1"), port);
        QVERIFY(client.waitForConnected(3000));
        client.write(QByteArray("kept"));
        QVERIFY(client.waitForBytesWritten(1000));
        // 拒绝后仍是直通：收到即完整输出。空结束符若被换上，这段会卡住或变成流片段。
        QTRY_VERIFY(std::any_of(messages.cbegin(), messages.cend(), [](const QList<QVariant> &params) {
            const auto message = params.at(0).value<Traffic::Message>();
            return message.direction == Traffic::Direction::Receive
                && message.raw == QByteArray("kept")
                && message.framing == Traffic::Framing::Message;
        }));

        client.disconnectFromHost();
        runtime.closeEndpoint(0);
    }

    void factoryCreatesEveryKind()
    {
        for (int kind = PortDraft::Serial; kind <= PortDraft::Udp; ++kind) {
            auto *transport = TransportFactory::create(kind, 0, this);
            QVERIFY2(transport, qPrintable(QString::number(kind)));
            delete transport;
        }
    }

    void tcpClientAndServerExchange()
    {
        TcpServerTransport server(0);
        QSignalSpy serverState(&server, &AbstractTransport::stateChanged);
        QSignalSpy serverPeers(&server, &AbstractTransport::peerListChanged);
        QSignalSpy serverData(&server, &AbstractTransport::dataReceived);
        QSignalSpy serverErrors(&server, &AbstractTransport::errorOccurred);

        const int port = findFreePort([&server, &serverState](int candidate) {
            server.start(makeDraft(PortDraft::TcpServer,
                {{"server.address", "127.0.0.1"}, {"server.port", candidate}, {"server.clients", 8}}), 1);
            const bool open = lastState(serverState) == int(Transport::State::Open);
            if (!open) serverState.clear(); // 成功的那次记录保留，供后续断言使用
            return open;
        });
        QVERIFY2(port != 0, "没有可用的回环监听端口");

        TcpClientTransport client(1);
        QSignalSpy clientState(&client, &AbstractTransport::stateChanged);
        QSignalSpy clientData(&client, &AbstractTransport::dataReceived);
        QSignalSpy connected(&server, &AbstractTransport::peerConnected);
        client.start(makeDraft(PortDraft::TcpClient,
            {{"client.host", "127.0.0.1"}, {"client.port", port}, {"client.timeout", 5000}}), 1);
        QTRY_COMPARE(lastState(clientState), int(Transport::State::Open));
        QTRY_COMPARE(peerCount(serverPeers), 1);
        const auto peerId = firstPeerId(serverPeers);
        QVERIFY(!peerId.isNull());

        // 客户端 → 服务器：来源携带 peer 身份与地址
        QCOMPARE(client.write("ping", QUuid()), qint64(4));
        QTRY_VERIFY(!serverData.isEmpty());
        QCOMPARE(serverData.last().at(2).toByteArray(), QByteArray("ping"));
        const auto rxSource = serverData.last().at(1).value<Traffic::Source>();
        QCOMPARE(rxSource.peerId, peerId);
        QVERIFY(rxSource.peerAddress.startsWith(u"127.0.0.1:"_s));

        // 服务器 → 指定 peer
        QCOMPARE(server.write("pong", peerId), qint64(4));
        QTRY_VERIFY(!clientData.isEmpty());
        QCOMPARE(clientData.last().at(2).toByteArray(), QByteArray("pong"));
        const auto clientSource = clientData.last().at(1).value<Traffic::Source>();
        QVERIFY(clientSource.peerAddress.endsWith(QStringLiteral(":%1").arg(port)));

        // 服务器 → 全部客户端（peerId 为空的广播语义）。返回值是接受最少的那个客户端。
        QCOMPARE(server.write("all", QUuid()), qint64(3));
        QTRY_VERIFY(clientData.count() >= 2);
        bool broadcastSeen = false;
        for (int i = 0; i < clientData.count(); ++i)
            broadcastSeen |= clientData.at(i).at(2).toByteArray() == QByteArray("all");
        QVERIFY(broadcastSeen);

        // 服务端握手完成时发出接入事件，地址进数据区。监听成功本身不算对端已连接。
        QTRY_COMPARE(connected.count(), 1);
        QCOMPARE(connected.at(0).at(1).toUuid(), peerId);
        QVERIFY(connected.at(0).at(2).toString().startsWith(QStringLiteral("127.0.0.1:")));
        // 客户端连上时的已打开说明带对端地址，数据区状态行直接能读出连的是谁。
        bool clientOpenHasPeer = false;
        for (int i = 0; i < clientState.count(); ++i) {
            if (clientState.at(i).at(1).toInt() != int(Transport::State::Open)) continue;
            clientOpenHasPeer = clientState.at(i).at(2).toString()
                .contains(QStringLiteral("127.0.0.1:%1").arg(port));
        }
        QVERIFY(clientOpenHasPeer);

        // 未知 peer 的定向写是失败而不是静默广播
        QCOMPARE(server.write("ghost", QUuid::createUuid()), qint64(-1));

        client.stop();
        QTRY_COMPARE(lastState(clientState), int(Transport::State::Closed));
        QTRY_COMPARE(peerCount(serverPeers), 0);
        server.stop();
        QCOMPARE(lastState(serverState), int(Transport::State::Open)); // stop 不发状态事件，代次由运行时管理
    }

    void tcpClientReportsConnectionRefused()
    {
        TcpClientTransport client(0);
        QSignalSpy state(&client, &AbstractTransport::stateChanged);
        QSignalSpy errors(&client, &AbstractTransport::errorOccurred);
        // 127.0.0.1:1 几乎必然没有监听；拒绝连接要变成显式错误而不是永远 Opening。
        client.start(makeDraft(PortDraft::TcpClient,
            {{"client.host", "127.0.0.1"}, {"client.port", 1}, {"client.timeout", 5000}}), 1);
        QTRY_COMPARE(lastState(state), int(Transport::State::Closed));
        QVERIFY(errors.count() >= 1);
        QCOMPARE(client.write("x", QUuid()), qint64(-1));
    }

    void tcpServerEnforcesClientLimit()
    {
        TcpServerTransport server(0);
        QSignalSpy serverState(&server, &AbstractTransport::stateChanged);
        QSignalSpy serverPeers(&server, &AbstractTransport::peerListChanged);
        QSignalSpy serverErrors(&server, &AbstractTransport::errorOccurred);
        const int port = findFreePort([&server, &serverState](int candidate) {
            server.start(makeDraft(PortDraft::TcpServer,
                {{"server.address", "127.0.0.1"}, {"server.port", candidate}, {"server.clients", 1}}), 1);
            const bool open = lastState(serverState) == int(Transport::State::Open);
            if (!open) serverState.clear();
            return open;
        });
        QVERIFY(port != 0);

        QTcpSocket first;
        first.connectToHost(QHostAddress::LocalHost, quint16(port));
        QTRY_COMPARE(peerCount(serverPeers), 1);

        QTcpSocket second;
        second.connectToHost(QHostAddress::LocalHost, quint16(port));
        // 超限连接被立即拒绝：服务器仍只有 1 个 peer，第二个客户端被断开。
        QTRY_VERIFY(serverErrors.count() >= 1);
        QTRY_COMPARE(peerCount(serverPeers), 1);
        QTRY_COMPARE(second.state(), QAbstractSocket::UnconnectedState);
        first.disconnectFromHost();
        QTRY_COMPARE(peerCount(serverPeers), 0);
        server.stop();
    }

    void udpPreservesDatagramBoundaryAndSource()
    {
        UdpTransport receiver(0);
        QSignalSpy receiverState(&receiver, &AbstractTransport::stateChanged);
        const int port = findFreePort([&receiver, &receiverState](int candidate) {
            receiver.start(makeDraft(PortDraft::Udp,
                {{"udp.address", "127.0.0.1"}, {"udp.port", candidate},
                 {"udp.target", ""}, {"udp.targetPort", 0}}), 1);
            const bool open = lastState(receiverState) == int(Transport::State::Open);
            if (!open) receiverState.clear();
            return open;
        });
        QVERIFY2(port != 0, "没有可用的回环绑定端口");

        UdpTransport sender(1);
        QSignalSpy senderState(&sender, &AbstractTransport::stateChanged);
        sender.start(makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
             {"udp.target", "127.0.0.1"}, {"udp.targetPort", port}}), 1);
        QCOMPARE(lastState(senderState), int(Transport::State::Open));

        QSignalSpy received(&receiver, &AbstractTransport::dataReceived);
        // 三个数据报（含一个零长）：边界逐一保留，不合并、不拆分。
        QCOMPARE(sender.write(QByteArray("ab"), QUuid()), qint64(2));
        QCOMPARE(sender.write(QByteArray(), QUuid()), qint64(0));
        QCOMPARE(sender.write(QByteArray("cde"), QUuid()), qint64(3));
        QTRY_COMPARE(received.count() >= 3, true);
        QCOMPARE(received.count(), 3);
        const QList<QByteArray> expected = {QByteArray("ab"), QByteArray(), QByteArray("cde")};
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(received.at(i).at(2).toByteArray(), expected.at(i));
            QCOMPARE(received.at(i).at(3).toInt(), int(Traffic::Framing::Datagram));
            const auto source = received.at(i).at(1).value<Traffic::Source>();
            QVERIFY(source.peerAddress.startsWith(u"127.0.0.1:"_s));
        }
        sender.stop();
        receiver.stop();
    }

    void runtimeUdpPassthroughPreservesEmptyDatagrams()
    {
        QList<Traffic::Message> received;
        EndpointRuntime runtime;
        connect(&runtime, &EndpointRuntime::messageReceived, this,
            [&received](const Traffic::Message &message) {
                if (message.direction == Traffic::Direction::Receive) received.append(message);
            });
        runtime.openEndpoint(0, makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0}}));
        auto *socket = runtime.findChild<QUdpSocket *>();
        QVERIFY(socket && socket->state() == QAbstractSocket::BoundState);
        QSignalSpy chunks(runtime.findChild<AbstractTransport *>(), &AbstractTransport::dataReceived);
        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        const auto address = QStringLiteral("127.0.0.1:%1").arg(sender.localPort());
        const QList<QByteArray> datagrams = {"one\r\ntwo", QByteArray(), "tail"};
        for (int i = 0; i < datagrams.size(); ++i) {
            QCOMPARE(sender.writeDatagram(datagrams[i], QHostAddress::LocalHost, socket->localPort()),
                qint64(datagrams[i].size()));
            QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), i + 1, 5000);
            QCOMPARE(received.size(), i + 1);
            QCOMPARE(received[i].raw, datagrams[i]);
            QCOMPARE(received[i].rawLength, qint64(datagrams[i].size()));
            QCOMPARE(received[i].framing, Traffic::Framing::Datagram);
            QCOMPARE(received[i].source.peerAddress, address);
            QVERIFY(!received[i].hasMoreFragments);
        }
        runtime.closeEndpoint(0);
        QCOMPARE(received.size(), datagrams.size());
    }

    void runtimeUdpDelimiterSeparatesInterleavedSources_data()
    {
        QTest::addColumn<bool>("includeDelimiter");
        QTest::addColumn<bool>("samePortDifferentIp");
        QTest::newRow("strip-delimiter") << false << false;
        QTest::newRow("keep-delimiter") << true << false;
        QTest::newRow("same-port-different-ip") << false << true;
    }

    void runtimeUdpDelimiterSeparatesInterleavedSources()
    {
        QFETCH(bool, includeDelimiter);
        QFETCH(bool, samePortDifferentIp);
        QList<Traffic::Message> received;
        EndpointRuntime runtime;
        auto draft = makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0}});
        draft.framing.mode = ReceiveFraming::Delimiter;
        draft.framing.delimiter = "\r\n";
        draft.framing.includeDelimiter = includeDelimiter;
        draft.framing.idleTimeoutMs = 10000;
        connect(&runtime, &EndpointRuntime::messageReceived, this,
            [&received](const Traffic::Message &message) {
                if (message.direction == Traffic::Direction::Receive) received.append(message);
            });
        runtime.openEndpoint(0, draft);
        auto *socket = runtime.findChild<QUdpSocket *>();
        QVERIFY(socket && socket->state() == QAbstractSocket::BoundState);
        const auto port = socket->localPort();
        QSignalSpy chunks(runtime.findChild<AbstractTransport *>(), &AbstractTransport::dataReceived);
        QUdpSocket first, second;
        QVERIFY(first.bind(QHostAddress::LocalHost, 0));
        const QHostAddress secondIp(samePortDifferentIp ? QStringLiteral("127.0.0.2")
                                                       : QStringLiteral("127.0.0.1"));
        QVERIFY2(second.bind(secondIp, samePortDifferentIp ? first.localPort() : quint16(0)),
            qPrintable(second.errorString()));
        if (samePortDifferentIp) QCOMPARE(first.localPort(), second.localPort());
        else QVERIFY(first.localPort() != second.localPort());
        const auto firstAddress = QStringLiteral("127.0.0.1:%1").arg(first.localPort());
        const auto secondAddress = QStringLiteral("%1:%2").arg(secondIp.toString()).arg(second.localPort());

        QCOMPARE(first.writeDatagram("first\r", QHostAddress::LocalHost, port), qint64(6));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 1, 5000);
        QVERIFY(received.isEmpty()); // 已收到原始报，不是靠等待没有消息来判定缓存成功。
        QCOMPARE(second.writeDatagram("second\r", QHostAddress::LocalHost, port), qint64(7));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 2, 5000);
        QVERIFY(received.isEmpty());
        QCOMPARE(first.writeDatagram("\nnext\r\n\r\nfirst-tail", QHostAddress::LocalHost, port), qint64(19));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 3, 5000);
        QCOMPARE(received.size(), 3);
        const QList<QByteArray> firstFrames = {"first", "next", QByteArray()};
        for (int i = 0; i < firstFrames.size(); ++i) {
            QCOMPARE(received[i].raw, firstFrames[i] + (includeDelimiter ? QByteArray("\r\n") : QByteArray()));
            QCOMPARE(received[i].source.peerAddress, firstAddress);
            QCOMPARE(received[i].framing, Traffic::Framing::Message);
            QVERIFY(!received[i].hasMoreFragments);
        }
        QCOMPARE(second.writeDatagram("\nsecond-tail", QHostAddress::LocalHost, port), qint64(12));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 4, 5000);
        QCOMPARE(received.size(), 4);
        QCOMPARE(received[3].raw, includeDelimiter ? QByteArray("second\r\n") : QByteArray("second"));
        QCOMPARE(received[3].source.peerAddress, secondAddress);
        QCOMPARE(received[3].framing, Traffic::Framing::Message);

        // 关闭必须排空每个来源；QHash 的遍历顺序不构成消息顺序约定。
        runtime.closeEndpoint(0);
        QCOMPARE(received.size(), 6);
        QMap<QString, QByteArray> tails;
        for (int i = 4; i < received.size(); ++i) {
            QCOMPARE(received[i].framing, Traffic::Framing::Stream);
            QVERIFY(received[i].hasMoreFragments);
            tails.insert(received[i].source.peerAddress, received[i].raw);
        }
        QCOMPARE(tails.size(), 2);
        QCOMPARE(tails.value(firstAddress), QByteArray("first-tail"));
        QCOMPARE(tails.value(secondAddress), QByteArray("second-tail"));
        runtime.closeEndpoint(0);
        QCOMPARE(received.size(), 6); // 重复关闭不得重复吐尾帧。
    }

    void runtimeUdpIdleTimersArePerSource_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("idle-mode") << int(ReceiveFraming::IdleTimeout);
        QTest::newRow("delimiter-fallback") << int(ReceiveFraming::Delimiter);
    }

    void runtimeUdpIdleTimersArePerSource()
    {
        QFETCH(int, mode);
        QList<Traffic::Message> received;
        EndpointRuntime runtime;
        auto draft = makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0}});
        draft.framing.mode = ReceiveFraming::Mode(mode);
        draft.framing.delimiter = "\r\n";
        draft.framing.idleTimeoutMs = 500;
        draft.framing.bufferThresholdBytes = 100000;
        connect(&runtime, &EndpointRuntime::messageReceived, this,
            [&received](const Traffic::Message &message) {
                if (message.direction == Traffic::Direction::Receive) received.append(message);
            });
        runtime.openEndpoint(0, draft);
        auto *socket = runtime.findChild<QUdpSocket *>();
        QVERIFY(socket && socket->state() == QAbstractSocket::BoundState);
        const auto port = socket->localPort();
        QSignalSpy chunks(runtime.findChild<AbstractTransport *>(), &AbstractTransport::dataReceived);
        QUdpSocket first, second;
        QVERIFY(first.bind(QHostAddress::LocalHost, 0));
        QVERIFY(second.bind(QHostAddress::LocalHost, 0));
        const auto firstAddress = QStringLiteral("127.0.0.1:%1").arg(first.localPort());
        const auto secondAddress = QStringLiteral("127.0.0.1:%1").arg(second.localPort());
        QCOMPARE(first.writeDatagram("quiet", QHostAddress::LocalHost, port), qint64(5));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 1, 5000);
        QVERIFY(received.isEmpty());
        QCOMPARE(second.writeDatagram("busy", QHostAddress::LocalHost, port), qint64(4));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 2, 5000);
        QVERIFY(received.isEmpty());

        // B 持续有输入：不能延后 A 的空闲输出，也不能让 B 在自己的期限前输出。
        QTimer heartbeat;
        heartbeat.setInterval(40);
        int sent = 0;
        bool writeFailed = false;
        connect(&heartbeat, &QTimer::timeout, this, [&] {
            if (second.writeDatagram("x", QHostAddress::LocalHost, port) != 1) writeFailed = true;
            else ++sent;
        });
        heartbeat.start();
        QTRY_COMPARE_WITH_TIMEOUT(received.size(), 1, 5000);
        heartbeat.stop();
        QVERIFY(!writeFailed);
        QVERIFY(sent > 0);
        QCOMPARE(received[0].raw, QByteArray("quiet"));
        QCOMPARE(received[0].source.peerAddress, firstAddress);
        QCOMPARE(received[0].framing, Traffic::Framing::Stream);
        QVERIFY(received[0].hasMoreFragments);
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), sent + 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(received.size(), 2, 5000);
        QCOMPARE(received[1].raw, QByteArray("busy") + QByteArray(sent, 'x'));
        QCOMPARE(received[1].source.peerAddress, secondAddress);
        QCOMPARE(received[1].framing, Traffic::Framing::Stream);
        QVERIFY(received[1].hasMoreFragments);
        runtime.closeEndpoint(0);
        QCOMPARE(received.size(), 2);
    }

    void runtimeUdpThresholdIsPerSource_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("idle-mode") << int(ReceiveFraming::IdleTimeout);
        QTest::newRow("delimiter-mode") << int(ReceiveFraming::Delimiter);
    }

    void runtimeUdpThresholdIsPerSource()
    {
        QFETCH(int, mode);
        QList<Traffic::Message> received;
        EndpointRuntime runtime;
        auto draft = makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0}});
        draft.framing.mode = ReceiveFraming::Mode(mode);
        draft.framing.delimiter = "\r\n";
        draft.framing.idleTimeoutMs = 10000;
        draft.framing.bufferThresholdBytes = 6;
        connect(&runtime, &EndpointRuntime::messageReceived, this,
            [&received](const Traffic::Message &message) {
                if (message.direction == Traffic::Direction::Receive) received.append(message);
            });
        runtime.openEndpoint(0, draft);
        auto *socket = runtime.findChild<QUdpSocket *>();
        QVERIFY(socket && socket->state() == QAbstractSocket::BoundState);
        const auto port = socket->localPort();
        QSignalSpy chunks(runtime.findChild<AbstractTransport *>(), &AbstractTransport::dataReceived);
        QUdpSocket first, second;
        QVERIFY(first.bind(QHostAddress::LocalHost, 0));
        QVERIFY(second.bind(QHostAddress::LocalHost, 0));
        QCOMPARE(first.writeDatagram("abc", QHostAddress::LocalHost, port), qint64(3));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 1, 5000);
        QCOMPARE(second.writeDatagram("123", QHostAddress::LocalHost, port), qint64(3));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 2, 5000);
        QVERIFY(received.isEmpty()); // 合计已达 6 字节，但每个来源都尚未达到阈值。
        QCOMPARE(first.writeDatagram("def", QHostAddress::LocalHost, port), qint64(3));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 3, 5000);
        QCOMPARE(received.size(), 1); // 阈值应立即输出，不等待 10 秒 idle 兜底。
        QCOMPARE(received[0].raw, QByteArray("abcdef"));
        QCOMPARE(received[0].source.peerAddress, QStringLiteral("127.0.0.1:%1").arg(first.localPort()));
        QCOMPARE(received[0].framing, Traffic::Framing::Stream);
        QVERIFY(received[0].hasMoreFragments);
        QCOMPARE(second.writeDatagram("4567", QHostAddress::LocalHost, port), qint64(4));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 4, 5000);
        QCOMPARE(received.size(), 2);
        QCOMPARE(received[1].raw, QByteArray("1234567"));
        QCOMPARE(received[1].source.peerAddress, QStringLiteral("127.0.0.1:%1").arg(second.localPort()));
        QCOMPARE(received[1].framing, Traffic::Framing::Stream);
        QVERIFY(received[1].hasMoreFragments);
        runtime.closeEndpoint(0);
        QCOMPARE(received.size(), 2);
    }

    void runtimeUdpFramingHotUpdateKeepsSocketAndFlushes()
    {
        QList<Traffic::Message> received;
        EndpointRuntime runtime;
        QSignalSpy states(&runtime, &EndpointRuntime::endpointStateChanged);
        connect(&runtime, &EndpointRuntime::messageReceived, this,
            [&received](const Traffic::Message &message) {
                if (message.direction == Traffic::Direction::Receive) received.append(message);
            });
        runtime.openEndpoint(0, makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0}}));
        auto *transport = runtime.findChild<AbstractTransport *>();
        auto *socket = runtime.findChild<QUdpSocket *>();
        QVERIFY(transport && socket && socket->state() == QAbstractSocket::BoundState);
        const auto descriptor = socket->socketDescriptor();
        const auto port = socket->localPort();
        const auto stateCount = states.size();
        QSignalSpy chunks(transport, &AbstractTransport::dataReceived);
        QUdpSocket first, second;
        QVERIFY(first.bind(QHostAddress::LocalHost, 0));
        QVERIFY(second.bind(QHostAddress::LocalHost, 0));
        const auto firstAddress = QStringLiteral("127.0.0.1:%1").arg(first.localPort());
        const auto secondAddress = QStringLiteral("127.0.0.1:%1").arg(second.localPort());
        QCOMPARE(first.writeDatagram("native\r\n", QHostAddress::LocalHost, port), qint64(8));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 1, 5000);
        QCOMPARE(received.size(), 1);
        QCOMPARE(received[0].raw, QByteArray("native\r\n"));
        QCOMPARE(received[0].framing, Traffic::Framing::Datagram);
        const auto generation = received[0].source.generation;

        ReceiveFraming framing;
        framing.mode = ReceiveFraming::Delimiter;
        framing.delimiter = "\r\n";
        framing.idleTimeoutMs = 10000;
        runtime.updateFraming(0, framing);
        QCOMPARE(first.writeDatagram("one\r\nold-a", QHostAddress::LocalHost, port), qint64(10));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 2, 5000);
        QCOMPARE(received.size(), 2);
        QCOMPARE(received[1].raw, QByteArray("one"));
        QCOMPARE(received[1].framing, Traffic::Framing::Message);
        QCOMPARE(second.writeDatagram("old-b", QHostAddress::LocalHost, port), qint64(5));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 3, 5000);
        QCOMPARE(received.size(), 2);

        framing.mode = ReceiveFraming::IdleTimeout;
        runtime.updateFraming(0, framing);
        QCOMPARE(received.size(), 4);
        QMap<QString, QByteArray> flushed;
        for (int i = 2; i < 4; ++i) {
            QCOMPARE(received[i].framing, Traffic::Framing::Stream);
            QVERIFY(received[i].hasMoreFragments);
            flushed.insert(received[i].source.peerAddress, received[i].raw);
        }
        QCOMPARE(flushed.value(firstAddress), QByteArray("old-a"));
        QCOMPARE(flushed.value(secondAddress), QByteArray("old-b"));
        QCOMPARE(first.writeDatagram("idle-tail", QHostAddress::LocalHost, port), qint64(9));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 4, 5000);
        QCOMPARE(received.size(), 4);
        runtime.updateFraming(0, ReceiveFraming());
        QCOMPARE(received.size(), 5);
        QCOMPARE(received[4].raw, QByteArray("idle-tail"));
        QCOMPARE(received[4].framing, Traffic::Framing::Stream);
        QCOMPARE(received[4].source.peerAddress, firstAddress);
        QVERIFY(received[4].hasMoreFragments);
        QCOMPARE(first.writeDatagram(QByteArray(), QHostAddress::LocalHost, port), qint64(0));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 5, 5000);
        QCOMPARE(received.size(), 6);
        QCOMPARE(received[5].raw, QByteArray());
        QCOMPARE(received[5].framing, Traffic::Framing::Datagram);
        QCOMPARE(first.writeDatagram("native-again\r\n", QHostAddress::LocalHost, port), qint64(14));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 6, 5000);
        QCOMPARE(received.size(), 7);
        QCOMPARE(received[6].raw, QByteArray("native-again\r\n"));
        QCOMPARE(received[6].framing, Traffic::Framing::Datagram);

        framing.mode = ReceiveFraming::Delimiter;
        runtime.updateFraming(0, framing);
        QCOMPARE(first.writeDatagram("fresh\r\n", QHostAddress::LocalHost, port), qint64(7));
        QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), 7, 5000);
        QCOMPARE(received.size(), 8);
        QCOMPARE(received[7].raw, QByteArray("fresh"));
        QCOMPARE(received[7].framing, Traffic::Framing::Message);
        QCOMPARE(runtime.findChild<AbstractTransport *>(), transport);
        QCOMPARE(runtime.findChild<QUdpSocket *>(), socket);
        QCOMPARE(socket->socketDescriptor(), descriptor);
        QCOMPARE(socket->localPort(), port);
        QCOMPARE(socket->state(), QAbstractSocket::BoundState);
        QCOMPARE(states.size(), stateCount);
        for (const auto &message : received) QCOMPARE(message.source.generation, generation);
        runtime.closeEndpoint(0);
        QCOMPARE(received.size(), 8);
    }

    void runtimeUdpFramingPreservesRawInputAndBridgeDatagrams()
    {
        QList<Traffic::Message> received;
        EndpointRuntime runtime;
        connect(&runtime, &EndpointRuntime::messageReceived, this,
            [&received](const Traffic::Message &message) {
                if (message.direction == Traffic::Direction::Receive) received.append(message);
            });
        QSignalSpy raw(&runtime, &EndpointRuntime::rawDataReceived);
        auto draft = makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0}});
        draft.framing.mode = ReceiveFraming::Delimiter;
        draft.framing.delimiter = "\r\n";
        draft.framing.idleTimeoutMs = 10000;
        runtime.openEndpoint(0, draft);
        auto *input = runtime.findChild<QUdpSocket *>();
        auto *transport = runtime.findChild<AbstractTransport *>();
        QVERIFY(input && input->state() == QAbstractSocket::BoundState);
        const auto port = input->localPort();
        QSignalSpy chunks(transport, &AbstractTransport::dataReceived);
        UdpTransport sink(0);
        sink.start(makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0}}), 1);
        auto *sinkSocket = sink.findChild<QUdpSocket *>();
        QVERIFY(sinkSocket && sinkSocket->state() == QAbstractSocket::BoundState);
        QSignalSpy forwarded(&sink, &AbstractTransport::dataReceived);
        runtime.openEndpoint(1, makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
             {"udp.target", "127.0.0.1"}, {"udp.targetPort", sinkSocket->localPort()}}));
        BridgeLink link;
        link.enabled = true;
        link.direction = int(BridgeDirection::AtoB);
        runtime.setBridge(link);
        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        const QList<QByteArray> datagrams = {"one\r", "\ntwo\r\nleft", "over\r\n"};
        const QList<int> frameCounts = {0, 2, 3};
        for (int i = 0; i < datagrams.size(); ++i) {
            QCOMPARE(sender.writeDatagram(datagrams[i], QHostAddress::LocalHost, port), qint64(datagrams[i].size()));
            QTRY_COMPARE_WITH_TIMEOUT(chunks.size(), i + 1, 5000);
            QCOMPARE(raw.size(), i + 1);
            QCOMPARE(raw[i][0].toInt(), 0);
            QCOMPARE(raw[i][1].value<Traffic::Source>().peerAddress,
                QStringLiteral("127.0.0.1:%1").arg(sender.localPort()));
            QCOMPARE(raw[i][2].toByteArray(), datagrams[i]);
            QCOMPARE(received.size(), frameCounts[i]);
            QTRY_COMPARE_WITH_TIMEOUT(forwarded.size(), i + 1, 5000);
            QCOMPARE(forwarded[i][2].toByteArray(), datagrams[i]);
            QCOMPARE(forwarded[i][3].toInt(), int(Traffic::Framing::Datagram));
        }
        QCOMPARE(received[0].raw, QByteArray("one"));
        QCOMPARE(received[1].raw, QByteArray("two"));
        QCOMPARE(received[2].raw, QByteArray("leftover"));
        for (const auto &message : received) QCOMPARE(message.framing, Traffic::Framing::Message);
        runtime.shutdown();
        QCOMPARE(received.size(), 3);
        QCOMPARE(raw.size(), datagrams.size());
        sink.stop();
    }

    void udpWithoutTargetRejectsSend()
    {
        UdpTransport receiver(0);
        QSignalSpy state(&receiver, &AbstractTransport::stateChanged);
        receiver.start(makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
             {"udp.target", ""}, {"udp.targetPort", 0}}), 1);
        QCOMPARE(lastState(state), int(Transport::State::Open));
        const auto detail = state.last().at(2).toString();
        QVERIFY(detail.contains(QStringLiteral("仅接收")));
        QVERIFY(detail.contains(QStringLiteral("不会建立连接或自动回发")));
        // 仅接收的端点没有发送目标：写操作显式失败，不悄悄挑选目的地。
        QCOMPARE(receiver.write("x", QUuid()), qint64(-1));
        receiver.stop();
    }

    void serialRejectsInvalidPortName()
    {
        SerialTransport transport(0);
        QSignalSpy state(&transport, &AbstractTransport::stateChanged);
        QSignalSpy errors(&transport, &AbstractTransport::errorOccurred);
        transport.start(makeDraft(PortDraft::Serial,
            {{"serial.port", "__not_a_serial_port__"}, {"serial.baud", "115200"},
             {"serial.bits", "8"}, {"serial.parity", "None"}, {"serial.stop", "1"},
             {"serial.flow", "None"}}), 1);
        QTRY_COMPARE(lastState(state), int(Transport::State::Closed));
        QVERIFY(errors.count() >= 1);
        QCOMPARE(transport.write("x", QUuid()), qint64(-1));
    }

    void endpointRuntimeAssemblesAndRoutesMessages()
    {
        EndpointRuntime runtime;
        QSignalSpy states(&runtime, &EndpointRuntime::endpointStateChanged);
        QSignalSpy messages(&runtime, &EndpointRuntime::messageReceived);
        QSignalSpy peers(&runtime, &EndpointRuntime::endpointPeersChanged);

        // A 端：UDP 仅接收；逐个候选端口直到打开成功。
        int port = 0;
        for (int candidate = PortScanBegin; candidate <= PortScanEnd && port == 0; ++candidate) {
            runtime.openEndpoint(0, makeDraft(PortDraft::Udp,
                {{"udp.address", "127.0.0.1"}, {"udp.port", candidate},
                 {"udp.target", ""}, {"udp.targetPort", 0}}));
            if (lastState(states) == int(Transport::State::Open)) {
                port = candidate;
            } else {
                runtime.closeEndpoint(0);
                states.clear();
            }
        }
        QVERIFY(port != 0);

        // B 端：UDP 发往 A。
        runtime.openEndpoint(1, makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
             {"udp.target", "127.0.0.1"}, {"udp.targetPort", port}}));
        QCOMPARE(lastState(states), int(Transport::State::Open));

        // 手动发送：B 出现 TX LocalCompleted 消息，A 收到 RX 消息。
        runtime.sendPayload(1, "ping", int(Traffic::Origin::Manual));
        const auto findMessage = [&messages](Traffic::Direction dir, int idx, const QByteArray &raw) {
            for (const auto &params : messages) {
                const auto m = params.at(0).value<Traffic::Message>();
                if (m.direction == dir && m.source.endpointIndex == idx && m.raw == raw)
                    return m;
            }
            return Traffic::Message{};
        };
        // TX：已打开且目标是明确地址 → 本地写出完成；身份与单调时钟已打戳。
        // 发送目标写在来源地址上，日志才能看出这一笔发给了谁。
        const auto tx = findMessage(Traffic::Direction::Transmit, 1, "ping");
        QCOMPARE(tx.state, Traffic::State::LocalCompleted);
        QVERIFY(tx.id != 0 && tx.monotonicNs > 0);
        QCOMPARE(tx.source.peerAddress, QStringLiteral("127.0.0.1:%1").arg(port));
        // RX：轮询重查 spy（QTRY 的表达式每次迭代都重新执行 findMessage）。
        QTRY_VERIFY(findMessage(Traffic::Direction::Receive, 0, "ping").id != 0);
        const auto rx = findMessage(Traffic::Direction::Receive, 0, "ping");
        QCOMPARE(rx.framing, Traffic::Framing::Datagram);
        QVERIFY(!rx.source.peerAddress.isEmpty());

        // UDP 没有连接事件：首次收到每个来源时，运行时要把来源传给界面，
        // 同时留下不会自动修改发送目标的明确提示。
        const auto observedPeers = [&peers] {
            QStringList result;
            for (const auto &params : peers)
                result.append(params.at(1).toStringList());
            return result;
        };
        QTRY_VERIFY(observedPeers().contains(rx.source.peerAddress));
        const auto peerNotices = [&messages] {
            int count = 0;
            for (const auto &params : messages) {
                const auto message = params.at(0).value<Traffic::Message>();
                if (message.direction == Traffic::Direction::Info && message.source.endpointIndex == 0
                    && message.statusText.contains(QStringLiteral("对端"))
                    && message.statusText.contains(QStringLiteral("已连接")))
                    ++count;
            }
            return count;
        };
        QCOMPARE(peerNotices(), 1);
        runtime.sendPayload(1, "ping-again", int(Traffic::Origin::Manual));
        QTRY_VERIFY(findMessage(Traffic::Direction::Receive, 0, "ping-again").id != 0);
        QCOMPARE(peerNotices(), 1); // 同一来源不重复刷状态日志

        // 仅接收端点（已打开但没有发送目标）发送：显式 Failed，不静默丢弃。
        const int baseline = messages.count();
        runtime.sendPayload(0, "nope", int(Traffic::Origin::Manual));
        QCOMPARE(messages.count(), baseline + 2);
        const auto failed = messages.at(baseline).at(0).value<Traffic::Message>();
        QCOMPARE(failed.state, Traffic::State::Failed);
        QCOMPARE(failed.direction, Traffic::Direction::Transmit);
        const auto dropped = messages.at(baseline + 1).at(0).value<Traffic::Message>();
        QCOMPARE(dropped.direction, Traffic::Direction::Error);
        QCOMPARE(dropped.rawLength, qint64(4));
        QVERIFY(dropped.statusText.contains(QStringLiteral("丢弃未写出数据 4 字节")));

        runtime.closeEndpoint(1);
        runtime.closeEndpoint(0);
        QTRY_COMPARE(lastState(states), int(Transport::State::Closed));
        // 关闭时的空列表广播用于复位界面显示。
        QVERIFY(!peers.isEmpty());
        QVERIFY(peers.last().at(1).toStringList().isEmpty());
    }

    void bridgeCopiesReceivedBytesToTheOtherEndpoint()
    {
        EndpointRuntime runtime;
        QSignalSpy states(&runtime, &EndpointRuntime::endpointStateChanged);
        QSignalSpy messages(&runtime, &EndpointRuntime::messageReceived);

        int portA = 0;
        for (int candidate = PortScanBegin; candidate <= PortScanEnd && portA == 0; ++candidate) {
            runtime.openEndpoint(0, makeDraft(PortDraft::Udp,
                {{"udp.address", "127.0.0.1"}, {"udp.port", candidate},
                 {"udp.target", ""}, {"udp.targetPort", 0}}));
            if (lastState(states) == int(Transport::State::Open)) portA = candidate;
            else { runtime.closeEndpoint(0); states.clear(); }
        }
        QVERIFY(portA != 0);

        UdpTransport sinkB(0);
        QSignalSpy sinkBState(&sinkB, &AbstractTransport::stateChanged);
        QSignalSpy sinkBData(&sinkB, &AbstractTransport::dataReceived);
        const int sinkPort = findFreePort([&sinkB, &sinkBState](int candidate) {
            sinkB.start(makeDraft(PortDraft::Udp,
                {{"udp.address", "127.0.0.1"}, {"udp.port", candidate},
                 {"udp.target", ""}, {"udp.targetPort", 0}}), 1);
            const bool open = lastState(sinkBState) == int(Transport::State::Open);
            if (!open) sinkBState.clear();
            return open;
        });
        QVERIFY(sinkPort != 0);

        int portB = 0;
        for (int candidate = portA + 1; candidate <= PortScanEnd && portB == 0; ++candidate) {
            if (candidate == sinkPort) continue;
            runtime.openEndpoint(1, makeDraft(PortDraft::Udp,
                {{"udp.address", "127.0.0.1"}, {"udp.port", candidate},
                 {"udp.target", "127.0.0.1"}, {"udp.targetPort", sinkPort}}));
            if (lastState(states) == int(Transport::State::Open)) portB = candidate;
            else { runtime.closeEndpoint(1); states.clear(); }
        }
        QVERIFY(portB != 0);

        // 未建立时，发往 A 的数据报只留在 A，不出现在 B 的发送目标上。
        UdpTransport outsider(0);
        QSignalSpy outsiderState(&outsider, &AbstractTransport::stateChanged);
        outsider.start(makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
             {"udp.target", "127.0.0.1"}, {"udp.targetPort", portA}}), 1);
        QCOMPARE(lastState(outsiderState), int(Transport::State::Open));
        QCOMPARE(outsider.write(QByteArray("before"), QUuid()), qint64(6));
        QTRY_VERIFY(std::any_of(messages.cbegin(), messages.cend(), [](const QList<QVariant> &params) {
            const auto message = params.at(0).value<Traffic::Message>();
            return message.direction == Traffic::Direction::Receive && message.raw == QByteArray("before");
        }));
        QCOMPARE(sinkBData.count(), 0);

        BridgeLink link;
        link.enabled = true;
        link.direction = int(BridgeDirection::AtoB);
        const int beforeBridge = messages.count();
        runtime.setBridge(link);
        QVERIFY(messages.count() > beforeBridge);
        {
            const auto notice = messages.at(beforeBridge).at(0).value<Traffic::Message>();
            QCOMPARE(notice.direction, Traffic::Direction::State);
            QCOMPARE(notice.source.endpointIndex, -1);
            QVERIFY(notice.source.endpointId.isNull());
            QVERIFY(notice.statusText.contains(QStringLiteral("桥接已建立")));
            QVERIFY(notice.statusText.contains(QStringLiteral("→")));
            QVERIFY(notice.statusText.contains(QStringLiteral(":%1").arg(portA)));
            QVERIFY(notice.statusText.contains(QStringLiteral(":%1").arg(portB)));
        }
        QCOMPARE(outsider.write(QByteArray("live"), QUuid()), qint64(4));
        QTRY_COMPARE(sinkBData.count(), 1);
        QCOMPARE(sinkBData.at(0).at(2).toByteArray(), QByteArray("live"));
        QCOMPARE(sinkBData.at(0).at(3).toInt(), int(Traffic::Framing::Datagram));

        const auto forwarded = [&messages] {
            int count = 0;
            for (const auto &params : messages) {
                const auto message = params.at(0).value<Traffic::Message>();
                if (message.direction == Traffic::Direction::Transmit
                    && message.origin == Traffic::Origin::Forward
                    && message.raw == QByteArray("live"))
                    ++count;
            }
            return count;
        };
        QTRY_COMPARE(forwarded(), 1);
        int receiveId = 0;
        int forwardId = 0;
        for (const auto &params : messages) {
            const auto message = params.at(0).value<Traffic::Message>();
            if (message.raw != QByteArray("live")) continue;
            if (message.direction == Traffic::Direction::Receive && message.source.endpointIndex == 0)
                receiveId = int(message.id);
            if (message.direction == Traffic::Direction::Transmit && message.origin == Traffic::Origin::Forward)
                forwardId = int(message.id);
        }
        QVERIFY(receiveId > 0);
        QVERIFY(forwardId > receiveId);
        QTRY_VERIFY(std::any_of(messages.cbegin(), messages.cend(), [&](const QList<QVariant> &params) {
            const auto message = params.at(0).value<Traffic::Message>();
            return message.direction == Traffic::Direction::Transmit
                && message.origin == Traffic::Origin::Forward
                && message.raw == QByteArray("live")
                && message.source.peerAddress == QStringLiteral("127.0.0.1:%1").arg(sinkPort);
        }));

        // 单向 A → B：发往 B 的字节留在 B，不经 A 再写出去。
        UdpTransport toB(0);
        QSignalSpy toBState(&toB, &AbstractTransport::stateChanged);
        toB.start(makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
             {"udp.target", "127.0.0.1"}, {"udp.targetPort", portB}}), 1);
        QCOMPARE(lastState(toBState), int(Transport::State::Open));
        QCOMPARE(toB.write(QByteArray("only-b"), QUuid()), qint64(6));
        QTRY_VERIFY(std::any_of(messages.cbegin(), messages.cend(), [](const QList<QVariant> &params) {
            const auto message = params.at(0).value<Traffic::Message>();
            return message.direction == Traffic::Direction::Receive
                && message.source.endpointIndex == 1
                && message.raw == QByteArray("only-b");
        }));
        QCOMPARE(sinkBData.count(), 1);

        link.enabled = false;
        const int beforeStop = messages.count();
        runtime.setBridge(link);
        QVERIFY(messages.count() > beforeStop);
        {
            const auto notice = messages.at(beforeStop).at(0).value<Traffic::Message>();
            QCOMPARE(notice.direction, Traffic::Direction::State);
            QCOMPARE(notice.statusText, QStringLiteral("桥接已停止"));
        }
        QCOMPARE(outsider.write(QByteArray("after"), QUuid()), qint64(5));
        QTRY_VERIFY(std::any_of(messages.cbegin(), messages.cend(), [](const QList<QVariant> &params) {
            const auto message = params.at(0).value<Traffic::Message>();
            return message.direction == Traffic::Direction::Receive && message.raw == QByteArray("after");
        }));
        QCOMPARE(sinkBData.count(), 1);
        outsider.stop();
        toB.stop();
        sinkB.stop();
        runtime.shutdown();
    }

    void openingDiscardsUnreadInput()
    {
        UdpTransport receiver(0);
        QSignalSpy state(&receiver, &AbstractTransport::stateChanged);
        QSignalSpy received(&receiver, &AbstractTransport::dataReceived);
        const int port = findFreePort([&receiver, &state](int candidate) {
            receiver.start(makeDraft(PortDraft::Udp,
                {{"udp.address", "127.0.0.1"}, {"udp.port", candidate},
                 {"udp.target", ""}, {"udp.targetPort", 0}}), 1);
            const bool open = lastState(state) == int(Transport::State::Open);
            if (!open) state.clear();
            return open;
        });
        QVERIFY(port != 0);

        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        QCOMPARE(sender.writeDatagram("stale", QHostAddress::LocalHost, quint16(port)), qint64(5));
        // 数据报先到达套接字，再由打开动作丢掉，不能进入本次接收。
        QTRY_VERIFY(receiver.findChild<QUdpSocket *>()->hasPendingDatagrams());
        receiver.discardPendingInput();
        QTest::qWait(40);
        QCOMPARE(received.count(), 0);

        QCOMPARE(sender.writeDatagram("fresh", QHostAddress::LocalHost, quint16(port)), qint64(5));
        QTRY_COMPARE(received.count(), 1);
        QCOMPARE(received.at(0).at(2).toByteArray(), QByteArray("fresh"));
        receiver.stop();
    }

    void tcpClientReceivesBannerWrittenOnAccept()
    {
        QTcpServer banner;
        QVERIFY(banner.listen(QHostAddress::LocalHost, 0));
        connect(&banner, &QTcpServer::newConnection, &banner, [&banner] {
            auto *peer = banner.nextPendingConnection();
            peer->write("hello-now");
            peer->flush();
        });

        TcpClientTransport client(0);
        QSignalSpy clientState(&client, &AbstractTransport::stateChanged);
        QSignalSpy clientData(&client, &AbstractTransport::dataReceived);
        client.start(makeDraft(PortDraft::TcpClient,
            {{"client.host", "127.0.0.1"}, {"client.port", banner.serverPort()}, {"client.timeout", 5000}}), 1);
        QTRY_COMPARE(lastState(clientState), int(Transport::State::Open));
        QTRY_VERIFY(!clientData.isEmpty());
        QCOMPARE(clientData.at(0).at(2).toByteArray(), QByteArray("hello-now"));
        client.stop();
        banner.close();
    }

    void tcpServerReceivesPayloadWrittenOnConnect()
    {
        TcpServerTransport server(0);
        QSignalSpy serverState(&server, &AbstractTransport::stateChanged);
        QSignalSpy serverData(&server, &AbstractTransport::dataReceived);
        const int port = findFreePort([&server, &serverState](int candidate) {
            server.start(makeDraft(PortDraft::TcpServer,
                {{"server.address", "127.0.0.1"}, {"server.port", candidate}, {"server.clients", 4}}), 1);
            const bool open = lastState(serverState) == int(Transport::State::Open);
            if (!open) serverState.clear();
            return open;
        });
        QVERIFY(port != 0);

        QTcpSocket client;
        connect(&client, &QTcpSocket::connected, &client, [&client] {
            client.write("instant");
            client.flush();
        });
        client.connectToHost(QHostAddress::LocalHost, quint16(port));
        QTRY_VERIFY(!serverData.isEmpty());
        QCOMPARE(serverData.at(0).at(2).toByteArray(), QByteArray("instant"));
        client.disconnectFromHost();
        server.stop();
    }

    void udpReceivesAfterSendToClosedPort()
    {
        UdpTransport endpoint(0);
        QSignalSpy state(&endpoint, &AbstractTransport::stateChanged);
        QSignalSpy received(&endpoint, &AbstractTransport::dataReceived);
        const int port = findFreePort([&endpoint, &state](int candidate) {
            endpoint.start(makeDraft(PortDraft::Udp,
                {{"udp.address", "127.0.0.1"}, {"udp.port", candidate},
                 {"udp.target", "127.0.0.1"}, {"udp.targetPort", 1}}), 1);
            const bool open = lastState(state) == int(Transport::State::Open);
            if (!open) state.clear();
            return open;
        });
        QVERIFY(port != 0);

        // 1 端口没有监听。Windows 上 ICMP 端口不可达会让后续 recv 失败，接收因此停住。
        QCOMPARE(endpoint.write(QByteArray("miss"), QUuid()), qint64(4));
        QTest::qWait(300);

        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        QCOMPARE(sender.writeDatagram("alive", QHostAddress::LocalHost, quint16(port)), qint64(5));
        QTRY_COMPARE(received.count(), 1);
        QCOMPARE(received.at(0).at(2).toByteArray(), QByteArray("alive"));
        endpoint.stop();
    }

    void bridgeReportsRejectedForward()
    {
        EndpointRuntime runtime;
        QSignalSpy states(&runtime, &EndpointRuntime::endpointStateChanged);
        QSignalSpy messages(&runtime, &EndpointRuntime::messageReceived);

        int portA = 0;
        for (int candidate = PortScanBegin; candidate <= PortScanEnd && portA == 0; ++candidate) {
            runtime.openEndpoint(0, makeDraft(PortDraft::Udp,
                {{"udp.address", "127.0.0.1"}, {"udp.port", candidate},
                 {"udp.target", ""}, {"udp.targetPort", 0}}));
            if (lastState(states) == int(Transport::State::Open)) portA = candidate;
            else { runtime.closeEndpoint(0); states.clear(); }
        }
        QVERIFY(portA != 0);

        int portB = 0;
        for (int candidate = portA + 1; candidate <= PortScanEnd && portB == 0; ++candidate) {
            runtime.openEndpoint(1, makeDraft(PortDraft::Udp,
                {{"udp.address", "127.0.0.1"}, {"udp.port", candidate},
                 {"udp.target", ""}, {"udp.targetPort", 0}}));
            if (lastState(states) == int(Transport::State::Open)) portB = candidate;
            else { runtime.closeEndpoint(1); states.clear(); }
        }
        QVERIFY(portB != 0);

        BridgeLink link;
        link.enabled = true;
        link.direction = int(BridgeDirection::BtoA);
        runtime.setBridge(link);

        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        QCOMPARE(sender.writeDatagram("fwd", QHostAddress::LocalHost, quint16(portB)), qint64(3));

        QTRY_VERIFY(std::any_of(messages.cbegin(), messages.cend(), [](const QList<QVariant> &params) {
            const auto message = params.at(0).value<Traffic::Message>();
            return message.direction == Traffic::Direction::Receive
                && message.source.endpointIndex == 1
                && message.raw == QByteArray("fwd");
        }));
        QTRY_VERIFY(std::any_of(messages.cbegin(), messages.cend(), [](const QList<QVariant> &params) {
            const auto message = params.at(0).value<Traffic::Message>();
            return message.direction == Traffic::Direction::Transmit
                && message.origin == Traffic::Origin::Forward
                && message.source.endpointIndex == 0
                && message.raw == QByteArray("fwd")
                && message.state == Traffic::State::Failed;
        }));
        QTRY_VERIFY(std::any_of(messages.cbegin(), messages.cend(), [](const QList<QVariant> &params) {
            const auto message = params.at(0).value<Traffic::Message>();
            return message.direction == Traffic::Direction::Error
                && message.source.endpointIndex == 0
                && message.rawLength == 3
                && message.statusText.contains(QStringLiteral("丢弃未写出数据 3 字节"));
        }));
        runtime.shutdown();
    }

    void bridgeOpeningCountsUnreadInputAsDropped()
    {
        EndpointRuntime runtime;
        QSignalSpy states(&runtime, &EndpointRuntime::endpointStateChanged);
        QSignalSpy messages(&runtime, &EndpointRuntime::messageReceived);

        int portA = 0;
        for (int candidate = PortScanBegin; candidate <= PortScanEnd && portA == 0; ++candidate) {
            runtime.openEndpoint(0, makeDraft(PortDraft::Udp,
                {{"udp.address", "127.0.0.1"}, {"udp.port", candidate},
                 {"udp.target", ""}, {"udp.targetPort", 0}}));
            if (lastState(states) == int(Transport::State::Open)) portA = candidate;
            else { runtime.closeEndpoint(0); states.clear(); }
        }
        QVERIFY(portA != 0);
        runtime.openEndpoint(1, makeDraft(PortDraft::Udp,
            {{"udp.address", "127.0.0.1"}, {"udp.port", 0},
             {"udp.target", "127.0.0.1"}, {"udp.targetPort", portA}}));
        QCOMPARE(lastState(states), int(Transport::State::Open));

        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        QCOMPARE(sender.writeDatagram("stale", QHostAddress::LocalHost, quint16(portA)), qint64(5));
        // 数据报先停在套接字里，建立桥接时被丢掉，不能变成一条接收。
        QTRY_VERIFY(runtime.findChild<QUdpSocket *>()->hasPendingDatagrams());

        BridgeLink link;
        link.enabled = true;
        link.direction = int(BridgeDirection::AtoB);
        runtime.setBridge(link);

        QTRY_VERIFY(std::any_of(messages.cbegin(), messages.cend(), [](const QList<QVariant> &params) {
            const auto message = params.at(0).value<Traffic::Message>();
            return message.direction == Traffic::Direction::Error
                && message.source.endpointIndex == 0
                && message.rawLength == 5
                && message.statusText.contains(QStringLiteral("建立桥接前丢弃未读数据 5 字节"));
        }));
        bool sawStale = false;
        for (const auto &params : messages) {
            const auto message = params.at(0).value<Traffic::Message>();
            sawStale |= message.direction == Traffic::Direction::Receive && message.raw == QByteArray("stale");
        }
        QVERIFY(!sawStale);
        runtime.shutdown();
    }

    void ymodemRoundTripOverRawLoopback()
    {
        QTemporaryDir sourceDir;
        QTemporaryDir destinationDir;
        QVERIFY(sourceDir.isValid());
        QVERIFY(destinationDir.isValid());

        const QString sourcePath = sourceDir.filePath(QStringLiteral("firmware.bin"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QByteArray expected;
        expected.reserve(1500);
        for (int i = 0; i < 1500; ++i) expected.append(char(i & 0xff));
        QCOMPARE(source.write(expected), qint64(expected.size()));
        source.close();

        LoopbackRuntime senderRuntime;
        LoopbackRuntime receiverRuntime;
        FileTransferService sender;
        FileTransferService receiver;
        senderRuntime.sink = &receiver;
        receiverRuntime.sink = &sender;
        sender.setRuntime(&senderRuntime);
        receiver.setRuntime(&receiverRuntime);

        QSignalSpy senderFinished(&sender, &FileTransferService::transferFinished);
        QSignalSpy receiverFinished(&receiver, &FileTransferService::transferFinished);
        QSignalSpy failures(&sender, &FileTransferService::transferFailed);
        QSignalSpy receiverFailures(&receiver, &FileTransferService::transferFailed);

        receiver.startYModemReceive(0, destinationDir.path());
        sender.startYModemSend(0, sourcePath, 1024);
        QTest::qWait(3000);
        QVERIFY(senderFinished.count() == 1 && receiverFinished.count() == 1);
        QCOMPARE(failures.count(), 0);
        QCOMPARE(receiverFailures.count(), 0);

        const QString destinationPath = destinationDir.filePath(QStringLiteral("firmware.bin"));
        QFile destination(destinationPath);
        QVERIFY(destination.open(QIODevice::ReadOnly));
        QCOMPARE(destination.readAll(), expected);
    }

    void xmodemRoundTripOverRawLoopback()
    {
        QTemporaryDir sourceDir;
        QTemporaryDir destinationDir;
        QVERIFY(sourceDir.isValid());
        QVERIFY(destinationDir.isValid());

        const QString sourcePath = sourceDir.filePath(QStringLiteral("payload.bin"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        // 129 字节：两包，第二包只剩 1 字节，其余用 0x1A 填充。
        QByteArray expected(129, '\0');
        for (int i = 0; i < expected.size(); ++i) expected[i] = char(i & 0xff);
        expected[128] = char(0x1a);
        QCOMPARE(source.write(expected), qint64(expected.size()));
        source.close();

        LoopbackRuntime senderRuntime;
        LoopbackRuntime receiverRuntime;
        FileTransferService sender;
        FileTransferService receiver;
        senderRuntime.sink = &receiver;
        receiverRuntime.sink = &sender;
        sender.setRuntime(&senderRuntime);
        receiver.setRuntime(&receiverRuntime);

        QSignalSpy senderFinished(&sender, &FileTransferService::transferFinished);
        QSignalSpy receiverFinished(&receiver, &FileTransferService::transferFinished);
        QSignalSpy failures(&sender, &FileTransferService::transferFailed);
        QSignalSpy receiverFailures(&receiver, &FileTransferService::transferFailed);
        QSignalSpy senderStats(&sender, &FileTransferService::statsChanged);

        const QString destinationPath = destinationDir.filePath(QStringLiteral("saved.bin"));
        receiver.startXModemReceive(0, destinationPath);
        sender.startXModemSend(0, sourcePath);
        QTRY_COMPARE_WITH_TIMEOUT(senderFinished.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(receiverFinished.count(), 1, 3000);
        QCOMPARE(failures.count(), 0);
        QCOMPARE(receiverFailures.count(), 0);
        QVERIFY(!senderRuntime.sent.isEmpty());
        QCOMPARE(quint8(senderRuntime.sent.first().at(0)), quint8(0x01));
        QCOMPARE(senderRuntime.sent.first().size(), 133);

        QFile destination(destinationPath);
        QVERIFY(destination.open(QIODevice::ReadOnly));
        // 协议不传长度。第二包只有 1 字节有效数据，其余 127 字节 0x1A 原样保留。
        QByteArray padded = expected;
        padded.append(QByteArray(127, char(0x1a)));
        QCOMPARE(destination.readAll(), padded);
        QVERIFY(!senderStats.isEmpty());
        QCOMPARE(senderStats.last().at(0).toLongLong(), qint64(expected.size()));
    }

    void xmodemNakRetriesTheSameBlock()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath(QStringLiteral("two.bin"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray payload(200, 'Z');
        QCOMPARE(source.write(payload), qint64(payload.size()));
        source.close();

        LoopbackRuntime runtime;
        FileTransferService sender;
        sender.setRuntime(&runtime);
        sender.startXModemSend(0, sourcePath);
        sender.rawDataReceived(0, Traffic::Source{}, QByteArray(1, char(0x43)));
        QTRY_COMPARE(runtime.sent.size(), 1);
        const QByteArray first = runtime.sent.at(0);
        QCOMPARE(first.size(), 133);
        QCOMPARE(quint8(first.at(0)), quint8(0x01));
        QCOMPARE(quint8(first.at(1)), quint8(0x01));
        QCOMPARE(quint8(first.at(2)), quint8(0xfe));
        QCOMPARE(first.mid(3, 128), QByteArray(128, 'Z'));

        sender.rawDataReceived(0, Traffic::Source{}, QByteArray(1, char(0x15)));
        QTRY_COMPARE(runtime.sent.size(), 2);
        QCOMPARE(runtime.sent.at(1), first);

        sender.rawDataReceived(0, Traffic::Source{}, QByteArray(1, char(0x06)));
        QTRY_COMPARE(runtime.sent.size(), 3);
        QCOMPARE(quint8(runtime.sent.at(2).at(1)), quint8(0x02));
        QCOMPARE(runtime.sent.at(2).mid(3, 72), QByteArray(72, 'Z'));
        QCOMPARE(runtime.sent.at(2).mid(3 + 72, 56), QByteArray(56, char(0x1a)));
        sender.stop();
    }

    void xmodemChecksumModeUses132ByteFrames()
    {
        QTemporaryDir sourceDir;
        QTemporaryDir destinationDir;
        QVERIFY(sourceDir.isValid());
        QVERIFY(destinationDir.isValid());
        const QString sourcePath = sourceDir.filePath(QStringLiteral("sum.bin"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray payload(10, 'A');
        QCOMPARE(source.write(payload), qint64(payload.size()));
        source.close();

        LoopbackRuntime runtime;
        FileTransferService sender;
        sender.setRuntime(&runtime);
        sender.startXModemSend(0, sourcePath);
        sender.rawDataReceived(0, Traffic::Source{}, QByteArray(1, char(0x15)));
        QTRY_COMPARE(runtime.sent.size(), 1);
        const QByteArray frame = runtime.sent.at(0);
        QCOMPARE(frame.size(), 132);
        QCOMPARE(quint8(frame.at(0)), quint8(0x01));
        QCOMPARE(quint8(frame.at(1)), quint8(0x01));
        QCOMPARE(quint8(frame.at(2)), quint8(0xfe));
        quint8 sum = 0;
        for (int i = 3; i < 131; ++i) sum = quint8(sum + quint8(frame.at(i)));
        QCOMPARE(quint8(frame.at(131)), sum);

        LoopbackRuntime receiverRuntime;
        FileTransferService receiver;
        receiver.setRuntime(&receiverRuntime);
        QSignalSpy finished(&receiver, &FileTransferService::transferFinished);
        const QString destinationPath = destinationDir.filePath(QStringLiteral("out.bin"));
        receiver.startXModemReceive(0, destinationPath);
        QTRY_VERIFY(receiverRuntime.sent.contains(QByteArray(1, char(0x43))));
        // 对端只认 NAK，不会等 C。132 字节帧到达后应锁定校验和并 ACK。
        receiver.rawDataReceived(0, Traffic::Source{}, frame);
        QTRY_COMPARE(receiverRuntime.sent.last(), QByteArray(1, char(0x06)));
        receiver.rawDataReceived(0, Traffic::Source{}, QByteArray(1, char(0x04)));
        QTRY_COMPARE(finished.count(), 1);
        QFile destination(destinationPath);
        QVERIFY(destination.open(QIODevice::ReadOnly));
        QByteArray padded = payload;
        padded.append(QByteArray(118, char(0x1a)));
        QCOMPARE(destination.readAll(), padded);
    }

    void xmodemReceiverRejects1kAndAcceptsDuplicate()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("out.bin"));
        LoopbackRuntime runtime;
        FileTransferService receiver;
        receiver.setRuntime(&runtime);
        QSignalSpy failed(&receiver, &FileTransferService::transferFailed);
        QSignalSpy finished(&receiver, &FileTransferService::transferFinished);
        receiver.startXModemReceive(0, path);
        QTRY_VERIFY(runtime.sent.contains(QByteArray(1, char(0x43))));

        auto packet = [](quint8 block, char fill) {
            QByteArray data(128, fill);
            quint16 crc = 0;
            for (char byte : data) {
                crc ^= quint16(quint8(byte)) << 8;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021) : quint16(crc << 1);
            }
            QByteArray frame;
            frame.append(char(0x01));
            frame.append(char(block));
            frame.append(char(~block));
            frame.append(data);
            frame.append(char((crc >> 8) & 0xff));
            frame.append(char(crc & 0xff));
            return frame;
        };

        const int before = runtime.sent.size();
        receiver.rawDataReceived(0, Traffic::Source{}, QByteArray(1, char(0x02)) + QByteArray(1028, 'K'));
        QTRY_VERIFY(runtime.sent.size() > before);
        QCOMPARE(runtime.sent.last(), QByteArray(1, char(0x15)));
        QCOMPARE(failed.count(), 0);

        receiver.rawDataReceived(0, Traffic::Source{}, packet(1, 'A'));
        QTRY_COMPARE(runtime.sent.last(), QByteArray(1, char(0x06)));
        receiver.rawDataReceived(0, Traffic::Source{}, packet(1, 'A'));
        QTRY_COMPARE(runtime.sent.last(), QByteArray(1, char(0x06)));
        receiver.rawDataReceived(0, Traffic::Source{}, QByteArray(1, char(0x04)));
        QTRY_COMPARE(finished.count(), 1);

        QFile destination(path);
        QVERIFY(destination.open(QIODevice::ReadOnly));
        QCOMPARE(destination.readAll(), QByteArray(128, 'A'));
    }

    void xmodemReceiverNaksABadCrc()
    {
        QTemporaryDir destinationDir;
        QVERIFY(destinationDir.isValid());
        LoopbackRuntime runtime;
        FileTransferService receiver;
        receiver.setRuntime(&runtime);
        receiver.startXModemReceive(0, destinationDir.filePath(QStringLiteral("out.bin")));
        QTRY_VERIFY(runtime.sent.contains(QByteArray(1, char(0x43))));

        QByteArray packet(133, '\0');
        packet[0] = char(0x01);
        packet[1] = char(0x01);
        packet[2] = char(0xfe);
        packet[3] = 'A';
        receiver.rawDataReceived(0, Traffic::Source{}, packet);
        QTRY_VERIFY(runtime.sent.contains(QByteArray(1, char(0x15))));
        QVERIFY(!runtime.sent.contains(QByteArray(1, char(0x06))));
        receiver.stop();
    }

    void rawTransferRunsSideCommandsAndPublishesCrc()
    {
        QTemporaryDir sourceDir;
        QVERIFY(sourceDir.isValid());
        const QString sourcePath = sourceDir.filePath(QStringLiteral("payload.bin"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray payload("123456789");
        QCOMPARE(source.write(payload), qint64(payload.size()));
        source.close();

        LoopbackRuntime runtime;
        FileTransferService service;
        service.setRuntime(&runtime);
        QSignalSpy finished(&service, &FileTransferService::transferFinished);
        QSignalSpy failed(&service, &FileTransferService::transferFailed);
        QSignalSpy stats(&service, &FileTransferService::statsChanged);

        FileTransferSides sides;
        sides.before = "PRE";
        sides.beforeDelayMs = 20;
        sides.after = "POST";
        sides.afterDelayMs = 20;
        service.startRawSend(0, sourcePath, sides);
        QTRY_COMPARE_WITH_TIMEOUT(runtime.sent.size(), 3, 2000);
        QCOMPARE(runtime.sent.at(0), QByteArray("PRE"));
        QCOMPARE(runtime.sent.at(1), payload);
        QCOMPARE(runtime.sent.at(2), QByteArray("POST"));
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!stats.isEmpty());
        const QList<QVariant> last = stats.last();
        QCOMPARE(last.at(0).toLongLong(), qint64(payload.size()));
        QCOMPARE(last.at(2).toInt(), 0);
        QCOMPARE(last.at(3).toInt(), 0);
        QCOMPARE(last.at(4).toInt(), 0x31c3);
        QCOMPARE(last.at(5).toUInt(), 0xcbf43926u);
    }

    void rawSendUsesRequestedChunkSize()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("payload.bin"));
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray payload(10, 'A');
        QCOMPARE(source.write(payload), qint64(payload.size()));
        source.close();

        LoopbackRuntime runtime;
        FileTransferService service;
        service.setRuntime(&runtime);
        QSignalSpy finished(&service, &FileTransferService::transferFinished);
        QSignalSpy failed(&service, &FileTransferService::transferFailed);
        service.startRawSend(0, path, {}, 4);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(runtime.sent.size(), 3);
        QCOMPARE(runtime.sent.at(0), QByteArray(4, 'A'));
        QCOMPARE(runtime.sent.at(1), QByteArray(4, 'A'));
        QCOMPARE(runtime.sent.at(2), QByteArray(2, 'A'));
    }

    void rawSendDoesNotCompleteBeforeLocalAcceptance()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath("raw.bin"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("payload"), qint64(7));
        file.close();
        EndpointRuntime runtime; // 没有打开的端点，真实运行时必须拒绝发送。
        FileTransferService service;
        service.setRuntime(&runtime);
        QSignalSpy finished(&service, &FileTransferService::transferFinished);
        QSignalSpy failed(&service, &FileTransferService::transferFailed);
        service.startRawSend(0, file.fileName());
        QVERIFY(service.isBusy());
        QCOMPARE(finished.size(), 0);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(finished.size(), 0);
        QVERIFY(!service.isBusy());
    }

    void rawPartialWriteStopsWithoutSendingNextChunk()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath("partial.bin"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("123456789"), qint64(9));
        file.close();
        LoopbackRuntime runtime;
        runtime.acceptLimit = 3;
        FileTransferService service;
        service.setRuntime(&runtime);
        QSignalSpy failed(&service, &FileTransferService::transferFailed);
        QSignalSpy finished(&service, &FileTransferService::transferFinished);
        QSignalSpy stats(&service, &FileTransferService::statsChanged);
        service.startRawSend(0, file.fileName(), {}, 4);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(finished.size(), 0);
        QCOMPARE(runtime.sent, QList<QByteArray>{QByteArray("123")});
        QCOMPARE(stats.last().at(0).toLongLong(), qint64(3));
        PayloadChecksum checksum;
        checksum.feed("123", 3);
        QCOMPARE(stats.last().at(4).toInt(), int(checksum.crc16));
    }

    void rawRestartIgnoresPreviousAcceptance()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath("restart.bin"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("payload"), qint64(7));
        file.close();
        LoopbackRuntime runtime;
        FileTransferService service;
        service.setRuntime(&runtime);
        QSignalSpy results(&runtime, &EndpointRuntime::filePayloadAccepted);
        QSignalSpy finished(&service, &FileTransferService::transferFinished);
        QSignalSpy progress(&service, &FileTransferService::progressChanged);
        // 模拟前一笔接受结果滞留在跨线程队列，停止后才到达接收方。
        QObject::disconnect(&runtime, nullptr, &service, nullptr);
        service.startRawSend(0, file.fileName());
        QCoreApplication::sendPostedEvents(&runtime, QEvent::MetaCall);
        QCOMPARE(results.size(), 1);
        const auto previousRequest = results.first().at(0).value<QUuid>();
        service.stop();
        service.setRuntime(&runtime);
        service.startRawSend(0, file.fileName());
        emit runtime.filePayloadAccepted(previousRequest, 7);
        QVERIFY(service.isBusy());
        QCOMPARE(finished.size(), 0);
        QCOMPARE(progress.size(), 0);
        QCoreApplication::sendPostedEvents(&runtime, QEvent::MetaCall);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(progress.last().at(0).toLongLong(), qint64(7));
    }

    void udpSourceBudgetsPreserveAllReceivedBytes()
    {
        EndpointRuntime runtime;
        auto draft = makeDraft(PortDraft::Udp, {{"udp.address", "127.0.0.1"}, {"udp.port", 0}});
        draft.framing.mode = ReceiveFraming::IdleTimeout;
        draft.framing.idleTimeoutMs = 60000;
        runtime.openEndpoint(0, draft);
        auto *transport = runtime.findChild<UdpTransport *>();
        QVERIFY(transport);
        int largestPeerList = 0;
        int peerUpdates = 0;
        int warnings = 0;
        int rawChunks = 0;
        QMap<QString, QByteArray> received;
        connect(&runtime, &EndpointRuntime::endpointPeersChanged, this,
            [&](int, const QStringList &peers) {
                largestPeerList = qMax(largestPeerList, int(peers.size()));
                ++peerUpdates;
            });
        connect(&runtime, &EndpointRuntime::rawDataReceived, this,
            [&](int, const Traffic::Source &, const QByteArray &) { ++rawChunks; });
        connect(&runtime, &EndpointRuntime::messageReceived, this, [&](const Traffic::Message &message) {
            if (message.direction == Traffic::Direction::Receive)
                received[message.source.peerAddress] += message.raw;
            if (message.direction == Traffic::Direction::Warning) ++warnings;
            QVERIFY(message.issue != Traffic::Issue::DataLoss);
        });
        // 直接驱动传输层信号，稳定覆盖大量唯一来源，避免 UDP 内核丢包影响资源预算检查。
        constexpr int Sources = 1024;
        for (int i = 0; i < Sources; ++i) {
            auto source = Traffic::makeSource(draft, 0);
            source.generation = 1;
            source.peerAddress = QStringLiteral("127.0.0.1:%1").arg(20000 + i);
            emit transport->dataReceived(1, source, QByteArray::number(i), int(Traffic::Framing::Datagram));
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(rawChunks, Sources);
        QVERIFY(largestPeerList <= 256);
        QVERIFY(peerUpdates <= 256);
        QVERIFY(runtime.findChildren<QTimer *>().size() <= 256);
        QCOMPARE(warnings, 2); // 来源列表和组帧容量各提示一次，持续来包不能刷屏。
        runtime.closeEndpoint(0);
        QCOMPARE(received.size(), Sources);
        for (int i = 0; i < Sources; ++i)
            QCOMPARE(received.value(QStringLiteral("127.0.0.1:%1").arg(20000 + i)), QByteArray::number(i));
        QCOMPARE(runtime.findChildren<QTimer *>().size(), 0);
    }

    void rawSendWaitsBetweenChunks()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("payload.bin"));
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(QByteArray(10, 'A')), qint64(10));
        source.close();

        LoopbackRuntime runtime;
        FileTransferService service;
        service.setRuntime(&runtime);
        QSignalSpy finished(&service, &FileTransferService::transferFinished);
        QSignalSpy failed(&service, &FileTransferService::transferFailed);
        QSignalSpy progress(&service, &FileTransferService::progressChanged);
        QElapsedTimer clock;
        clock.start();
        service.startRawSend(0, path, {}, 4, 80);
        QCOMPARE(progress.size(), 0);
        QCoreApplication::sendPostedEvents(&runtime, QEvent::MetaCall);
        QCOMPARE(progress.size(), 1);
        QVERIFY2(clock.elapsed() < 40,
            qPrintable(QStringLiteral("第一包应马上发出，实际 %1 ms").arg(clock.elapsed())));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
        QCOMPARE(failed.count(), 0);
        QVERIFY2(clock.elapsed() >= 140,
            qPrintable(QStringLiteral("两包之间应各等 80 ms，实际 %1 ms").arg(clock.elapsed())));
        QCOMPARE(runtime.sent.size(), 3);
        QCOMPARE(runtime.sent.at(0), QByteArray(4, 'A'));
        QCOMPARE(runtime.sent.at(1), QByteArray(4, 'A'));
        QCOMPARE(runtime.sent.at(2), QByteArray(2, 'A'));
    }

    void rawReceiveIgnoresBytesDuringSideCommandWait()
    {
        QTemporaryDir destinationDir;
        QVERIFY(destinationDir.isValid());
        const QString destinationPath = destinationDir.filePath(QStringLiteral("out.bin"));

        LoopbackRuntime runtime;
        FileTransferService service;
        service.setRuntime(&runtime);
        QSignalSpy finished(&service, &FileTransferService::transferFinished);
        QSignalSpy failed(&service, &FileTransferService::transferFailed);

        FileTransferSides sides;
        sides.before = "GO";
        sides.beforeDelayMs = 40;
        sides.after = "DONE";
        sides.afterDelayMs = 20;
        QSignalSpy status(&service, &FileTransferService::statusChanged);
        service.startRawReceive(0, destinationPath, sides);
        service.rawDataReceived(0, Traffic::Source{}, QByteArray("NOISE"));
        QTRY_VERIFY_WITH_TIMEOUT(status.last().at(0).toString().contains(QStringLiteral("正在接收")), 2000);
        service.rawDataReceived(0, Traffic::Source{}, QByteArray("FILE"));
        service.stop();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
        QCOMPARE(failed.count(), 0);
        QTRY_VERIFY(runtime.sent.contains(QByteArray("GO")));
        QTRY_VERIFY(runtime.sent.contains(QByteArray("DONE")));

        QFile destination(destinationPath);
        QVERIFY(destination.open(QIODevice::ReadOnly));
        QCOMPARE(destination.readAll(), QByteArray("FILE"));
    }

    void elapsedStartsAtFirstDataInteraction()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        LoopbackRuntime runtime;
        FileTransferService receiver;
        receiver.setRuntime(&runtime);
        QSignalSpy receiveStats(&receiver, &FileTransferService::statsChanged);
        receiver.startRawReceive(0, dir.filePath(QStringLiteral("out.bin")));
        QTest::qWait(120);
        QVERIFY(!receiveStats.isEmpty());
        QCOMPARE(receiveStats.last().at(6).toLongLong(), qint64(0));
        receiver.rawDataReceived(0, Traffic::Source{}, QByteArray("AB"));
        QVERIFY(receiveStats.last().at(6).toLongLong() < 80);

        const QString sourcePath = dir.filePath(QStringLiteral("firmware.bin"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(QByteArray("payload")), qint64(7));
        source.close();

        FileTransferService sender;
        sender.setRuntime(&runtime);
        QSignalSpy sendStats(&sender, &FileTransferService::statsChanged);
        QSignalSpy status(&sender, &FileTransferService::statusChanged);
        FileTransferSides sides;
        sides.before = "PRE";
        sides.beforeDelayMs = 400;
        sender.startYModemSend(0, sourcePath, 128, sides);
        QTRY_VERIFY(status.last().at(0).toString().contains(QStringLiteral("前置指令")));
        sender.rawDataReceived(0, Traffic::Source{}, QByteArray(1, char(0x43)));
        QCOMPARE(sendStats.last().at(6).toLongLong(), qint64(0));

        QTRY_VERIFY_WITH_TIMEOUT(
            status.last().at(0).toString().contains(QStringLiteral("等待接收端")), 2000);
        QTest::qWait(250);
        QCOMPARE(sendStats.last().at(6).toLongLong(), qint64(0));
        sender.rawDataReceived(0, Traffic::Source{}, QByteArray(1, char(0x43)));
        sender.rawDataReceived(0, Traffic::Source{}, QByteArray(1, char(0x06)));
        sender.rawDataReceived(0, Traffic::Source{}, QByteArray(1, char(0x43)));
        QVERIFY(sendStats.last().at(0).toLongLong() > 0);
        QVERIFY(sendStats.last().at(6).toLongLong() < 200);
        sender.stop();
    }
};

QTEST_MAIN(TransportTest)
#include "tst_transport.moc"
