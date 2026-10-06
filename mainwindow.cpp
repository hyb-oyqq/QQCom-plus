#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "version/version.h"
#include "app/endpointruntime.h"
#include "ui/panels/routing/routeeditorwidget.h"
#include "ui/panels/sending/sendpanel.h"
#include "ui/panels/traffic/trafficpanel.h"
#include "ui/panels/commands/commandpanel.h"
#include "ui/common/payloadcodec.h"
#include "discovery/portdiscoveryservice.h"
#include "persistence/preferencesstore.h"

#include <QCloseEvent>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
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
    setMinimumSize(900, 720);  // 宽度的真实下限由内容决定（见 updateWindowMinimumWidth），这里只兜底
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
        showStatusNote(message, 5000);
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
    // 端口条**不挂在根布局上**，而是稍后与数据区一起组成左侧列（见"左侧列"一段）。
    // 这样它只跟左侧同宽，不会横贯到指令面板下方去——用户反馈"点击扩展时端口设置区域
    // 跟着拉长"（面板展开把窗口加宽，横贯全宽的端口条就被一起拉长了）。

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
    horizontalSplitter->setObjectName("horizontalSplitter");
    horizontalSplitter->setChildrenCollapsible(true);  // 允许完全折叠子控件
    m_horizontalSplitter = horizontalSplitter;

    auto *dataArea = new QWidget;
    m_dataArea = dataArea;
    auto *dataLayout = new QVBoxLayout(dataArea);
    // **左右归零**：横向留白统一由左侧列那一层的 6px 提供（端口条与卡片共用同一条边界）。
    // 这里若再给左右边距，卡片外框就会比端口条底色更靠内，右上角与右下角错开
    //（用户反馈"端口选择区与数据显示区没有对齐"——上一轮只对齐了内容、漏了外框）。
    // 内容左边缘 = 左列 6 + 这里 0 + 卡片内边距 6 = 12，与端口条内容的缩进一致。
    // 上下留 8px：这一层是纯留白，收紧后纵向多出的空间全给数据显示区。
    // 上下 6px：端口条卡片与数据区卡片之间的缝。两张卡片各有 1px 边框，6px 足够分开；
    // 再宽就像"两块板子中间空一块"（用户反馈区域之间要窄一些）。
    dataLayout->setContentsMargins(0, 6, 0, 6);
    dataLayout->addWidget(dataSplitter);

    // 左侧列 = 端口条 + 数据区（纵向堆叠）。
    //
    // 端口条放进这一列而不是留在根布局里，是为了让它**只跟左侧同宽**：留在根布局
    // （横贯全宽）时，展开指令面板会把窗口加宽，横贯的端口条跟着被拉长，看起来就像
    // 端口设置"被撑开了"（用户反馈）。放进左列后，面板占满整个右侧、端口条止步于
    // 分栏线，两件事各归其位。
    //
    // **左右边距 6 只在这一层给**，于是"端口条底色"与"数据区卡片外框"落在同一条竖线上
    // （两者本来就都铺到这个位置）。内容左边缘另有一层等式：这里 6 + 各自的内边距 6 = 12，
    // 端口条内容与卡片内容的缩进因此也一样——两层都要成立，界面才真正对齐
    // （用户反馈"端口选择区与数据显示区没有对齐"）。改任一项都要回头量这两个边界。
    auto *leftColumn = new QWidget;
    m_leftColumn = leftColumn;
    auto *leftLayout = new QVBoxLayout(leftColumn);
    leftLayout->setContentsMargins(6, 0, 6, 0);
    leftLayout->setSpacing(0);
    leftLayout->addWidget(endpoints, 0);
    leftLayout->addWidget(dataArea, 1);

    // 指令面板（右侧）
    m_commandPanel = new CommandPanel;
    m_commandPanel->setPreferencesStore(preferences);
    connect(m_commandPanel, &CommandPanel::sendCommand, this, &MainWindow::handleCommandSend);

    // 连接流量面板的展开/折叠按钮到指令面板
    // 先记一次当前几何再切换：commandPanelToggled 是"按钮点下去"的第一手信号，
    // 此刻 CommandPanel 还没被压成 0 宽，这是最后一个能读到真实展开几何的时刻
    // （连线顺序就是执行顺序，所以这一条必须在下面的 setExpanded 之前）。
    connect(m_traffic, &TrafficPanel::commandPanelToggled, this,
        [this] { recordExpandedGeometry(); });
    connect(m_traffic, &TrafficPanel::commandPanelToggled, m_commandPanel, &CommandPanel::setExpanded);

    // 连接指令面板的展开/折叠状态变化：**数据区宽度是锚点，窗口宽度跟着面板变**。
    //
    // 用户钦定：收起面板时数据区**大小不变**、窗口就地变窄；展开时窗口向右变宽。
    // 早先写的是"收起就把全部空间给左侧"，于是收起那一下数据区突然变宽、展开又缩回去——
    // 监视区的折行位置与列宽跟着跳，正看着的内容会整屏重排。
    //
    // 这里**不需要记住"上次的数据区宽度"**：收起状态下右侧为 0，分栏的 sizes[0]
    // 本身就是数据区当前宽度，展开时原样用它即可。于是"用户拖过窗口/分栏"的结果
    // 天然被带上，也不用维护一份可能与现实脱节的记忆值。
    connect(m_commandPanel, &CommandPanel::expandedChanged, this, [this](bool expanded) {
        // 先更新窗口最小宽度：它随展开状态变化（收起时不必为面板留位置）。
        // 顺序反了的话，紧接着的"就地变窄"会被旧的最小宽度顶住、根本缩不动。
        updateWindowMinimumWidth();

        if (!m_horizontalSplitter) return;

        const int handle = m_expandedHandle > 0 ? m_expandedHandle
                                                : m_horizontalSplitter->handleWidth();
        // 面板宽度：**优先还原上次实测的那一份**，只在没有记录时（首次展开）才回落到
        // 内容最小宽度。用最小宽度会让"展开 → 收起 → 展开"每次都比上一次宽一点，
        // 窗口一路漂移（实测差 6px）；而且用户拖宽过面板的话，最小宽度等于把他的调整抹掉。
        const int panelMin = m_commandPanel->expandedMinimumWidth() > 0
            ? m_commandPanel->expandedMinimumWidth()
            : m_commandPanel->minimumWidth();
        const int panelWidth = expanded ? qMax(m_expandedPanelWidth, panelMin) : 0;

        // 左列宽度取**展开态下记录的**那一份。
        // 不能读 sizes[0]：走到这里时 CommandPanel 已经把面板压成 0、分栏也把空出来的
        // 空间分给了左列，读到的是"已经变宽之后"的值（实测 +28px），拿它算目标宽度
        // 只会把错误固化。快照由 resizeEvent / splitterMoved 在展开态下维护。
        int columnWidth = m_expandedColumnWidth;
        if (columnWidth <= 0) {
            const QList<int> sizes = m_horizontalSplitter->sizes();
            columnWidth = sizes.size() == 2 ? sizes.at(0) : 0;
        }
        if (columnWidth <= 0) columnWidth = m_leftColumnMinimumWidth;
        columnWidth = qMax(columnWidth, m_leftColumnMinimumWidth);

        // 目标窗口宽度分两种情形，差别就在**分栏手柄是否占位**：
        //   展开 —— 两块都在，中间那条手柄在，宽度是 左列 + 面板 + 手柄；
        //   收起 —— 面板宽 0，QSplitter 会把相邻那条手柄一起隐藏，那一段宽度
        //           会**全部归给唯一可见的左列**。所以收起时目标宽度里不能再算手柄，
        //           否则左列会白长出手柄那一段（实测 1242 → 1270，多出来的正是它）。
        // 外壳从**分栏自己的宽度**反推（与分栏内部怎么分无关，因此稳定），不写常量。
        const int chrome = width() - m_horizontalSplitter->width();
        const int contentWidth = expanded ? columnWidth + panelWidth + handle : columnWidth;
        const int targetWidth = contentWidth + chrome;
        if (width() != targetWidth) resize(targetWidth, height());

        // resize 之后分栏才拿到新宽度，此时按上面算好的两份宽度分配，落到就是最终值。
        if (expanded) {
            m_horizontalSplitter->setSizes({columnWidth, panelWidth});
            m_expandedColumnWidth = columnWidth;
            m_expandedPanelWidth = panelWidth;
        } else {
            m_horizontalSplitter->setSizes({m_horizontalSplitter->width(), 0});
        }
    });

    // 用户拖分栏或缩放窗口时，把展开态的宽度记下来——这是"收起后左列该多宽"的唯一依据。
    connect(horizontalSplitter, &QSplitter::splitterMoved, this, [this] { recordExpandedGeometry(); });

    horizontalSplitter->addWidget(leftColumn);
    horizontalSplitter->addWidget(m_commandPanel);
    horizontalSplitter->setStretchFactor(0, 1);
    horizontalSplitter->setStretchFactor(1, 0);

    // 「扩展」按钮跟随**恢复出来的**面板状态：按钮的 checked 初值是硬编码的 true，
    // 上次退出时若为收起，它会显示"已展开"而面板其实收着，两者对不上。
    // 放在这里（分栏已装好之后）而不是更早：setChecked 会 emit commandPanelToggled，
    // 那条链路的后半段要读分栏几何。
    //
    // 这里原来写的是**无条件的 `setExpanded(true)`**——它会把 CommandPanel::loadPreferences()
    // 刚恢复的"收起"直接冲掉，于是"扩展状态不被记忆"（用户反馈）。
    // 现在恢复只由 loadPreferences 一处负责，这里只做按钮与状态的对齐。
    m_traffic->setCommandPanelToggle(m_commandPanel->isExpanded());

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
    // 语义身份而不是样式名：工作区里有多处标签共用 "Muted"，拿它定位是脆的。
    // 样式不丢——QSS 里 `QLabel#Muted, QLabel#StatusNote` 是同一条规则（见 styles/*.qss）。
    m_statusNote->setObjectName("StatusNote");
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
        [this](const QByteArray &payload, int targetMask, bool scheduled) {
            // 手动与定时走**同一条发送路径**，但来源标记不同：定时的一拍标 Scheduled，
            // 屏幕上显示为 `TX·定时` 而不是 `TX·手动`（见 TrafficTextView 的前缀拼装）。
            // 这个参数以前被 lambda 丢掉了——形参只声明了两个，于是定时发送的每一拍
            // 都被记成"手动"，用户想区分"我点的"和"机器按期发的"就无从看起。
            const int origin = int(scheduled ? Traffic::Origin::Scheduled
                                             : Traffic::Origin::Manual);
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

    // 数据区宽度下限 = **数据区自己报出的最小宽度**。
    //
    // 这是一条完整的链：dataArea 的布局 → 竖直分栏 → TrafficPanel → 头块两行 → 每个控件。
    // 链上的控件一律按自己的 sizeHint 声明下限（`rowmetrics.h::pinRowToContents` 把横向
    // 策略钉成 Fixed，语义就是"布局按我的 sizeHint 给我位置"），所以头两行"最少要多宽"
    // 由 Qt 在布局时算出来，包含内边距、间距、边框在内，一项不漏。
    //
    // 这里刻意不做任何像素求和——早先按"逐项 sizeHint 之和 + 手算边距"估过一次，
    // 漏了 TrafficPanel 自己的 20px 内边距，结果窗口到了最小宽度、顶栏最右那个按钮
    // 仍差 1px 探出右缘。这种账目错误不会报错，只在特定窗口宽度上悄悄显形。
    m_trafficMinimumWidth = dataArea->minimumSizeHint().width();
    // 同时把它作为硬下限挂在数据区上：手动拖分栏也拖不到比这更窄。
    dataArea->setMinimumWidth(m_trafficMinimumWidth);
    // 左列下限由布局报出：端口条与数据区各自声明下限，左列的 minimumSizeHint 取较大者。
    // 与 m_trafficMinimumWidth 同一手法——**不手工求和**，免得漏掉某个内边距。
    // 顺序有讲究：必须在上面设完 dataArea 的下限之后再量，否则量到的是还没被下限约束的宽度。
    m_leftColumnMinimumWidth = leftColumn->minimumSizeHint().width();
    // 同样挂成硬下限：分栏拖拽时左列不得被压到装不下端口条。
    leftColumn->setMinimumWidth(m_leftColumnMinimumWidth);
    updateWindowMinimumWidth();
}

