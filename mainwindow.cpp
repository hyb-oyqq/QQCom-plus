#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "version/version.h"
#include "app/endpointruntime.h"
#include "ui/panels/routing/routeeditorwidget.h"
#include "ui/panels/sending/sendpanel.h"
#include "ui/panels/traffic/trafficpanel.h"
#include "ui/panels/commands/commandpanel.h"
#include "discovery/portdiscoveryservice.h"
#include "persistence/preferencesstore.h"

#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// 串口列表的轮询间隔：设备插拔靠周期扫描发现，服务端会合并重叠的扫描。
constexpr int SerialAutoRefreshMs = 2000;
// 收发速率的采样节拍：状态栏数字每秒跳一次足够可读，也更省 GUI 时间。
constexpr int RateTickMs = 1000;

// 端点草稿 ↔ QVariantMap：preferences.ini 的存取都吃 QVariantMap，
// 领域值对象在这里做唯一一次形状转换，存取两侧共用同一份字段名。
QVariantMap framingToVariant(const ReceiveFraming &f)
{
    return {{"mode", int(f.mode)}, {"delimiter", f.delimiter}, {"idle", f.idleTimeoutMs},
        {"threshold", f.bufferThresholdBytes}, {"includeDelimiter", f.includeDelimiter},
        {"includeLengthPrefix", f.includeLengthPrefix}, {"logHints", f.logHints}};
}

ReceiveFraming framingFromVariant(const QVariantMap &m)
{
    ReceiveFraming f;
    f.mode = ReceiveFraming::Mode(m.value("mode", int(f.mode)).toInt());
    f.delimiter = m.value("delimiter").toByteArray();
    f.idleTimeoutMs = m.value("idle", f.idleTimeoutMs).toInt();
    f.bufferThresholdBytes = m.value("threshold", f.bufferThresholdBytes).toInt();
    f.includeDelimiter = m.value("includeDelimiter", f.includeDelimiter).toBool();
    f.includeLengthPrefix = m.value("includeLengthPrefix", f.includeLengthPrefix).toBool();
    f.logHints = m.value("logHints", f.logHints).toBool();
    return f;
}

QVariantMap draftToVariant(const PortDraft &d)
{
    return {{"id", d.id.toString()}, {"kind", d.kind}, {"values", d.values},
        {"framing", framingToVariant(d.framing)}};
}

PortDraft draftFromVariant(const QVariantMap &m)
{
    PortDraft d;
    const auto id = QUuid::fromString(m.value("id").toString());
    if (!id.isNull()) d.id = id; // 端点身份跨会话稳定：监视区开关、桥接预览都认它
    d.kind = m.value("kind", d.kind).toInt();
    d.values = m.value("values").toMap();
    d.framing = framingFromVariant(m.value("framing").toMap());
    return d;
}

