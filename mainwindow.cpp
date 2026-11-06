#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "version/version.h"
#include "app/endpointruntime.h"
#include "automation/localcontrol.h"
#include "ui/panels/routing/routeeditorwidget.h"
#include "ui/panels/sending/sendpanel.h"
#include "ui/panels/traffic/trafficpanel.h"
#include "ui/panels/commands/commandpanel.h"
#include "ui/common/payloadcodec.h"
#include "ui/common/bytesformat.h"
#include "ui/common/apptheme.h"
#include "ui/common/endpointalertstate.h"
#include "ui/common/tintedicon.h"
#include "discovery/portdiscoveryservice.h"
#include "persistence/preferencesstore.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QImage>
#include <QResizeEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#pragma comment(lib, "user32.lib")
#endif
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

// 串口列表的轮询间隔：设备插拔靠周期扫描发现，服务端会合并重叠的扫描。
constexpr int SerialAutoRefreshMs = 2000;
// 收发速率的采样节拍：状态栏数字每秒跳一次足够可读，也更省 GUI 时间。
constexpr int RateTickMs = 1000;
// 状态栏右侧动作（检查更新、外观、置顶）与端点状态块共用这一高度，中线才对齐。
constexpr int StatusControlHeight = 20;

// 左列（端口条 + 数据区）。内容有多宽只留给「展开面板」当起步宽度，
// 不能再当成左右拖动条的停靠点：QSplitter 用子控件的 minimumSizeHint 卡住手柄，
// 端口条和监视顶栏都很宽，手柄就拖不进主界面，快捷面板的宽度被封死。
class LeftColumn : public QWidget
{
public:
    using QWidget::QWidget;

    QSize minimumSizeHint() const override
    {
        QSize hint = QWidget::minimumSizeHint();
        hint.setWidth(0);
        return hint;
    }
};

// 动态属性变了必须重新套样式，否则 QSS 的 [state="open"] 这类选择器不会刷新。
void restyleProperty(QWidget *widget, const char *name, const QVariant &value)
{
    if (!widget || widget->property(name) == value) return;
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QFrame *makeStatusSeparator()
{
    auto *line = new QFrame;
    line->setObjectName(QStringLiteral("StatusSep"));
    line->setFrameShape(QFrame::NoFrame);
    line->setFixedSize(1, 14);
    return line;
}

// 整张图统一换成告警色，保留原图的透明轮廓、明暗和白色图案。
// 常用尺寸单独生成并缓存；正常时直接恢复原始 QIcon。
QIcon alertWindowIcon(const QIcon &base, const QColor &color)
{
    QIcon result;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
        QImage image = base.pixmap(QSize(size, size), 1.0).toImage().convertToFormat(QImage::Format_ARGB32);
        if (image.isNull()) continue;
        for (int y = 0; y < image.height(); ++y) {
            auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const QColor original = QColor::fromRgba(pixels[x]);
                const float saturation = original.hsvSaturationF();
                const QColor tinted = QColor::fromHsvF(color.hsvHueF(),
                    saturation * color.hsvSaturationF(),
                    original.valueF() * (1.0f - saturation * (1.0f - color.valueF())),
                    original.alphaF());
                pixels[x] = tinted.rgba();
            }
        }
        result.addPixmap(QPixmap::fromImage(image));
    }
    return result;
}

// 端点草稿 ↔ QVariantMap：preferences.ini 的存取都吃 QVariantMap，
// 领域值对象在这里做唯一一次形状转换，存取两侧共用同一份字段名。
QVariantMap framingToVariant(const ReceiveFraming &f)
{
    return {{"mode", int(f.mode)}, {"delimiter", f.delimiter}, {"idle", f.idleTimeoutMs},
        {"threshold", f.bufferThresholdBytes}, {"includeDelimiter", f.includeDelimiter},
        {"logHints", f.logHints}};
}

ReceiveFraming framingFromVariant(const QVariantMap &m)
{
    ReceiveFraming f;
    // 长度前缀曾经插在结束符和空闲超时之间。去掉之后空闲超时的存盘值
    // 从 3 变成 2，旧配置读到 3 时收成空闲超时。
    int mode = m.value("mode", int(f.mode)).toInt();
    if (mode == 3) mode = int(ReceiveFraming::IdleTimeout);
    f.mode = ReceiveFraming::Mode(mode);
    f.delimiter = m.value("delimiter").toByteArray();
    f.idleTimeoutMs = m.value("idle", f.idleTimeoutMs).toInt();
    f.bufferThresholdBytes = m.value("threshold", f.bufferThresholdBytes).toInt();
    // 300 字节和 2048 KB 都是改默认之前写进草稿的出厂值。用户没改过它们，
    // 读到时换成 2048 字节。改过的其他阈值保持原样。
    if (f.bufferThresholdBytes == 300 || f.bufferThresholdBytes == 2048 * 1024)
        f.bufferThresholdBytes = ReceiveFraming{}.bufferThresholdBytes;
    f.includeDelimiter = m.value("includeDelimiter", f.includeDelimiter).toBool();
    f.logHints = m.value("logHints", f.logHints).toBool();
    return f;
}

QVariantMap draftToVariant(const PortDraft &d)
{
    return {{"id", d.id.toString()}, {"kind", d.kind}, {"values", d.values},
        {"autoRecover", d.autoRecover}, {"framing", framingToVariant(d.framing)}};
}