void MainWindow::restoreWindowState()
{
    if (!m_preferences) return;
    const QByteArray geometry = m_preferences->value(QStringLiteral("window/geometry")).toByteArray();
    // 空值（首次启动，或用户从未退出过）就保持构造时的默认尺寸。
    if (!geometry.isEmpty()) restoreGeometry(geometry);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 几何在**退出时**落盘一次即可：它是低频的用户意图，不需要在每次 resize 里写盘。
    // 存的是 saveGeometry() 的完整状态（大小 + 位置 + 最大化），不只是宽高。
    if (m_preferences) m_preferences->setValue(QStringLiteral("window/geometry"), saveGeometry());
    QMainWindow::closeEvent(event);
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
    // 发送按钮的可用性也按当前连接状态收敛一次：端点配置变化可能发生在"某端已打开"
    // 之后（例如程序启动时带 --auto-open，或将来加了自动重连），这时 handleEndpointState
    // 早已跑过，只靠它喂状态就会漏。这里按 m_endpointStates 幂等同步，不引入第二个真相源。
    for (int index = 0; index < EndpointRuntime::EndpointCount; ++index)
        m_sender->setEndpointOpen(index, m_endpointStates[index] == Transport::State::Open);
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
    // ★ 端点状态一变就必须喂给发送区：发送按钮的可用性=「选中目标里至少有一个已连接」，
    // 而它读的是 SendPanel::m_openFlags。这条线曾经**根本没接**——setEndpointOpen()
    // 全项目没有任何调用点，于是 m_openFlags 恒为 false、发送按钮**永远禁用**，
    // 「发送」看上去就是个摆设（用户反馈"实现发送区的发送功能"）。
    // 与"连接状态只由发送按钮外观 + tooltip 承载"配套：这里是那外观的唯一驱动源。
    if (m_sender) m_sender->setEndpointOpen(index, state == int(Transport::State::Open));
    updateEndpointStatus();
    // 打开成功的参数摘要不再走状态栏短消息：那会隐藏整行统计数据（见 showStatusNote
    // 的说明），而且监视区现在已经有一条 `状态: 已打开 · COM2 @115200` 的迁移记录，
    // 信息重复。失败原因由独立的 Error 行承载。
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
    // 错误播报走状态栏右端的临时提示格（showStatusNote），**不是** showMessage：
    // 后者会隐藏所有 addWidget 挂的普通部件，把 RX/TX/条数统计整行顶掉——而打开失败
    // 常常连续发生，用户看到的正是"统计数据消失、很久才回来"（见 showStatusNote 说明）。
    if (message.direction == Traffic::Direction::Error && !message.statusText.isEmpty())
        showStatusNote(message.statusText, 5000);
    m_traffic->appendMessage(message);
}

