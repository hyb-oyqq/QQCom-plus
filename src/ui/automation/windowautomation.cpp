#include "mainwindow.h"
#include "automation/capabilitycatalog.h"
#include "automation/commandautomation.h"
#include "persistence/preferencesstore.h"
#include "automation/localcontrol.h"
#include "app/commandmanager.h"
#include "app/endpointruntime.h"
#include "discovery/portdiscoveryservice.h"
#include "ui/common/payloadcodec.h"
#include "ui/common/styledialogs.h"
#include "ui/panels/commands/commandpanel.h"
#include "ui/panels/routing/routeeditorwidget.h"
#include "ui/panels/sending/sendpanel.h"
#include "ui/panels/traffic/trafficpanel.h"
#include "version/version.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QMenu>
#include <QNetworkInterface>
#include <QStatusBar>
#include <QToolButton>

void MainWindow::setupAutomation(QStatusBar *statusbar)
{
    const auto handler = [this](const QString &name, const QJsonObject &arguments) {
        return executeAutomationTool(name, arguments);
    };
    m_controlServer = new LocalControlServer(QStringLiteral(QQCOM_VERSION_STR), this);
    m_controlServer->setHandler(handler);
    if (!m_controlServer->start())
        qWarning() << "Local CLI control unavailable:" << m_controlServer->errorString();
    auto *button = new QToolButton(statusbar);
    button->setObjectName("aiAccessButton");
    button->setText(tr("AI 接入"));
    button->setToolTip(tr("AI 可通过 CLI 操作端口、查询数据和编辑快捷指令"));
    button->setAutoRaise(true);
    button->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(button);
    auto *copyCli = menu->addAction(tr("复制 CLI 入口"));
    connect(copyCli, &QAction::triggered, this, [this] {
        const auto launcher = QCoreApplication::applicationDirPath() + "/qqcom-cli.cmd";
        const bool hasLauncher = QFile::exists(launcher);
        const auto path = QDir::toNativeSeparators(hasLauncher ? launcher : QCoreApplication::applicationFilePath());
        // PowerShell single-quoted literal; a path containing apostrophes remains literal.
        auto quoted = path;
        quoted.replace("'", "''");
        QApplication::clipboard()->setText("& '" + quoted + (hasLauncher ? "' describe" : "' --cli describe"));
        showStatusNote(tr("已复制 CLI 入口"), 4000);
    });
    menu->addSeparator();
    auto *help = menu->addAction(tr("如何让 AI 发现 QQCom"));
    connect(help, &QAction::triggered, this, [this] {
        StyleDialogs::information(this, tr("AI 接入"),
            tr("使用随程序附带的 qqcom-cli.cmd 即可接入。\n"
               "AI 先运行 describe 了解能力，再执行 state、configure、open、send、traffic 等命令。\n"
               "CLI 会自动连接当前 QQCom；没有窗口时自动启动，多个窗口时按实例选择。\n"
               "可用“复制 CLI 入口”把完整命令交给 AI。\n\n"
               "支持 A/B 串口、TCP 客户端、TCP 服务器、UDP 的配置、开关、发送，以及状态、历史记录和界面文本查询。\n"
               "快捷指令分组和组内指令支持增删改、完整内容查询和组内排序。"));
    });
    button->setMenu(menu);
    statusbar->addPermanentWidget(button);
}

QJsonObject MainWindow::automationState() const
{
    QJsonArray endpoints;
    for (int index = 0; index < m_selectors.size(); ++index) {
        auto endpoint = CapabilityCatalog::endpoint(m_selectors.at(index)->draft());
        endpoint.insert("endpoint", index == 0 ? "A" : "B");
        endpoint.insert("state", QStringList{"closed", "opening", "open"}.value(int(m_endpointStates[index])));
        endpoint.insert("operationPending", m_automationPending[index]);
        endpoint.insert("recovering", m_recoverArmed[index]);
        endpoint.insert("valid", m_selectors.at(index)->hasValidConfiguration());
        endpoint.insert("validationError", m_selectors.at(index)->validationError());
        endpoint.insert("peers", QJsonArray::fromStringList(m_endpointPeers[index]));
        endpoints.append(endpoint);
    }
    const auto link = m_bridge->link();
    return {{"application", "QQCom"}, {"version", QQCOM_VERSION_STR}, {"title", windowTitle()},
        {"endpoints", endpoints},
        {"localControl", m_controlServer->descriptor()},
        {"counters", QJsonObject{{"rxBytes", QString::number(m_rxBytes)}, {"txBytes", QString::number(m_txBytes)},
                                 {"droppedBytes", QString::number(m_droppedBytes)}}},
        {"bridge", QJsonObject{{"enabled", link.enabled}, {"direction", QStringList{"A-to-B", "B-to-A", "both"}.value(link.direction)}}},
        {"send", m_sender->automationState()}, {"traffic", m_traffic->automationState()},
        {"commandsExpanded", m_commandPanel && m_commandPanel->isExpanded()}};
}