PortDraft draftFromVariant(const QVariantMap &m)
{
    PortDraft d;
    const auto id = QUuid::fromString(m.value("id").toString());
    if (!id.isNull()) d.id = id; // 端点身份跨会话稳定：监视区开关认它
    d.kind = m.value("kind", d.kind).toInt();
    d.autoRecover = m.value("autoRecover", false).toBool();
    d.values = m.value("values").toMap();
    d.framing = framingFromVariant(m.value("framing").toMap());
    return d;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle(QStringLiteral(QQCOM_APP_TITLE));
    m_normalWindowIcon = windowIcon().isNull() ? QIcon(QStringLiteral(":/icons/app.ico")) : windowIcon();
    setWindowIcon(m_normalWindowIcon);
    for (int level = 0; level < 2; ++level) {
        for (int bright = 0; bright < 2; ++bright) {
            QColor color(level == 0 ? QStringLiteral("#d97706") : QStringLiteral("#dc2626"));
            if (!bright) color = color.darker(155);
            m_alertWindowIcons[level][bright] = alertWindowIcon(m_normalWindowIcon, color);
        }
    }
    // 不设最小尺寸。默认布局约束会把内容的 minimumSizeHint 写回窗口，边框就拖不下去。
    releaseWindowSizeFloor();
    resize(1280, 860);
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
    m_discovery = discovery;
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
        auto *alerts = new EndpointAlertState(this);
        alerts->setObjectName(QStringLiteral("endpointAlerts") + label);
        m_endpointAlerts[index] = alerts;
        connect(alerts, &EndpointAlertState::changed, this, &MainWindow::updateEndpointAlerts);
        connect(alerts, &EndpointAlertState::attentionRequested, this, &MainWindow::requestAlertAttention);
        if (m_selectors.size() > 1) {
            auto *line = new QFrame;
            line->setObjectName("EndpointDivider");
            line->setFrameShape(QFrame::HLine);
            endpointLayout->addWidget(line);
        }
        endpointLayout->addWidget(selector);
        connect(selector, &PortSelectorWidget::configurationChanged, this, &MainWindow::refreshEndpoints);
        connect(selector, &PortSelectorWidget::configurationEdited, alerts, &EndpointAlertState::acknowledge);
        connect(selector, &PortSelectorWidget::addressCommitted, preferences, &PreferencesStore::rememberAddress);
        connect(discovery, &PortDiscoveryService::portsChanged, selector, &PortSelectorWidget::setSerialPorts);
    }
    connect(discovery, &PortDiscoveryService::scanFinished, this,
        [this](quint64 generation, const QList<SerialPortDescriptor> &ports) {
            m_serialScanGeneration = generation;
            m_automationPorts = ports;
            m_presentSerialPorts.clear();
            for (const auto &port : ports)
                m_presentSerialPorts.insert(port.portName);
            recoverEndpoints();
        });
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
    m_recoverTimer = new QTimer(this);
    m_recoverTimer->setInterval(PortDraft::AutoRecoverRetryMs);
    connect(m_recoverTimer, &QTimer::timeout, this, &MainWindow::recoverEndpoints);

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
    connect(m_sender, &SendPanel::preferredHeightChanged, this, &MainWindow::fitSendPane);
    // 监视区摘要放主窗口状态栏：条数是全局信息，不该占用显示区的纵向空间。
    connect(m_traffic, &TrafficPanel::summaryChanged, this,
        [this](const QString &text) { m_trafficStatus->setText(text); });
    connect(m_traffic, &TrafficPanel::performanceChanged, this,
        [this](const QString &text) { m_trafficStatus->setToolTip(text); });
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
    // 上下 4px：端口条卡片与数据区卡片之间的缝。两张卡片各有 1px 边框，4px 足够分开。
    dataLayout->setContentsMargins(0, 4, 0, 4);
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
    auto *leftColumn = new LeftColumn;
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
        if (!m_horizontalSplitter) return;

        const int handle = m_expandedHandle > 0 ? m_expandedHandle
                                                : m_horizontalSplitter->handleWidth();
        // 面板宽度：**优先还原上次实测的那一份**，只在没有记录时（首次展开）才回落到
        // 内容最小宽度。用最小宽度会让"展开 → 收起 → 展开"每次都比上一次宽一点，
        // 窗口一路漂移（实测差 6px）；而且用户拖宽过面板的话，最小宽度等于把他的调整抹掉。
        const int panelMin = m_commandPanel->expandedMinimumWidth() > 0
            ? m_commandPanel->expandedMinimumWidth()
            : m_commandPanel->minimumWidth();
        // 有快照就还原那一次的实际宽度。快照可以小于内容首选宽度：分栏允许把面板压窄，
        // 用 qMax 会把窗口撑得比收起前更宽。
        const int panelWidth = expanded
            ? (m_expandedPanelWidth > 0 ? m_expandedPanelWidth : panelMin)
            : 0;

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

        // 收起时把扩展区移出分栏。QSplitter 只要右侧控件还在，就会重新显示中间那条手柄并给它留宽；
        // hide 和 QSS 都收不掉这一截。移走之后分栏里只剩左列，拖动条按 Qt 自己的规则消失。
        // 展开则先加宽窗口，再把面板加回去。顺序反了，分栏会先在旧宽度里把左列撑开。
        const int chrome = width() - m_horizontalSplitter->width();
        const int contentWidth = expanded ? columnWidth + panelWidth + handle : columnWidth;
        const int targetWidth = contentWidth + chrome;

        if (!expanded)
            syncExtensionPanel(false);
        if (width() != targetWidth) resize(targetWidth, height());
        if (expanded) {
            syncExtensionPanel(true);
            m_horizontalSplitter->setSizes({columnWidth, panelWidth});
            m_expandedColumnWidth = columnWidth;
            m_expandedPanelWidth = panelWidth;
        }
    });

    // 用户拖分栏或缩放窗口时，把展开态的宽度记下来——这是"收起后左列该多宽"的唯一依据。
    connect(horizontalSplitter, &QSplitter::splitterMoved, this, [this] { recordExpandedGeometry(); });

    horizontalSplitter->addWidget(leftColumn);
    horizontalSplitter->setStretchFactor(0, 1);
    // 收起时不要先 addWidget 再移出。QSplitter 只要右侧控件进过分栏，就会先造出中间手柄并留宽；
    // 初次启动默认收起，这一截不该出现。展开过的会话仍把面板加回去。
    if (m_commandPanel->isExpanded()) {
        horizontalSplitter->addWidget(m_commandPanel);
        horizontalSplitter->setStretchFactor(1, 0);
    } else {
        m_commandPanel->setParent(this);
        m_commandPanel->hide();
    }

    // 「扩展」按钮跟随**恢复出来的**面板状态。按钮初值是收起，与没有保存记录时的
    // 面板一致。上次退出时若为展开，这里再把按钮按下。
    // 放在这里（分栏已装好之后）而不是更早：setChecked 会 emit commandPanelToggled，
    // 那条链路的后半段要读分栏几何。
    //
    // 这里原来写的是**无条件的 `setExpanded(true)`**——它会把 CommandPanel::loadPreferences()
    // 刚恢复的"收起"直接冲掉，于是"扩展状态不被记忆"（用户反馈）。
    // 现在恢复只由 loadPreferences 一处负责，这里只做按钮与状态的对齐。
    // 偏好里已经是收起时，按钮初值也是收起，setChecked 不会 emit。
    m_traffic->setCommandPanelToggle(m_commandPanel->isExpanded());

    root->addWidget(horizontalSplitter, 1);

    // 整条状态栏放进同一个布局，再按垂直居中摆。
    // 以前「检查更新」和置顶是两个 permanent widget，QStatusBar 各自排版，
    // 文字按钮和 16px 图标的高度不一样，中线就错开了。
    ui->statusbar->setSizeGripEnabled(true);
    auto *status = new QWidget;
    status->setObjectName(QStringLiteral("StatusStrip"));
    auto *statusLayout = new QHBoxLayout(status);
    statusLayout->setContentsMargins(12, 0, 8, 0);
    statusLayout->setSpacing(0);
    const auto addCentered = [statusLayout](QWidget *widget, int stretch = 0) {
        statusLayout->addWidget(widget, stretch, Qt::AlignVCenter);
    };
    const auto addGap = [statusLayout](int pixels) { statusLayout->addSpacing(pixels); };
    const auto addSep = [&] {
        addGap(12);
        addCentered(makeStatusSeparator());
        addGap(12);
    };
    const auto makeKey = [](const QString &text, const QString &name) {
        auto *label = new QLabel(text);
        label->setObjectName(name);
        label->setAlignment(Qt::AlignVCenter);
        return label;
    };
    const auto makeFigure = [](const QString &text, const QString &name) {
        auto *label = new QLabel(text);
        label->setObjectName(name);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        return label;
    };

    const QString zeroBytes = formatBytes(0);
    const QString zeroRate = tr("%1/s").arg(zeroBytes);
    addCentered(makeKey(QStringLiteral("RX"), QStringLiteral("StatusKeyRx")));
    addGap(8);
    m_rxStatus = makeFigure(zeroBytes, QStringLiteral("RxStatus"));
    addCentered(m_rxStatus);
    addGap(10);
    m_rxRate = makeFigure(zeroRate, QStringLiteral("StatusRate"));
    addCentered(m_rxRate);
    addSep();
    addCentered(makeKey(QStringLiteral("TX"), QStringLiteral("StatusKeyTx")));
    addGap(8);
    m_txStatus = makeFigure(zeroBytes, QStringLiteral("TxStatus"));
    addCentered(m_txStatus);
    addGap(10);
    m_txRate = makeFigure(zeroRate, QStringLiteral("StatusRate"));
    addCentered(m_txRate);
    // 没有丢弃时整组隐藏，分隔和间距也不占位。
    m_dropGroup = new QWidget;
    m_dropGroup->setObjectName(QStringLiteral("DropGroup"));
    auto *dropLayout = new QHBoxLayout(m_dropGroup);
    dropLayout->setContentsMargins(12, 0, 0, 0);
    dropLayout->setSpacing(0);
    auto *dropSep = makeStatusSeparator();
    dropLayout->addWidget(dropSep, 0, Qt::AlignVCenter);
    dropLayout->addSpacing(12);
    auto *dropKey = makeKey(tr("丢弃"), QStringLiteral("StatusKeyDrop"));
    dropKey->setToolTip(tr("读出失败、写出被拒绝或部分写出时扔掉的字节。为 0 时不显示。"));
    dropLayout->addWidget(dropKey, 0, Qt::AlignVCenter);
    dropLayout->addSpacing(8);
    m_dropStatus = makeFigure(zeroBytes, QStringLiteral("DropStatus"));
    dropLayout->addWidget(m_dropStatus, 0, Qt::AlignVCenter);
    m_dropGroup->hide();
    addCentered(m_dropGroup);
    addSep();
    m_trafficStatus = new QLabel(tr("共 0 条"));
    m_trafficStatus->setObjectName(QStringLiteral("TrafficStatus"));
    m_trafficStatus->setAlignment(Qt::AlignVCenter);
    addCentered(m_trafficStatus);
    addSep();
    addCentered(makeKey(tr("桥接"), QStringLiteral("StatusKey")));
    addGap(8);
    m_routeStatus = new QLabel(tr("未启动"));
    m_routeStatus->setObjectName(QStringLiteral("RouteStatus"));
    m_routeStatus->setAlignment(Qt::AlignVCenter);
    addCentered(m_routeStatus);

    // 弹簧把连接状态和动作顶到右边。临时提示插在弹簧和状态块之间，隐藏时不占位。
    auto *spring = new QWidget;
    spring->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    spring->setMinimumWidth(16);
    addCentered(spring, 1);
    m_statusNote = new QLabel;
    // 语义身份而不是样式名：工作区里有多处标签共用 "Muted"，拿它定位是脆的。
    // 样式不丢——QSS 里 `QLabel#Muted, QLabel#StatusNote` 是同一条规则（见 styles/*.qss）。
    m_statusNote->setObjectName(QStringLiteral("StatusNote"));
    // 左对齐：右对齐时超长会先裁掉左边，地址被切成「!7.0.0.1」。
    // 短句按内容宽度显示。上限只防异常长句把右侧状态块挤出窗口。
    m_statusNote->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_statusNote->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    m_statusNote->setMaximumWidth(280);
    m_statusNote->hide();
    addGap(12);
    addCentered(m_statusNote);

    for (int index = 0; index < 2; ++index) {
        addGap(index == 0 ? 12 : 6);
        auto *chip = new QWidget;
        chip->setObjectName(QStringLiteral("EndpointChip"));
        chip->setFixedHeight(StatusControlHeight);
        auto *chipLayout = new QHBoxLayout(chip);
        chipLayout->setContentsMargins(6, 0, 6, 0);
        chipLayout->setSpacing(6);
        auto *dot = new QFrame;
        dot->setObjectName(QStringLiteral("StatusDot"));
        dot->setFrameShape(QFrame::NoFrame);
        dot->setFixedSize(6, 6);
        dot->setProperty("state", QStringLiteral("closed"));
        m_endpointDot[index] = dot;
        auto *name = new QLabel(QString(QLatin1Char('A' + char(index))));
        name->setObjectName(QStringLiteral("EndpointName"));
        name->setAlignment(Qt::AlignVCenter);
        auto *state = new QLabel(tr("未连接"));
        state->setObjectName(QStringLiteral("EndpointState"));
        state->setAlignment(Qt::AlignVCenter);
        m_endpointState[index] = state;
        chipLayout->addWidget(dot, 0, Qt::AlignVCenter);
        chipLayout->addWidget(name, 0, Qt::AlignVCenter);
        chipLayout->addWidget(state, 0, Qt::AlignVCenter);
        addCentered(chip);
    }

    addGap(6);

    // 检查更新、外观和置顶各占一块浅底，高度、圆角与端点状态一致。
    // 合在同一块里时，文字和 12px 图标贴在一起，点的时候分不清点到哪一个。
    const auto prepareAction = [](QToolButton *button) {
        button->setFixedHeight(StatusControlHeight - 2);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
    };
    const auto makeActionChip = [&](const QString &name, int left, int right) {
        auto *chip = new QWidget;
        chip->setObjectName(name);
        chip->setFixedHeight(StatusControlHeight);
        auto *layout = new QHBoxLayout(chip);
        layout->setContentsMargins(left, 0, right, 0);
        layout->setSpacing(0);
        return chip;
    };