// 状态栏字节数：KB/MB 用一位小数，B 保持整数——速率栏要的是"一眼量级"。
// 自由函数没有 this 上下文，tr 用 QObject 的全局语境。
QString formatBytes(qint64 bytes)
{
    if (bytes >= 1024 * 1024 * 1024)
        return QString::number(double(bytes) / (1024.0 * 1024.0 * 1024.0), 'f', 2) + QObject::tr(" GB");
    if (bytes >= 1024 * 1024)
        return QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 1) + QObject::tr(" MB");
    if (bytes >= 1024)
        return QString::number(double(bytes) / 1024.0, 'f', 1) + QObject::tr(" KB");
    return QString::number(bytes) + QObject::tr(" B");
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle(QStringLiteral(EASYDEBUG_APP_TITLE) + tr(" · 通信调试工作台"));
    setMinimumSize(1120, 660);
    resize(1440, 920);
    ui->menubar->hide();

    auto *root = new QVBoxLayout(ui->centralwidget);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Both endpoints stay expanded: configuring A or B never requires a selection step.
    auto *endpoints = new QFrame;
    endpoints->setObjectName("EndpointBar");
    auto *barLayout = new QHBoxLayout(endpoints);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(0);
    auto *endpointColumn = new QWidget;
    auto *endpointLayout = new QVBoxLayout(endpointColumn);
    endpointLayout->setContentsMargins(0, 0, 0, 0);
    endpointLayout->setSpacing(0);
    barLayout->addWidget(endpointColumn, 1);

    auto *preferences = new PreferencesStore(this);
    m_preferences = preferences;
    auto *discovery = new PortDiscoveryService(this);
    const auto reloadHistory = [this, preferences] {
        for (auto *selector : m_selectors)
            selector->setAddressHistory(preferences->addressHistory(false), preferences->addressHistory(true));
    };

    for (const auto kind : {PortDraft::Serial, PortDraft::TcpServer}) {
        const int index = m_selectors.size();
        const QString label = QString(QChar::fromLatin1('A' + char(index)));
        auto *selector = new PortSelectorWidget;
        selector->setObjectName("endpoint" + label);
        selector->setEndpointLabel(label);
        PortDraft draft;
        draft.kind = kind;
        selector->setDraft(draft);
        m_selectors.append(selector);
        m_ports.append(selector->draft());
        if (m_selectors.size() > 1) {
            auto *line = new QFrame;
            line->setObjectName("EndpointDivider");
            line->setFrameShape(QFrame::HLine);
            endpointLayout->addWidget(line);
        }
        endpointLayout->addWidget(selector);
        connect(selector, &PortSelectorWidget::configurationChanged, this, &MainWindow::refreshEndpoints);
        connect(selector, &PortSelectorWidget::addressCommitted, preferences, &PreferencesStore::rememberAddress);
        connect(discovery, &PortDiscoveryService::portsChanged, selector, &PortSelectorWidget::setSerialPorts);
    }
    // 端点草稿持久化：启动时恢复上次的串口选择与全部参数（含接收组帧）。
    // 串口名先以"待扫描"占位，设备列表扫描到位后自动对齐。
    restoreEndpointDrafts(preferences);
    reloadHistory();
    connect(preferences, &PreferencesStore::addressHistoryChanged, this, reloadHistory);
    connect(preferences, &PreferencesStore::saveFailed, this, [this](const QString &message) {
        ui->statusbar->showMessage(message, 5000);
    });
    QTimer::singleShot(0, discovery, &PortDiscoveryService::refresh);
    // 串口列表自动刷新：设备插拔无需手动操作；离屏实例（--snapshot、自动化测试）不轮询硬件。
    m_autoRefresh = new QTimer(this);
    m_autoRefresh->setInterval(SerialAutoRefreshMs);
    connect(m_autoRefresh, &QTimer::timeout, discovery, &PortDiscoveryService::refresh);

    auto *bridgeDivider = new QFrame;
    bridgeDivider->setObjectName("BridgeDivider");
    bridgeDivider->setFrameShape(QFrame::VLine);
    barLayout->addWidget(bridgeDivider);
    m_bridge = new RouteEditorWidget;
    barLayout->addWidget(m_bridge);
    root->addWidget(endpoints);

    auto *dataSplitter = new QSplitter(Qt::Vertical);
    dataSplitter->setObjectName("dataSplitter");
    dataSplitter->setChildrenCollapsible(false);
    // Minimums stay at each panel's own floor so the expanded endpoint rows have room.
    m_traffic = new TrafficPanel;
    m_sender = new SendPanel;
    // 显示选项持久化：监视区开关/列状态与发送区格式/行尾记进 preferences.ini。
    m_traffic->setPreferences(preferences);
    m_sender->setPreferences(preferences);
    // 监视区摘要放主窗口状态栏：条数是全局信息，不该占用显示区的纵向空间。
    connect(m_traffic, &TrafficPanel::summaryChanged, this,
        [this](const QString &text) { m_trafficStatus->setText(text); });
    dataSplitter->addWidget(m_traffic);
    dataSplitter->addWidget(m_sender);
    // 显示区承载更多内容：发送区正文固定约三行、贴着自身内容高度，
    // 初始在 showEvent 把它压到最小高度，剩余纵向空间全给显示区（发送区 stretch 为 0，
    // 窗口缩放的增量也都进显示区）；用户仍可拖动分隔条给发送区加高。
    dataSplitter->setStretchFactor(0, 1);
    dataSplitter->setStretchFactor(1, 0);
    m_dataSplitter = dataSplitter;

    // 水平分割：左侧数据区（监视+发送），右侧指令面板
    auto *horizontalSplitter = new QSplitter(Qt::Horizontal);

    auto *dataArea = new QWidget;
    auto *dataLayout = new QVBoxLayout(dataArea);
    dataLayout->setContentsMargins(14, 12, 14, 12);
    dataLayout->addWidget(dataSplitter);

    // 指令面板（右侧）
    m_commandPanel = new CommandPanel;
    m_commandPanel->setPreferencesStore(preferences);
    connect(m_commandPanel, &CommandPanel::sendCommand, this, &MainWindow::handleCommandSend);

    // 连接流量面板的展开/折叠按钮到指令面板
    connect(m_traffic, &TrafficPanel::commandPanelToggled, m_commandPanel, &CommandPanel::setExpanded);

    // 同步初始状态：确保按钮状态和面板状态一致
    // TrafficPanel 的按钮默认是 checked=true（展开），CommandPanel 的 m_expanded 也是 true
    // 但需要触发一次 setExpanded 来确保 UI 正确应用这个状态
    m_commandPanel->setExpanded(true);

    horizontalSplitter->addWidget(dataArea);
    horizontalSplitter->addWidget(m_commandPanel);
    horizontalSplitter->setStretchFactor(0, 1);
    horizontalSplitter->setStretchFactor(1, 0);

    root->addWidget(horizontalSplitter, 1);

    ui->statusbar->setSizeGripEnabled(true);
    auto *status = new QWidget;
    auto *statusLayout = new QHBoxLayout(status);
    statusLayout->setContentsMargins(12, 2, 12, 2);
    statusLayout->setSpacing(22);
    m_rxStatus = new QLabel(tr("RX   0 B   ·   0 B/s"));
    m_rxStatus->setObjectName("RxStatus");
    m_txStatus = new QLabel(tr("TX   0 B   ·   0 B/s"));
    m_txStatus->setObjectName("TxStatus");
    m_trafficStatus = new QLabel(tr("共 0 条"));
    m_trafficStatus->setObjectName("TrafficStatus");
    statusLayout->addWidget(m_rxStatus);
    statusLayout->addWidget(m_txStatus);
    statusLayout->addWidget(m_trafficStatus);
    m_routeStatus = new QLabel(tr("桥接  未启动"));
    m_routeStatus->setObjectName("RouteStatus");
    statusLayout->addWidget(m_routeStatus);
    statusLayout->addStretch();
    m_statusNote = new QLabel(tr("所有端口未连接"));
    m_statusNote->setObjectName("Muted");
    statusLayout->addWidget(m_statusNote);
    ui->statusbar->addWidget(status, 1);

    // 通信运行时：对象搬进 I/O 线程，GUI 只通过队列信号与它往来。
    // 线程结束时运行时在 I/O 线程内自我销毁，串口/socket 的收尾不落在 GUI。
    m_runtime = new EndpointRuntime;
    m_ioThread = new QThread(this);
    m_runtime->moveToThread(m_ioThread);
    connect(m_ioThread, &QThread::finished, m_runtime, &QObject::deleteLater);
    m_ioThread->start();
    connect(m_runtime, &EndpointRuntime::endpointStateChanged, this, &MainWindow::handleEndpointState);
    connect(m_runtime, &EndpointRuntime::endpointPeersChanged, this, &MainWindow::handleEndpointPeers);
    connect(m_runtime, &EndpointRuntime::messageReceived, this, &MainWindow::handleMessage);

    for (int index = 0; index < m_selectors.size(); ++index) {
        connect(m_selectors.at(index), &PortSelectorWidget::actionRequested, this,
            [this, index] { toggleEndpoint(index); });
    }

    connect(m_sender, &SendPanel::sendRequested, this,
        [this](const QByteArray &payload, int targetMask) {
            const int origin = int(Traffic::Origin::Manual);
            for (int index = 0; index < EndpointRuntime::EndpointCount; ++index)
                if (targetMask & (1 << index))
                    QMetaObject::invokeMethod(m_runtime, "sendPayload", Qt::QueuedConnection,
                        Q_ARG(int, index), Q_ARG(QByteArray, payload), Q_ARG(int, origin));
        });
    connect(m_bridge, &RouteEditorWidget::draftCountChanged, this, [this](int count) {
        m_routeStatus->setText(count > 0 ? tr("桥接  已建立 · 预览") : tr("桥接  未启动"));
    });

    // 速率节拍：与串口扫描一样只在窗口真实显示时运行。
    m_rateTimer = new QTimer(this);
    m_rateTimer->setInterval(RateTickMs);
    connect(m_rateTimer, &QTimer::timeout, this, &MainWindow::updateRateLabels);
    refreshEndpoints();
}