void MainWindow::recordExpandedGeometry()
{
    // 只在**展开态**记：收起态下两个宽度是 {左列, 0}，把它记下来等于把"面板宽度 0"
    // 当成了常态，下次展开就没宽度可加。布局还没落定时也跳过（0 宽不是有效观测）。
    if (!m_horizontalSplitter || !m_commandPanel || !m_commandPanel->isExpanded()) return;
    const QList<int> sizes = m_horizontalSplitter->sizes();
    if (sizes.size() != 2 || sizes.at(0) <= 0 || sizes.at(1) <= 0) return;
    m_expandedColumnWidth = sizes.at(0);
    m_expandedPanelWidth = sizes.at(1);
    // 手柄实际宽度 = 分栏宽度 - 两块之和。三个数出自同一次观测，反推出来的值一定是准的；
    // 而 handleWidth() 是"样式希望的手柄宽度"，QSS 可以把它画得更窄（见成员声明处的说明）。
    m_expandedHandle = qMax(0, m_horizontalSplitter->width() - sizes.at(0) - sizes.at(1));
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // 窗口缩放（用户拖边缘、最大化、布局自身收敛）都会让水平分栏重新分配两块宽度，
    // 所以快照要在这里刷新——而不是只在"点了扩展按钮"那一刻更新，
    // 否则用户先拖宽窗口再收起，收起时会退回一个过期的左列宽度。
    recordExpandedGeometry();
}