#ifdef Q_OS_WIN
    // 没有独立设置页。检查更新放在状态栏，和置顶一样是窗口级动作。
    auto *updateChip = makeActionChip(QStringLiteral("StatusActions"), 6, 6);
    auto *checkUpdate = new QToolButton;
    checkUpdate->setObjectName(QStringLiteral("checkUpdate"));
    checkUpdate->setText(tr("检查更新"));
    checkUpdate->setToolTip(tr("检查更新（当前 %1）").arg(QStringLiteral(QQCOM_VERSION_STR)));
    checkUpdate->setAccessibleName(tr("检查更新"));
    checkUpdate->setToolButtonStyle(Qt::ToolButtonTextOnly);
    prepareAction(checkUpdate);
    updateChip->layout()->addWidget(checkUpdate);
    addCentered(updateChip);
    connect(checkUpdate, &QToolButton::clicked, this, &MainWindow::updateCheckRequested);
    addGap(6);
#endif
    auto *schemeChip = makeActionChip(QStringLiteral("StatusScheme"), 1, 1);
    m_colorScheme = new QToolButton;
    m_colorScheme->setObjectName(QStringLiteral("colorScheme"));
    m_colorScheme->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_colorScheme->setIconSize(QSize(12, 12));
    m_colorScheme->setFixedSize(StatusControlHeight - 2, StatusControlHeight - 2);
    m_colorScheme->setPopupMode(QToolButton::InstantPopup);
    m_colorScheme->setToolTip(tr("外观"));
    m_colorScheme->setAccessibleName(tr("外观"));
    prepareAction(m_colorScheme);
    // 三选一用普通菜单：点一项就切换并收起。勾选保持打开的菜单只翻转勾选，不发 triggered。
    auto *schemeMenu = new QMenu(m_colorScheme);
    schemeMenu->setObjectName(QStringLiteral("colorSchemeMenu"));
    auto *schemeGroup = new QActionGroup(schemeMenu);
    schemeGroup->setExclusive(true);
    const QString schemeLabels[3] = {tr("亮色"), tr("暗色"), tr("跟随系统")};
    const QString schemeKeys[3] = {QStringLiteral("light"), QStringLiteral("dark"),
        QStringLiteral("system")};
    for (int index = 0; index < 3; ++index) {
        auto *action = schemeMenu->addAction(schemeLabels[index]);
        action->setObjectName(QStringLiteral("colorScheme_") + schemeKeys[index]);
        action->setCheckable(true);
        action->setData(schemeKeys[index]);
        schemeGroup->addAction(action);
        m_schemeActions[index] = action;
        connect(action, &QAction::triggered, this, [this, key = schemeKeys[index]] {
            setColorScheme(key, true);
        });
    }
    m_colorScheme->setMenu(schemeMenu);
    schemeChip->layout()->addWidget(m_colorScheme);
    addCentered(schemeChip);
    // 存过就按保存的来。没存过只把勾对准当前模式，不写入：
    // 正式程序此时是跟随系统，测试进程会事先钉在亮色上。写入会把那一档当成用户的选择。
    if (preferences->contains(QStringLiteral("window/colorScheme")))
        setColorScheme(preferences->value(QStringLiteral("window/colorScheme")).toString(), true);
    else
        setColorScheme(AppTheme::modeKey(AppTheme::mode()), false);
    addGap(6);
    auto *pinChip = makeActionChip(QStringLiteral("StatusPin"), 1, 1);
    m_stayOnTop = new QToolButton;
    m_stayOnTop->setObjectName(QStringLiteral("stayOnTop"));
    m_stayOnTop->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_stayOnTop->setIconSize(QSize(12, 12));
    m_stayOnTop->setFixedSize(StatusControlHeight - 2, StatusControlHeight - 2);
    m_stayOnTop->setCheckable(true);
    m_stayOnTop->setToolTip(tr("窗口置顶"));
    m_stayOnTop->setAccessibleName(tr("置顶"));
    prepareAction(m_stayOnTop);
    pinChip->layout()->addWidget(m_stayOnTop);
    addCentered(pinChip);
    connect(m_stayOnTop, &QToolButton::toggled, this, &MainWindow::setStayOnTop);
    connect(AppTheme::instance(), &AppTheme::schemeChanged, this, [this] {
        if (m_stayOnTop) setStayOnTop(m_stayOnTop->isChecked());
    });
    if (preferences->value(QStringLiteral("window/stayOnTop"), false).toBool())
        m_stayOnTop->setChecked(true);
    else
        setStayOnTop(false);

    ui->statusbar->addWidget(status, 1);
    setupAutomation(ui->statusbar);

    // 通信运行时：对象搬进 I/O 线程，GUI 只通过队列信号与它往来。
    // 线程结束时运行时在 I/O 线程内自我销毁，串口/socket 的收尾不落在 GUI。
    m_runtime = new EndpointRuntime;
    m_ioThread = new QThread(this);
    m_runtime->moveToThread(m_ioThread);
    connect(m_ioThread, &QThread::finished, m_runtime, &QObject::deleteLater);
    m_ioThread->start();
    m_sender->setRuntime(m_runtime);
    connect(m_runtime, &EndpointRuntime::endpointStateChanged, this, &MainWindow::handleEndpointState);
    connect(m_runtime, &EndpointRuntime::endpointPeersChanged, this, &MainWindow::handleEndpointPeers);
    connect(m_runtime, &EndpointRuntime::rawDataReceived, m_sender,
        &SendPanel::handleRawDataReceived);
    connect(m_runtime, &EndpointRuntime::messageReceived, this, &MainWindow::handleMessage);
    connect(m_runtime, &EndpointRuntime::endpointIssueReported, this,
        [this](int index, int issue, const QString &reason) {
            if (index < 0 || index >= EndpointRuntime::EndpointCount || m_alertsSuppressed[index]) return;
            m_endpointAlerts[index]->report(Traffic::Issue(issue), reason);
        });

    for (int index = 0; index < m_selectors.size(); ++index) {
        connect(m_selectors.at(index), &PortSelectorWidget::actionRequested, this,
            [this, index] { toggleEndpoint(index); });
    }

    connect(m_sender, &SendPanel::sendRequested, this,
        [this](const QByteArray &payload, int targetMask, bool scheduled) {
            // 手动与定时走**同一条发送路径**，但来源标记不同：定时的一拍标 Scheduled，
            // 屏幕上显示为 `TX+TIMER` 而不是 `TX+MAN`（见 TrafficTextView 的前缀拼装）。
            // 这个参数以前被 lambda 丢掉了——形参只声明了两个，于是定时发送的每一拍
            // 都被记成"手动"，用户想区分"我点的"和"机器按期发的"就无从看起。
            const int origin = int(scheduled ? Traffic::Origin::Scheduled
                                             : Traffic::Origin::Manual);
            for (int index = 0; index < EndpointRuntime::EndpointCount; ++index)
                if (targetMask & (1 << index))
                    QMetaObject::invokeMethod(m_runtime, "sendPayload", Qt::QueuedConnection,
                        Q_ARG(int, index), Q_ARG(QByteArray, payload), Q_ARG(int, origin));
        });
    connect(m_bridge, &RouteEditorWidget::bridgeChanged, this, [this](const BridgeLink &link) {
        m_routeStatus->setText(link.enabled ? tr("已建立") : tr("未启动"));
        restyleProperty(m_routeStatus, "active", link.enabled);
        QMetaObject::invokeMethod(m_runtime, "setBridge", Qt::QueuedConnection,
            Q_ARG(BridgeLink, link), Q_ARG(PortDraft, m_ports.value(0)), Q_ARG(PortDraft, m_ports.value(1)));
    });

    // 速率节拍：与串口扫描一样只在窗口真实显示时运行。
    m_rateTimer = new QTimer(this);
    m_rateTimer->setInterval(RateTickMs);
    connect(m_rateTimer, &QTimer::timeout, this, &MainWindow::updateRateLabels);
    refreshEndpoints();

    // 内容铺开要多宽，只留给「展开指令面板」当没有快照时的起步宽度。
    // 不挂到控件上，也不写进窗口：挂上之后 Qt 会把这个数当成边框拖动的硬下限。
    // 左列自己的 minimumSizeHint 宽度已经是 0（见 LeftColumn），这里改问布局。
    m_trafficMinimumWidth = dataArea->minimumSizeHint().width();
    m_leftColumnMinimumWidth = leftColumn->layout()
        ? leftColumn->layout()->totalMinimumSize().width()
        : 0;
    releaseWindowSizeFloor();
}

