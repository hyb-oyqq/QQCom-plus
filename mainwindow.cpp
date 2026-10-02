#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "version/version.h"
#include "src/ui/panels/routing/routeeditorwidget.h"
#include "src/ui/panels/sending/sendpanel.h"
#include "src/ui/panels/traffic/trafficpanel.h"
#include "src/discovery/portdiscoveryservice.h"
#include "src/persistence/preferencesstore.h"

#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// 串口列表的轮询间隔：设备插拔靠周期扫描发现，服务端会合并重叠的扫描。
constexpr int SerialAutoRefreshMs = 2000;

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
    auto *discovery = new PortDiscoveryService(this);
    const auto reloadHistory = [this, preferences] {
        for (auto *selector : m_selectors)
            selector->setAddressHistory(preferences->addressHistory(false), preferences->addressHistory(true));
    };

    for (const auto kind : {PortDraft::Serial, PortDraft::TcpServer}) {
        const QString label = QString(QChar::fromLatin1('A' + char(m_selectors.size())));
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
    dataSplitter->addWidget(m_traffic);
    dataSplitter->addWidget(m_sender);
    // 显示区承载更多内容：发送区正文固定约三行、贴着自身内容高度，
    // 初始在 showEvent 把它压到最小高度，剩余纵向空间全给显示区（发送区 stretch 为 0，
    // 窗口缩放的增量也都进显示区）；用户仍可拖动分隔条给发送区加高。
    dataSplitter->setStretchFactor(0, 1);
    dataSplitter->setStretchFactor(1, 0);
    m_dataSplitter = dataSplitter;
    auto *dataArea = new QWidget;
    auto *dataLayout = new QVBoxLayout(dataArea);
    dataLayout->setContentsMargins(14, 12, 14, 12);
    dataLayout->addWidget(dataSplitter);
    root->addWidget(dataArea, 1);

    ui->statusbar->setSizeGripEnabled(true);
    auto *status = new QWidget;
    auto *statusLayout = new QHBoxLayout(status);
    statusLayout->setContentsMargins(12, 2, 12, 2);
    statusLayout->setSpacing(22);
    auto *rx = new QLabel(tr("RX   0 B   ·   0 B/s"));
    rx->setObjectName("RxStatus");
    auto *tx = new QLabel(tr("TX   0 B   ·   0 B/s"));
    tx->setObjectName("TxStatus");
    statusLayout->addWidget(rx);
    statusLayout->addWidget(tx);
    m_routeStatus = new QLabel(tr("桥接  未启动"));
    m_routeStatus->setObjectName("RouteStatus");
    statusLayout->addWidget(m_routeStatus);
    statusLayout->addStretch();
    auto *statusNote = new QLabel(tr("所有端口未连接  ·  示例不发送到设备"));
    statusNote->setObjectName("Muted");
    statusLayout->addWidget(statusNote);
    ui->statusbar->addWidget(status, 1);

    connect(m_traffic, &TrafficPanel::reusePayload, m_sender, &SendPanel::setPayload);
    connect(m_bridge, &RouteEditorWidget::draftCountChanged, this, [this](int count) {
        m_routeStatus->setText(count > 0 ? tr("桥接  已建立 · 预览") : tr("桥接  未启动"));
    });
    refreshEndpoints();
}

MainWindow::~MainWindow()
{
    for (auto *selector : m_selectors) selector->commitAddressHistory();
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
}

void MainWindow::loadExample() { m_traffic->loadExample(m_ports); }
void MainWindow::showRoutes() { m_bridge->findChild<QWidget *>("bridgeButton")->setFocus(); }

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
    if (!testAttribute(Qt::WA_DontShowOnScreen)) m_autoRefresh->start();
}

void MainWindow::hideEvent(QHideEvent *event)
{
    QMainWindow::hideEvent(event);
    m_autoRefresh->stop();
}