MainWindow::~MainWindow()
{
    for (auto *selector : m_selectors) selector->commitAddressHistory();
    // 先停 I/O 线程再拆 UI：运行时析构在 I/O 线程内完成，端口关闭
    // 不需要 GUI 参与，也就不会出现退出时的竞态。
    if (m_ioThread) {
        m_ioThread->quit();
        m_ioThread->wait();
    }
    delete ui;
}

void MainWindow::refreshEndpoints()
{
    bool ready = true;
    for (int i = 0; i < m_selectors.size(); ++i) {
        m_ports[i] = m_selectors.at(i)->draft();
        ready = ready && m_selectors.at(i)->hasValidConfiguration();
    }
    m_traffic->setPorts(m_ports);
    m_sender->setPorts(m_ports);
    m_bridge->setEndpoints(m_ports, ready);
    updateEndpointStatus(); // 端点类型变了，状态栏摘要的措辞要跟着换
    saveEndpointDrafts();   // 配置变化即落盘：任何改动都即时记住，无需等退出
}

void MainWindow::restoreEndpointDrafts(PreferencesStore *preferences)
{
    for (int index = 0; index < m_selectors.size(); ++index) {
        const auto saved = preferences
            ->value(QStringLiteral("endpoint/%1").arg(QLatin1Char('A' + char(index))))
            .toMap();
        if (saved.isEmpty()) continue;
        const auto draft = draftFromVariant(saved);
        if (draft.kind < PortDraft::Serial || draft.kind > PortDraft::Udp) continue;
        m_selectors.at(index)->setDraft(draft);
        m_ports[index] = draft;
    }
}