void MainWindow::restoreWindowState()
{
    if (!m_preferences) return;
    const QByteArray geometry = m_preferences->value(QStringLiteral("window/geometry")).toByteArray();
    // 空值（首次启动，或用户从未退出过）就保持构造时的默认尺寸。
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    // restoreGeometry 在高 DPI / 离屏窗口上会把逻辑宽度折半。宽高另存一份逻辑像素，
    // 对不上就改回来。最大化时不能 resize，否则会退出最大化。
    if (isMaximized() || isFullScreen()) return;
    const int savedWidth = m_preferences->value(QStringLiteral("window/width")).toInt();
    const int savedHeight = m_preferences->value(QStringLiteral("window/height")).toInt();
    if (savedWidth > 0 && savedHeight > 0
        && (qAbs(width() - savedWidth) > 4 || qAbs(height() - savedHeight) > 4))
        resize(savedWidth, savedHeight);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 几何在**退出时**落盘一次即可：它是低频的用户意图，不需要在每次 resize 里写盘。
    // 存的是 saveGeometry() 的完整状态（大小 + 位置 + 最大化），不只是宽高。
    if (m_preferences) {
        m_preferences->setValue(QStringLiteral("window/geometry"), saveGeometry());
        if (!isMaximized() && !isFullScreen()) {
            m_preferences->setValue(QStringLiteral("window/width"), width());
            m_preferences->setValue(QStringLiteral("window/height"), height());
        }
    }
    QMainWindow::closeEvent(event);
}