QJsonObject MainWindow::executeAutomationTool(const QString &name, const QJsonObject &arguments)
{
    const auto validation = CapabilityCatalog::validateArguments(name, arguments);
    if (!validation.isEmpty()) return CapabilityCatalog::failure("invalid_arguments", validation);
    if (name == "qqcom_get_capabilities") return CapabilityCatalog::capabilities();
    if (name == "qqcom_get_state") return automationState();
    if (CommandAutomation::handles(name)) {
        const bool mutation = CommandAutomation::isMutation(name);
        if (mutation && m_commandPanel->hasPendingSends())
            return CapabilityCatalog::failure("commands_busy", "Wait for pending command sends or stop the active command loop before editing");
        if (mutation && m_commandPanel->hasActiveCommandEditor())
            return CapabilityCatalog::failure("commands_busy", "Finish the active GUI command editor before editing through automation");
        auto *manager = CommandManager::instance();
        auto result = CommandAutomation::execute(manager, name, arguments);
        if (mutation && !result.contains("error")) {
            manager->save(m_preferences);
            const bool persisted = m_preferences->sync();
            result.insert("applied", true);
            result.insert("persisted", persisted);
            if (!persisted) result.insert("error", CapabilityCatalog::failure("save_failed",
                "Edit applied in memory but could not be saved; inspect commands before retrying").value("error"));
        }
        return result;
    }
    if (name == "qqcom_list_ports") {
        if (arguments.value("refresh").toBool() && m_discovery) m_discovery->refresh();
        QJsonArray devices, addresses;
        for (const auto &port : m_automationPorts)
            devices.append(QJsonObject{{"port", port.portName}, {"description", port.description},
                {"manufacturer", port.manufacturer}, {"systemLocation", port.systemLocation}});
        QSet<QString> unique{QStringLiteral("0.0.0.0"), QStringLiteral("127.0.0.1")};
        for (const auto &address : QNetworkInterface::allAddresses())
            if (address.protocol() == QAbstractSocket::IPv4Protocol) unique.insert(address.toString());
        auto sorted = unique.values();
        sorted.sort();
        for (const auto &address : sorted) addresses.append(address);
        return {{"serialPorts", devices}, {"localIPv4", addresses},
                {"scanGeneration", QString::number(m_serialScanGeneration)},
                {"refreshRequested", arguments.value("refresh").toBool()}};
    }
    if (name == "qqcom_query_traffic") return m_traffic->queryTraffic(arguments);
    if (name == "qqcom_read_ui_text") return m_traffic->readDisplayedText(arguments.value("maxChars").toInt(16000));
    if (name == "qqcom_clear_traffic") {
        m_traffic->clearTraffic();
        return {{"cleared", true}};
    }
    if (name == "qqcom_list_commands") {
        QJsonArray groups;
        int remaining = 500;
        bool truncated = false;
        for (const auto &set : CommandManager::instance()->allSets()) {
            if (remaining == 0 || groups.size() >= 100) { truncated = true; break; }
            QJsonArray commands;
            for (const auto &command : set.commands) {
                if (remaining == 0) { truncated = true; break; }
                --remaining;
                auto data = command.toJson();
                data.insert("content", command.content.left(1024));
                data.insert("contentTruncated", command.content.size() > 1024);
                data.insert("description", command.description.left(1024));
                data.insert("descriptionTruncated", command.description.size() > 1024);
                commands.append(data);
            }
            groups.append(QJsonObject{{"id", set.id.toString(QUuid::WithoutBraces)},
                {"name", set.name.left(1024)}, {"notes", set.notes.left(1024)}, {"commands", commands}});
        }
        return {{"groups", groups}, {"truncated", truncated}};
    }
    const int index = arguments.value("endpoint").toString() == "A" ? 0 : 1;
    if (name == "qqcom_configure_endpoint") {
        if (m_endpointStates[index] != Transport::State::Closed || m_recoverArmed[index] || m_automationPending[index])
            return CapabilityCatalog::failure("endpoint_busy", "Close the endpoint and wait for closed state before configuring");
        auto draft = m_selectors.at(index)->draft();
        const auto previous = draft;
        QString error;
        if (!CapabilityCatalog::configure(arguments, draft, error)) return CapabilityCatalog::failure("invalid_configuration", error);
        m_selectors.at(index)->setDraft(draft);
        if (!m_selectors.at(index)->hasValidConfiguration()) {
            const auto reason = m_selectors.at(index)->validationError();
            m_selectors.at(index)->setDraft(previous);
            return CapabilityCatalog::failure("invalid_configuration", reason);
        }
        refreshEndpoints();
        return {{"configured", true}, {"endpoint", arguments.value("endpoint")},
                {"configuration", CapabilityCatalog::endpoint(m_selectors.at(index)->draft())}};
    }
    if (name == "qqcom_open_endpoint" || name == "qqcom_close_endpoint") {
        const bool open = name == "qqcom_open_endpoint";
        if (m_automationPending[index]) return CapabilityCatalog::failure("operation_pending", "Wait for the previous endpoint operation");
        if (open && (m_endpointStates[index] != Transport::State::Closed || m_recoverArmed[index]))
            return {{"submitted", false}, {"alreadyActive", true}, {"endpoint", arguments.value("endpoint")}};
        if (!open && m_endpointStates[index] == Transport::State::Closed && !m_recoverArmed[index])
            return {{"submitted", false}, {"alreadyClosed", true}, {"endpoint", arguments.value("endpoint")}};
        if (m_sender->fileTransferBusy()) return CapabilityCatalog::failure("file_transfer_busy", "Stop the active file transfer first");
        if (open && !m_selectors.at(index)->hasValidConfiguration())
            return CapabilityCatalog::failure("invalid_configuration", m_selectors.at(index)->validationError());
        m_automationPending[index] = true;
        toggleEndpoint(index);
        // Run after the runtime command and its state signals, including recovery cancellation.
        QMetaObject::invokeMethod(m_runtime, [this, index] {
            QMetaObject::invokeMethod(this, [this, index] { m_automationPending[index] = false; }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
        return {{"submitted", true}, {"endpoint", arguments.value("endpoint")}, {"action", open ? "open" : "close"}};
    }
    if (name == "qqcom_send") {
        if (m_endpointStates[index] != Transport::State::Open || m_automationPending[index])
            return CapabilityCatalog::failure("endpoint_not_open", "Open the endpoint and wait for state=open before sending");
        if (m_sender->fileTransferBusy()) return CapabilityCatalog::failure("file_transfer_busy", "Stop the active file transfer first");
        const auto formats = QStringList{"hex", "dec", "base64", "utf8", "gbk", "ascii", "escape"};
        const auto suffix = arguments.value("lineEnding").toString("none");
        const QByteArray tail = suffix == "cr" ? QByteArray("\r") : suffix == "lf" ? QByteArray("\n")
            : suffix == "crlf" ? QByteArray("\r\n") : QByteArray();
        const auto parsed = PayloadCodec::parse(arguments.value("data").toString(),
            PayloadCodec::Format(formats.indexOf(arguments.value("format").toString())), tail);
        if (!parsed.valid() || parsed.bytes.isEmpty())
            return CapabilityCatalog::failure("invalid_payload", parsed.valid() ? "Payload must not be empty" : parsed.error);
        QMetaObject::invokeMethod(m_runtime, "sendPayload", Qt::QueuedConnection,
            Q_ARG(int, index), Q_ARG(QByteArray, parsed.bytes), Q_ARG(int, int(Traffic::Origin::Automation)));
        return {{"submitted", true}, {"endpoint", arguments.value("endpoint")}, {"bytes", parsed.bytes.size()},
                {"remoteAcknowledged", false}};
    }
    return CapabilityCatalog::failure("unknown_tool", name);
}