void MainWindow::saveEndpointDrafts()
{
    if (!m_preferences) return;
    for (int index = 0; index < m_selectors.size(); ++index)
        m_preferences->setValue(QStringLiteral("endpoint/%1").arg(QLatin1Char('A' + char(index))),
            draftToVariant(m_selectors.at(index)->draft()));
}

void MainWindow::toggleEndpoint(int index)
{
    if (index < 0 || index >= m_selectors.size()) return;
    // Closed → 打开；Opening/Open → 关闭（Opening 时即为取消连接）。
    // 按钮的可用性由配置校验保证，这里不需要重复校验提示。
    if (m_endpointStates[index] != Transport::State::Closed) {
        QMetaObject::invokeMethod(m_runtime, "closeEndpoint", Qt::QueuedConnection, Q_ARG(int, index));
        return;
    }
    const auto draft = m_selectors.at(index)->draft();
    if (!m_selectors.at(index)->hasValidConfiguration()) return;
    QMetaObject::invokeMethod(m_runtime, "openEndpoint", Qt::QueuedConnection,
        Q_ARG(int, index), Q_ARG(PortDraft, draft));
}

void MainWindow::autoOpenEndpoint(const PortDraft &draft)
{
    constexpr int EndpointA = 0;
    if (m_selectors.isEmpty()) return;
    // 与"打开 A"按钮走同一条路径：草稿落进选择控件 → 广播到各面板 → toggleEndpoint。
    // setDraft 是"载入"语义、不广播 configurationChanged，所以这里显式 refresh 一次；
    // 打开失败弹窗 / Error 行 / 状态行等需求 3 的行为由 openEndpoint 事件照常触发。
    m_selectors.at(EndpointA)->setDraft(draft);
    refreshEndpoints();
    toggleEndpoint(EndpointA);
}

void MainWindow::handleEndpointState(int index, int state, const QString &detail)
{
    if (index < 0 || index >= m_selectors.size()) return;
    m_endpointStates[index] = Transport::State(state);
    m_selectors.at(index)->setConnectionOpen(state == int(Transport::State::Open));
    updateEndpointStatus();
    // 打开成功的参数摘要进状态栏短消息；失败原因由独立的 Error 消息承载
    // （见 handleMessage），这里不重复播报"打开失败"这类没有原因的标题。
    if (Transport::State(state) == Transport::State::Open && !detail.isEmpty())
        ui->statusbar->showMessage(detail, 5000);
}

void MainWindow::handleEndpointPeers(int index, const QStringList &peers)
{
    if (index < 0 || index >= m_selectors.size()) return;
    m_endpointPeers[index] = peers;
    m_selectors.at(index)->setPeerList(peers);
    updateEndpointStatus();
}