MainWindow::~MainWindow()
{
    if (m_controlServer) m_controlServer->stop();
    for (auto *selector : m_selectors) selector->commitAddressHistory();
    // 先停 I/O 线程再拆 UI：运行时析构在 I/O 线程内完成，端口关闭
    // 不需要 GUI 参与，也就不会出现退出时的竞态。
    if (m_ioThread) {
        QMetaObject::invokeMethod(m_runtime, &EndpointRuntime::shutdown, Qt::BlockingQueuedConnection);
        m_ioThread->quit();
        m_ioThread->wait();
        // I/O 已停止，处理关闭前排队的尾帧，再让监视面板结束日志记录。
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    }
    if (m_alertInputFilterInstalled) qApp->removeEventFilter(this);
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
    // 已经连上时，完整的组帧改动直接下发。地址和端口仍要断开再连。
    // 结束符还没写完时控件自己弹窗并退回，这里不再把半截规则送进运行时。
    if (!m_runtime) return;
    for (int index = 0; index < m_selectors.size() && index < EndpointRuntime::EndpointCount; ++index) {
        if (m_endpointStates[index] == Transport::State::Closed) continue;
        if (!m_selectors.at(index)->canApplyFraming()) continue;
        QMetaObject::invokeMethod(m_runtime, &EndpointRuntime::updateFraming, Qt::QueuedConnection,
            index, m_ports[index].framing);
    }
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
    m_endpointAlerts[index]->acknowledge();
    // Closed → 打开；Opening/Open → 关闭（Opening 时即为取消连接）。
    // 正在等待自动恢复时按钮是「等待中」：取消本次恢复，勾选本身保留。
    // 取消必须立刻挡住已经排队的重试，不能等队列槽；否则占用中的重试失败
    // 会把「等待中」马上又点亮。
    if (m_endpointStates[index] != Transport::State::Closed || m_recoverArmed[index]) {
        m_alertsSuppressed[index] = true;
        m_endpointAlerts[index]->stopByUser();
        m_selectors.at(index)->setOpenFailure({});
        m_recoverArmed[index] = false;
        if (m_recoverTimer && !m_recoverArmed[0] && !m_recoverArmed[1])
            m_recoverTimer->stop();
        m_selectors.at(index)->setRecovering(false);
        if (m_endpointStates[index] != Transport::State::Closed) {
            m_userStop[index] = true;
            m_recoverCancelled[index] = false;
            QMetaObject::invokeMethod(m_runtime, "closeEndpoint", Qt::QueuedConnection, Q_ARG(int, index));
        } else {
            m_userStop[index] = false;
            m_recoverCancelled[index] = true;
            if (m_runtime) m_runtime->suppressRecover(index);
            showStatusNote(tr("已取消自动恢复"), 3000);
        }
        return;
    }
    m_userStop[index] = false;
    m_recoverCancelled[index] = false;
    const auto draft = m_selectors.at(index)->draft();
    if (!m_selectors.at(index)->hasValidConfiguration()) return;
    m_endpointAlerts[index]->beginUserAttempt();
    m_alertsSuppressed[index] = false;
    QMetaObject::invokeMethod(m_runtime, "openEndpoint", Qt::QueuedConnection,
        Q_ARG(int, index), Q_ARG(PortDraft, draft));
}