void MainWindow::updateWindowMinimumWidth()
{
    // 窗口最小宽度 = 左右两块面板"内容完整可见"所需宽度之和 + 分栏手柄 + 外边距。
    // 这条规则的动机（用户钦定）：窗口不够宽时应当**变大窗口**，而不是压缩面板内容
    // ——后者会让"分组"下拉消失、表格末尾的手柄列被裁掉、过滤条按钮挤成一团。
    //
    // 左侧用 **m_leftColumnMinimumWidth**（左列 = 端口条 + 数据区），不是数据区下限：
    // 分栏的左半边是整列，端口条也在里面，只按数据区算会漏掉它的宽度需求。
    const int panelWidth = m_commandPanel && m_commandPanel->isExpanded()
        ? m_commandPanel->expandedMinimumWidth()
        : 0;
    const int splitterWidth = m_horizontalSplitter ? m_horizontalSplitter->handleWidth() : 0;
    const int wanted = m_leftColumnMinimumWidth + panelWidth + splitterWidth;
    // 下限**完全由内容决定**，不再设那个写死的 1280 地板。
    //
    // 地板曾经是"怕面板展开挤压按钮"的保险，现在那件事由内容下限负责，地板就成了
    // 纯副作用：用户收起面板时窗口想缩到"左列 + 外壳"，却被地板顶住，多出来的宽度
    // 无处可去、只能塞给左列——正是用户报的"收回时数据区反而变宽"（实测多 28px）。
    // 内容下限是诚实的，加地板只会让"就地收窄"在某些尺寸下失效。
    const int minWidth = wanted;
    if (minimumWidth() != minWidth) setMinimumWidth(minWidth);
    // 高度下限不变：纵向空间本来就全给数据区，没有"被压缩的分支"要保护。
    setMinimumHeight(720);
}