void MainWindow::handleMessage(const Traffic::Message &message)
{
    // 字节统计与显示分开：暂停、筛选、清空都不改变计数；TX 只在本地
    // 写出完成/部分完成时计入，Failed 不冒充流量。
    if (message.isData()) {
        if (message.direction == Traffic::Direction::Receive)
            m_rxBytes += message.rawLength;
        else if (message.state == Traffic::State::LocalCompleted
            || message.state == Traffic::State::Partial)
            m_txBytes += message.rawLength;
    }
    if (message.direction == Traffic::Direction::Error && !message.statusText.isEmpty())
        ui->statusbar->showMessage(message.statusText, 5000);
    m_traffic->appendMessage(message);
}

void MainWindow::updateEndpointStatus()
{
    const auto stateText = [this](int index) {
        const int kind = m_ports.value(index).kind;
        switch (m_endpointStates[index]) {
        case Transport::State::Opening:
            return tr("连接中");
        case Transport::State::Open:
            if (kind == PortDraft::TcpServer)
                return tr("监听中 · %1 个客户端").arg(m_endpointPeers[index].size());
            return kind == PortDraft::Udp ? tr("已绑定") : tr("已连接");
        case Transport::State::Closed:
            break;
        }
        return tr("未连接");
    };
    m_statusNote->setText(tr("A %1 · B %2").arg(stateText(0), stateText(1)));
}

void MainWindow::updateRateLabels()
{
    const auto rxRate = qMax<qint64>(0, m_rxBytes - m_rxBytesAtTick);
    const auto txRate = qMax<qint64>(0, m_txBytes - m_txBytesAtTick);
    m_rxBytesAtTick = m_rxBytes;
    m_txBytesAtTick = m_txBytes;
    m_rxStatus->setText(tr("RX   %1   ·   %2/s").arg(formatBytes(m_rxBytes), formatBytes(rxRate)));
    m_txStatus->setText(tr("TX   %1   ·   %2/s").arg(formatBytes(m_txBytes), formatBytes(txRate)));
}

void MainWindow::loadExample() { m_traffic->loadExample(m_ports); }

quint64 MainWindow::sessionRecordCount() const { return m_traffic ? m_traffic->totalAccepted() : 0; }

qint64 MainWindow::sessionRxBytes() const { return m_rxBytes; }

void MainWindow::showRoutes()
{
    // objectName 是跨文件的字符串约定，找不到时静默返回而不是解引用空指针。
    if (auto *button = m_bridge->findChild<QWidget *>("bridgeButton"))
        button->setFocus();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    // 发送区贴着自身内容高度的分配在几何定型后应用一次——setSizes 接收的是绝对像素；
    // 发送区只取最小高度（正文约三行），剩余全部给数据显示区。
    if (!m_dataRatioApplied) {
        m_dataRatioApplied = true;
        const int total = qMax(0, m_dataSplitter->height() - m_dataSplitter->handleWidth());
        const int send = m_sender->minimumSizeHint().height();
        m_dataSplitter->setSizes({qMax(0, total - send), send});
    }
    if (!testAttribute(Qt::WA_DontShowOnScreen)) {
        m_autoRefresh->start();
        m_rateTimer->start();
    }
}

void MainWindow::hideEvent(QHideEvent *event)
{
    QMainWindow::hideEvent(event);
    m_autoRefresh->stop();
    m_rateTimer->stop();
}

void MainWindow::handleCommandSend(const QString &content, bool isHex, int targetEndpoint)
{
    // 指令面板发送：转换格式后通过发送面板发送
    // targetEndpoint: 0=A, 1=B, 2=AB
    if (!m_sender) return;

    // 将内容设置到发送面板
    // 这里简化处理：直接通过 runtime 发送
    QByteArray payload;
    if (isHex) {
        // HEX 格式：移除空格，转换为字节
        QString hexStr = content;
        hexStr.remove(QChar(' '));
        hexStr.remove(QChar(','));
        payload = QByteArray::fromHex(hexStr.toLatin1());
    } else {
        payload = content.toUtf8();
    }

    if (payload.isEmpty()) return;

    // 发送到指定端点
    QList<int> targets;
    if (targetEndpoint == 0) {
        targets.append(0); // A
    } else if (targetEndpoint == 1) {
        targets.append(1); // B
    } else if (targetEndpoint == 2) {
        targets.append(0); // AB
        targets.append(1);
    }

    for (int index : targets) {
        if (index >= 0 && index < m_selectors.size()) {
            // 通过运行时发送
            QMetaObject::invokeMethod(m_runtime, "write",
                                      Qt::QueuedConnection,
                                      Q_ARG(int, index),
                                      Q_ARG(QByteArray, payload),
                                      Q_ARG(QUuid, QUuid())); // 空 UUID = 广播
        }
    }
}