void MainWindow::recoverEndpoints()
{
    if (!m_runtime) return;
    bool waiting = false;
    for (int index = 0; index < m_selectors.size() && index < EndpointRuntime::EndpointCount; ++index) {
        if (!m_recoverArmed[index] || m_endpointStates[index] != Transport::State::Closed)
            continue;
        const auto draft = m_selectors.at(index)->draft();
        if (!draft.autoRecover || draft.kind == PortDraft::TcpServer) {
            m_recoverArmed[index] = false;
            m_selectors.at(index)->setRecovering(false);
            continue;
        }
        if (!m_selectors.at(index)->hasValidConfiguration()) {
            waiting = true;
            continue;
        }
        waiting = true;
        // 断线瞬间的扫描快照可能还列着刚拔掉的口。先等一次新扫描，再决定打不打开。
        if (draft.kind == PortDraft::Serial
            && m_serialScanGeneration <= m_serialRecoverAfterScan[index])
            continue;
        const bool present = draft.kind != PortDraft::Serial
            || m_presentSerialPorts.contains(draft.values.value(QStringLiteral("serial.port")).toString());
        if (draft.kind == PortDraft::Serial && !present)
            continue;
        // 用成员函数指针，避免按名字查找槽时因参数元类型对不上而静默丢弃这一拍。
        QMetaObject::invokeMethod(m_runtime, &EndpointRuntime::recoverEndpoint, Qt::QueuedConnection,
            index, draft, present);
    }
    const bool visible = isVisible() && !testAttribute(Qt::WA_DontShowOnScreen);
    if (!m_recoverTimer) return;
    if (waiting && visible) {
        if (!m_recoverTimer->isActive()) m_recoverTimer->start();
    } else {
        m_recoverTimer->stop();
    }
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
    Q_UNUSED(detail);
    if (index < 0 || index >= m_selectors.size()) return;
    m_endpointStates[index] = Transport::State(state);
    const bool opened = state == int(Transport::State::Open);
    const bool opening = state == int(Transport::State::Opening);
    m_endpointAlerts[index]->setConnectionOpen(opened);
    if (opened) {
        m_recoverArmed[index] = false;
        m_recoverCancelled[index] = false;
        m_userStop[index] = false;
    } else if (opening) {
        // 这一拍正在拨号。恢复标志先留着：失败后还要继续等，按钮也不该在
        // 「等待中」和「正在连接」之间来回跳。
        m_userStop[index] = false;
    } else if (m_userStop[index] || m_recoverCancelled[index]) {
        // 用户已经取消这次等待。在途的重试失败还会报到这里，不能再重新武装。
        m_recoverArmed[index] = false;
        m_userStop[index] = false;
    } else if (m_ports.value(index).autoRecover
        && m_ports.value(index).kind != PortDraft::TcpServer
        && !m_recoverArmed[index]) {
        m_recoverArmed[index] = true;
        if (m_ports.value(index).kind == PortDraft::Serial) {
            m_serialRecoverAfterScan[index] = m_serialScanGeneration;
            if (m_discovery) m_discovery->refresh();
        }
    }
    m_selectors.at(index)->setConnectionOpen(opened, opening);
    m_selectors.at(index)->setRecovering(m_recoverArmed[index] && !opened);
    // ★ 端点状态一变就必须喂给发送区：发送按钮的可用性=「选中目标里至少有一个已连接」，
    // 而它读的是 SendPanel::m_openFlags。这条线曾经**根本没接**——setEndpointOpen()
    // 全项目没有任何调用点，于是 m_openFlags 恒为 false、发送按钮**永远禁用**，
    // 「发送」看上去就是个摆设（用户反馈"实现发送区的发送功能"）。
    // 与"连接状态只由发送按钮外观 + tooltip 承载"配套：这里是那外观的唯一驱动源。
    if (m_sender) m_sender->setEndpointOpen(index, state == int(Transport::State::Open));
    updateEndpointStatus();
    recoverEndpoints();
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
    } else if (message.direction == Traffic::Direction::Error && message.rawLength > 0) {
        // 错误行的 rawLength 只在确实扔掉字节时填写。打开失败等没有载荷的错误保持 0。
        m_droppedBytes += message.rawLength;
        if (m_dropStatus) {
            m_dropStatus->setText(formatBytes(m_droppedBytes));
            m_dropStatus->setToolTip(tr("已丢弃 %1").arg(formatBytes(m_droppedBytes)));
        }
        if (m_dropGroup) m_dropGroup->setVisible(true);
    }
    // 错误播报走状态栏右端的临时提示格（showStatusNote），**不是** showMessage：
    // 后者会隐藏所有 addWidget 挂的普通部件，把 RX/TX/条数统计整行顶掉——而打开失败
    // 常常连续发生，用户看到的正是"统计数据消失、很久才回来"（见 showStatusNote 说明）。
    if (message.direction == Traffic::Direction::Error && !message.statusText.isEmpty()) {
        // 底部用短句。全文留在悬停、数据区和日志里。
        const QString note = message.statusBrief.isEmpty() ? message.statusText : message.statusBrief;
        showStatusNote(note, 5000, true, message.statusText);
        const int index = message.source.endpointIndex;
        // 监听/连接失败时状态仍是关闭，按钮文字不变。纯数据也不画错误行。
        // 已经处于打开中的运行时错误（例如客户端断开）不写到端口行上，避免把正在听的那一行盖住。
        if (message.issue == Traffic::Issue::EndpointFailure
            && index >= 0 && index < m_selectors.size() && !m_alertsSuppressed[index]
            && m_endpointStates[index] != Transport::State::Open)
            m_selectors.at(index)->setOpenFailure(message.statusText);
    }
    m_traffic->appendMessage(message);
}