void MainWindow::updateEndpointStatus()
{
    // 有临时提示（showStatusNote）正在显示时，它"借走"了右端这一格：此处不得覆盖。
    // 否则紧随状态上报而来的摘要会立刻把刚播出的提示顶掉——实测"打开失败"时，
    // errorOccurred 播出的原因在同一个事件循环里就被随后 stateChanged→ 本函数重算的
    // "A 未连接 · B 未连接"覆盖，用户根本来不及看到。提示到时由 m_statusNoteTimer 的
    // 回调再次调用本函数，那一刻计时器已停、本守卫放行，摘要照常恢复（复原路径不变）。
    if (m_statusNoteTimer && m_statusNoteTimer->isActive()) return;
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

void MainWindow::showStatusNote(const QString &text, int ms)
{
    if (!m_statusNote) return;
    if (!m_statusNoteTimer) {
        m_statusNoteTimer = new QTimer(this);
        m_statusNoteTimer->setSingleShot(true);
        // 复原走 updateEndpointStatus()：它是"A/B 连接状态汇总"的唯一计算点，
        // 这里复用它而不是另写一句文案，免得两处措辞各自漂移。
        connect(m_statusNoteTimer, &QTimer::timeout, this, &MainWindow::updateEndpointStatus);
    }
    m_statusNote->setText(text);
    m_statusNoteTimer->start(qMax(1, ms));
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
    // 恢复上次的窗口大小/位置。**必须在这里、不能在构造末尾**：窗口框架（标题栏）要到
    // 首次显示时才建立，构造期 restoreGeometry() 会因为框架还没算而差出一整条标题栏
    //（实测高度 800 → 774，差的 26px 正是它）。只做一次。
    if (!m_windowStateRestored) {
        m_windowStateRestored = true;
        restoreWindowState();
    }
    // 发送区贴着自身内容高度的分配在几何定型后应用一次——setSizes 接收的是绝对像素；
    // 发送区只取最小高度（正文约三行），剩余全部给数据显示区。
    // 放在几何恢复之后：它要用恢复后的分栏高度算绝对像素。
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
    // 首次显示时布局刚定型：这里补记一次展开几何。构造期记不了（那时窗口还没排过版，
    // 两块宽度都还是 0），而 resizeEvent 也不是每次显示都会来——用户在窗口尺寸没变时
    // "隐藏再显示"就收不到它。两条路都记，保证第一次点「扩展」时快照一定是有效的。
    recordExpandedGeometry();
}

void MainWindow::hideEvent(QHideEvent *event)
{
    QMainWindow::hideEvent(event);
    m_autoRefresh->stop();
    m_rateTimer->stop();
}

void MainWindow::handleCommandSend(const CommandItem &command)
{
    if (!m_sender) return;

    // 内容按指令自己的格式声明解析（表格里的 Hex 勾选框），**结尾符在解析之后按字节追加**。
    //
    // 这一段是"实现发送数据的功能"的核心：早先这里自己做 remove(' ') + fromHex()，
    // 绕开了 PayloadCodec 的统一规则，于是把结尾符当文本拼进内容一起交给 fromHex()
    // ——那三个控制字符不是十六进制数字，会被静默跳过，HEX 指令的结尾符就此丢失
    // （文本指令碰巧没事，因为 toUtf8() 直接把 \r\n 写成字节）。
    const auto format = command.isHex ? PayloadCodec::Format::Hex : PayloadCodec::Format::Utf8;
    const auto parsed = PayloadCodec::parse(command.content, format);
    if (!parsed.valid()) {
        // 解析失败必须说出来：以前这条路径静默什么都不做，看上去就是"按钮坏了"。
        // 同 handleMessage 的错误播报：走 showStatusNote 而不是 showMessage，
        // 避免把统计行顶掉（项目铁律，见 showStatusNote 的说明）。
        showStatusNote(tr("指令「%1」无法按 %2 解析：%3")
                           .arg(command.description.isEmpty() ? command.content
                                                              : command.description,
                               PayloadCodec::formatName(format), parsed.error),
            6000);
        return;
    }

    const QByteArray payload = parsed.bytes + command.tailBytes();
    if (payload.isEmpty()) return;

    // 目标不在指令里：以发送区当前选中的目标为准（用户钦定）。交给发送区发出去，
    // 与手点「发送」共用同一个信号、同一套目标规则、同一个来源标记。
    m_sender->deliverPayload(payload);
}