void MainWindow::updateEndpointAlerts()
{
    int level = EndpointAlertState::Normal;
    bool bright = true;
    bool blinking = false;
    for (int index = 0; index < EndpointRuntime::EndpointCount; ++index) {
        const auto *alerts = m_endpointAlerts[index];
        if (!alerts) continue;
        if (alerts->level() > level) {
            level = alerts->level();
            bright = alerts->bright();
            blinking = alerts->blinking();
        } else if (alerts->level() == level) {
            // 同级别的新告警仍能闪烁，另一个端点的常亮告警不盖住它。
            if (alerts->blinking()) {
                bright = alerts->bright();
                blinking = true;
            } else if (!blinking) {
                bright = true;
            }
        }
    }
    if (level != m_windowAlertLevel || bright != m_windowAlertBright) {
        setWindowIcon(level == EndpointAlertState::Normal ? m_normalWindowIcon
            : m_alertWindowIcons[level - 1][bright ? 1 : 0]);
        m_windowAlertLevel = level;
        m_windowAlertBright = bright;
    }
    if (level == EndpointAlertState::Normal) stopAlertAttention();
    const bool needsInput = level != EndpointAlertState::Normal;
    if (needsInput != m_alertInputFilterInstalled) {
        m_alertInputFilterInstalled = needsInput;
        if (needsInput) qApp->installEventFilter(this);
        else qApp->removeEventFilter(this);
    }
    updateEndpointStatus();
}

void MainWindow::acknowledgeEndpointAlerts()
{
    for (auto *alerts : m_endpointAlerts)
        if (alerts && alerts->level() != EndpointAlertState::Normal)
            alerts->acknowledge();
    stopAlertAttention();
}

void MainWindow::requestAlertAttention()
{
    if (isActiveWindow() || !isVisible() || testAttribute(Qt::WA_DontShowOnScreen)) return;
#if defined(Q_OS_WIN)
    FLASHWINFO info = {};
    info.cbSize = sizeof(info);
    info.hwnd = reinterpret_cast<HWND>(winId());
    info.dwFlags = FLASHW_TRAY;
    info.uCount = 3;
    info.dwTimeout = 500;
    FlashWindowEx(&info);
#else
    QApplication::alert(this, 5000);
#endif
}

void MainWindow::stopAlertAttention()
{
#if defined(Q_OS_WIN)
    if (!internalWinId()) return;
    FLASHWINFO info = {};
    info.cbSize = sizeof(info);
    info.hwnd = reinterpret_cast<HWND>(internalWinId());
    info.dwFlags = FLASHW_STOP;
    FlashWindowEx(&info);
#endif
}

void MainWindow::syncExtensionPanel(bool expanded)
{
    if (!m_horizontalSplitter || !m_commandPanel) return;
    const bool attached = m_horizontalSplitter->indexOf(m_commandPanel) >= 0;
    if (expanded) {
        if (!attached)
            m_horizontalSplitter->addWidget(m_commandPanel);
        // 收起时 hide() 过。addWidget 不会显示被显式隐藏的控件，隐藏控件在分栏里宽度是 0。
        m_commandPanel->show();
        m_horizontalSplitter->setStretchFactor(0, 1);
        m_horizontalSplitter->setStretchFactor(1, 0);
        return;
    }
    // 移出后父对象仍是主窗口，面板不会被销毁。分栏只剩左列时，Qt 不再创建中间手柄。
    if (attached)
        m_commandPanel->setParent(this);
    m_commandPanel->hide();
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

QSize MainWindow::minimumSizeHint() const
{
    // 顶层窗口的有效下限是 minimumSize() 与 minimumSizeHint() 里较大的那个。
    // 内容再宽，也不拿来挡住手动拖边框。
    return QSize(0, 0);
}

bool MainWindow::event(QEvent *event)
{
    const bool handled = QMainWindow::event(event);
    if (event->type() == QEvent::WindowActivate) acknowledgeEndpointAlerts();
    // activate() 在 LayoutRequest 里跑。默认约束会在那里把内容下限写回窗口，
    // 写完这一下边框就又拖不动了。这里在它返回后把下限清掉。
    if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show) {
        const bool constrained = layout()
            && layout()->sizeConstraint() != QLayout::SetNoConstraint;
        if (constrained || minimumWidth() > 0 || minimumHeight() > 0)
            releaseWindowSizeFloor();
    }
    return handled;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress) {
        const auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && (widget == this || isAncestorOf(widget)))
            acknowledgeEndpointAlerts();
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::releaseWindowSizeFloor()
{
    setMinimumSize(0, 0);
    // SetDefaultConstraint 会在每次布局时把内容下限写回窗口，拖小会被弹回去。
    if (QLayout *page = layout())
        page->setSizeConstraint(QLayout::SetNoConstraint);
    if (ui && ui->centralwidget)
        if (QLayout *page = ui->centralwidget->layout())
            page->setSizeConstraint(QLayout::SetNoConstraint);
}

void MainWindow::setColorScheme(const QString &key, bool persist)
{
    const AppTheme::Mode mode = AppTheme::modeFromKey(key);
    const QString stored = AppTheme::modeKey(mode);
    // 已经是这一档就只同步勾选和图标。再 setMode 会让「跟随系统」把手动钉住的配色放开。
    if (AppTheme::mode() != mode)
        AppTheme::setMode(mode);
    for (auto *action : m_schemeActions) {
        if (!action) continue;
        const bool selected = action->data().toString() == stored;
        if (action->isChecked() != selected) action->setChecked(selected);
    }
    if (m_colorScheme) {
        const QColor color = AppTheme::isDark() ? QColor(0xe8, 0xee, 0xfb) : QColor(0x14, 0x32, 0x6f);
        m_colorScheme->setIcon(tintedIcon(QStringLiteral(":/icons/theme.svg"), color, 12));
        const QString label = mode == AppTheme::Mode::Light ? tr("亮色")
            : mode == AppTheme::Mode::Dark ? tr("暗色")
                                            : tr("跟随系统");
        m_colorScheme->setToolTip(tr("外观：%1").arg(label));
    }
    if (persist && m_preferences
        && m_preferences->value(QStringLiteral("window/colorScheme")).toString() != stored)
        m_preferences->setValue(QStringLiteral("window/colorScheme"), stored);
}

void MainWindow::setStayOnTop(bool on)
{
    if (m_stayOnTop) {
        // 图钉：未置顶是空心，置顶后填实。颜色跟当前主题状态栏的静音字和选中字。
        const QColor color = AppTheme::isDark()
            ? (on ? QColor(0xe8, 0xee, 0xfb) : QColor(0x8b, 0x95, 0xa8))
            : (on ? QColor(0x14, 0x32, 0x6f) : QColor(0x64, 0x75, 0x90));
        m_stayOnTop->setIcon(tintedIcon(on ? QStringLiteral(":/icons/pin.svg")
                                           : QStringLiteral(":/icons/pin-outline.svg"),
            color, 12));
    }
    if (windowFlags().testFlag(Qt::WindowStaysOnTopHint) != on) {
        // 不用 setWindowFlag：它会拆掉原生窗口再新建。高 DPI 下这一下之后
        // saveGeometry 会把宽度记成一半，下次启动窗口就缩了一截。
        Qt::WindowFlags flags = windowFlags();
        flags.setFlag(Qt::WindowStaysOnTopHint, on);
        overrideWindowFlags(flags);
#if defined(Q_OS_WIN)
        if (!testAttribute(Qt::WA_DontShowOnScreen)) {
            if (HWND hwnd = reinterpret_cast<HWND>(winId())) {
                ::SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
#endif
    }
    if (m_preferences)
        m_preferences->setValue(QStringLiteral("window/stayOnTop"), on);
}

void MainWindow::updateEndpointStatus()
{
    const auto stateText = [this](int index) {
        const int kind = m_ports.value(index).kind;
        switch (m_endpointStates[index]) {
        case Transport::State::Opening:
            if (m_recoverArmed[index]) return tr("等待恢复");
            return tr("连接中");
        case Transport::State::Open:
            if (kind == PortDraft::TcpServer)
                return tr("监听中 · %1 个客户端").arg(m_endpointPeers[index].size());
            if (kind == PortDraft::Udp) {
                const int sources = m_endpointPeers[index].size();
                return sources == 0 ? tr("已绑定")
                                    : tr("已绑定 · %1 个来源").arg(sources);
            }
            return tr("已连接");
        case Transport::State::Closed:
            if (m_recoverArmed[index]) return tr("等待恢复");
            break;
        }
        return tr("未连接");
    };
    for (int index = 0; index < 2; ++index) {
        if (!m_endpointState[index]) continue;
        const QString text = stateText(index);
        m_endpointState[index]->setText(text);
        m_endpointState[index]->setToolTip(
            tr("端点 %1：%2").arg(QString(QLatin1Char('A' + char(index))), text));
        const char *dot = "closed";
        if (m_endpointStates[index] == Transport::State::Opening) dot = "opening";
        else if (m_endpointStates[index] == Transport::State::Open) dot = "open";
        if (m_endpointAlerts[index] && m_endpointAlerts[index]->level() != EndpointAlertState::Normal) {
            dot = m_endpointAlerts[index]->hasFault() ? "error" : "warning";
            m_endpointState[index]->setToolTip(m_endpointAlerts[index]->summary());
        }
        restyleProperty(m_endpointDot[index], "state", QString::fromLatin1(dot));
    }
    // 临时提示自己占一格。计时没结束时不能清掉它——打开失败时，error 播出的原因
    // 会在同一个事件循环里被随后的 stateChanged 跟过来，清掉的话用户来不及看见。
    if (m_statusNoteTimer && m_statusNoteTimer->isActive()) return;
    if (!m_statusNote) return;
    m_statusNote->clear();
    m_statusNote->setToolTip({});
    restyleProperty(m_statusNote, "tone", QString());
    m_statusNote->hide();
}

void MainWindow::showStatusNote(const QString &text, int ms, bool alert, const QString &detail)
{
    if (!m_statusNote) return;
    if (!m_statusNoteTimer) {
        m_statusNoteTimer = new QTimer(this);
        m_statusNoteTimer->setSingleShot(true);
        // 到时清掉提示，并顺手刷新 A/B 状态块。连接文案只在 updateEndpointStatus 里写。
        connect(m_statusNoteTimer, &QTimer::timeout, this, &MainWindow::updateEndpointStatus);
    }
    m_statusNote->setText(text);
    m_statusNote->setToolTip(detail.isEmpty() ? text : detail);
    restyleProperty(m_statusNote, "tone", alert ? QStringLiteral("error") : QString());
    m_statusNote->setVisible(!text.isEmpty());
    m_statusNoteTimer->start(qMax(1, ms));
}

void MainWindow::updateRateLabels()
{
    const auto rxRate = qMax<qint64>(0, m_rxBytes - m_rxBytesAtTick);
    const auto txRate = qMax<qint64>(0, m_txBytes - m_txBytesAtTick);
    m_rxBytesAtTick = m_rxBytes;
    m_txBytesAtTick = m_txBytes;
    m_rxStatus->setText(formatBytes(m_rxBytes));
    m_txStatus->setText(formatBytes(m_txBytes));
    m_rxRate->setText(tr("%1/s").arg(formatBytes(rxRate)));
    m_txRate->setText(tr("%1/s").arg(formatBytes(txRate)));
}

void MainWindow::fitSendPane(int height)
{
    if (!m_dataSplitter || !m_sender || height <= 0) return;
    m_sender->setMinimumHeight(height);
    const int total = qMax(0, m_dataSplitter->height() - m_dataSplitter->handleWidth());
    const int trafficMin = m_traffic ? m_traffic->minimumSizeHint().height() : 80;
    const int sendH = qMin(height, qMax(0, total - trafficMin));
    if (total <= 0) return;
    m_dataSplitter->setSizes({qMax(0, total - sendH), sendH});
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
        // 恢复几何之后布局会再激活一次，默认约束可能把下限写回来。
        releaseWindowSizeFloor();
    }
    // 发送区贴着自身内容高度的分配在几何定型后应用一次——setSizes 接收的是绝对像素；
    // 发送区只取最小高度（正文约三行），剩余全部给数据显示区。
    // 放在几何恢复之后：它要用恢复后的分栏高度算绝对像素。
    if (!m_dataRatioApplied) {
        m_dataRatioApplied = true;
        fitSendPane(m_sender->preferredHeight());
    }
    if (!testAttribute(Qt::WA_DontShowOnScreen)) {
        m_autoRefresh->start();
        m_rateTimer->start();
        recoverEndpoints();
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
    if (m_recoverTimer) m_recoverTimer->stop();
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

