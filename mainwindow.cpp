#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "src/ui/sendpanel.h"
#include "src/ui/trafficpanel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QToolButton>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle(tr("easyDebug · 通信调试工作台"));
    setMinimumSize(1120, 700);
    resize(1440, 920);
    ui->menubar->hide();

    auto *root = new QVBoxLayout(ui->centralwidget);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *header = new QFrame;
    header->setObjectName("AppHeader");
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(22, 14, 22, 14);
    headerLayout->setSpacing(12);
    auto *logo = new QLabel("E/");
    logo->setObjectName("Logo");
    logo->setFixedSize(38, 38);
    logo->setAlignment(Qt::AlignCenter);
    headerLayout->addWidget(logo);
    auto *brand = new QLabel("easyDebug");
    brand->setObjectName("Brand");
    headerLayout->addWidget(brand);
    auto *subtitle = new QLabel(tr("通信调试工作台"));
    subtitle->setObjectName("Muted");
    headerLayout->addWidget(subtitle);
    auto *preview = new QLabel(tr("界面预览"));
    preview->setObjectName("PreviewBadge");
    headerLayout->addWidget(preview);
    headerLayout->addStretch();
    auto *example = new QPushButton(tr("载入示例"));
    example->setObjectName("loadExample");
    auto *stop = new QPushButton(tr("停止全部"));
    stop->setEnabled(false);
    stop->setToolTip(tr("当前没有运行中的发送或转发任务"));
    headerLayout->addWidget(example);
    headerLayout->addWidget(stop);
    root->addWidget(header);

    auto *workspace = new QSplitter(Qt::Horizontal);
    workspace->setObjectName("workspaceSplitter");
    workspace->setChildrenCollapsible(false);
    auto *sidebar = new QFrame;
    sidebar->setObjectName("Sidebar");
    sidebar->setMinimumWidth(266);
    sidebar->setMaximumWidth(390);
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(0);
    auto *sideHeading = new QHBoxLayout;
    sideHeading->setContentsMargins(16, 16, 16, 10);
    auto *portsLabel = new QLabel(tr("连接端口"));
    portsLabel->setObjectName("SectionTitle");
    m_portCount = new QLabel;
    m_portCount->setObjectName("CountBadge");
    sideHeading->addWidget(portsLabel);
    sideHeading->addWidget(m_portCount);
    sideHeading->addStretch();
    auto *add = new QToolButton;
    add->setText(tr("＋ 添加"));
    add->setObjectName("addPort");
    add->setPopupMode(QToolButton::InstantPopup);
    auto *addMenu = new QMenu(add);
    for (int kind = 0; kind < 4; ++kind) {
        auto *action = addMenu->addAction(portKindName(kind));
        connect(action, &QAction::triggered, this, [this, kind] { addPort(kind); });
    }
    add->setMenu(addMenu);
    sideHeading->addWidget(add);
    sideLayout->addLayout(sideHeading);
    m_portList = new QListWidget;
    m_portList->setObjectName("portList");
    m_portList->setFixedHeight(274);
    m_portList->setSpacing(5);
    m_portList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_portList->setContextMenuPolicy(Qt::CustomContextMenu);
    sideLayout->addWidget(m_portList);
    auto *separator = new QFrame;
    separator->setFrameShape(QFrame::HLine);
    separator->setObjectName("Separator");
    sideLayout->addWidget(separator);
    auto *configScroll = new QScrollArea;
    configScroll->setFrameShape(QFrame::NoFrame);
    configScroll->setWidgetResizable(true);
    configScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_selector = new PortSelectorWidget;
    configScroll->setWidget(m_selector);
    sideLayout->addWidget(configScroll, 1);
    workspace->addWidget(sidebar);

    auto *mainArea = new QWidget;
    auto *mainLayout = new QVBoxLayout(mainArea);
    mainLayout->setContentsMargins(18, 16, 18, 16);
    mainLayout->setSpacing(12);
    auto *areaHeading = new QHBoxLayout;
    auto *title = new QLabel(tr("调试工作区"));
    title->setObjectName("WorkspaceTitle");
    areaHeading->addWidget(title);
    areaHeading->addStretch();
    m_selectedTitle = new QLabel;
    m_selectedTitle->setObjectName("Muted");
    areaHeading->addWidget(m_selectedTitle);
    mainLayout->addLayout(areaHeading);
    auto *dataSplitter = new QSplitter(Qt::Vertical);
    dataSplitter->setObjectName("dataSplitter");
    dataSplitter->setChildrenCollapsible(false);
    m_traffic = new TrafficPanel;
    m_traffic->setMinimumHeight(240);
    m_sender = new SendPanel;
    m_sender->setMinimumHeight(265);
    dataSplitter->addWidget(m_traffic);
    dataSplitter->addWidget(m_sender);
    dataSplitter->setStretchFactor(0, 3);
    dataSplitter->setStretchFactor(1, 2);
    dataSplitter->setSizes({450, 300});
    mainLayout->addWidget(dataSplitter, 1);
    workspace->addWidget(mainArea);
    workspace->setStretchFactor(0, 0);
    workspace->setStretchFactor(1, 1);
    workspace->setSizes({290, 1150});
    root->addWidget(workspace, 1);

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
    m_routeStatus = new QLabel(tr("转发  0 条运行"));
    statusLayout->addWidget(m_routeStatus);
    statusLayout->addStretch();
    auto *statusNote = new QLabel(tr("所有端口未连接  ·  示例不发送到设备"));
    statusNote->setObjectName("Muted");
    statusLayout->addWidget(statusNote);
    ui->statusbar->addWidget(status, 1);

    connect(example, &QPushButton::clicked, this, &MainWindow::loadExample);
    connect(m_portList, &QListWidget::currentRowChanged, this, &MainWindow::selectPort);
    connect(m_selector, &PortSelectorWidget::configurationChanged, this, [this] {
        const int row = m_portList->currentRow();
        if (row < 0 || row >= m_ports.size()) return;
        m_ports[row] = m_selector->draft();
        if (m_ports[row].name.trimmed().isEmpty()) m_ports[row].name = tr("未命名端口");
        updatePortList();
        synchronizePorts();
    });
    connect(m_traffic, &TrafficPanel::reusePayload, m_sender, &SendPanel::setPayload);
    connect(m_sender, &SendPanel::routeDraftCountChanged, this, [this](int count) {
        m_routeStatus->setText(tr("转发  0 条运行 / %1 条草稿").arg(count));
    });
    connect(m_portList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        auto *item = m_portList->itemAt(point);
        if (!item) return;
        const int row = m_portList->row(item);
        QMenu menu;
        auto *remove = menu.addAction(tr("删除端口"));
        if (menu.exec(m_portList->viewport()->mapToGlobal(point)) != remove) return;
        m_ports.removeAt(row);
        updatePortList();
        if (!m_ports.isEmpty()) m_portList->setCurrentRow(qMin(row, int(m_ports.size() - 1)));
        selectPort(m_portList->currentRow());
        synchronizePorts();
    });

    addPort(0);
    addPort(2);
    addPort(1);
    addPort(3);
    m_portList->setCurrentRow(0);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::addPort(int kind)
{
    if (m_ports.size() >= 16) {
        ui->statusbar->showMessage(tr("最多可添加 16 个端口"), 3000);
        return;
    }
    PortDraft port;
    const QString label = m_portSequence < 26 ? QString(QChar('A' + m_portSequence)) : QString::number(m_portSequence + 1);
    ++m_portSequence;
    port.name = label + " · " + portKindName(kind);
    port.kind = kind;
    m_ports.append(port);
    updatePortList();
    m_portList->setCurrentRow(m_ports.size() - 1);
    selectPort(m_portList->currentRow());
    synchronizePorts();
}

void MainWindow::updatePortList()
{
    const QSignalBlocker blocker(m_portList);
    const int row = m_portList->currentRow();
    while (m_portList->count() > m_ports.size()) delete m_portList->takeItem(m_portList->count() - 1);
    while (m_portList->count() < m_ports.size()) m_portList->addItem(new QListWidgetItem);
    for (int i = 0; i < m_ports.size(); ++i) {
        auto *item = m_portList->item(i);
        item->setText(m_ports.at(i).name + tr("    ○\n") + portDraftSummary(m_ports.at(i)));
        item->setToolTip(tr("%1\n%2\n未连接 · 右键可删除").arg(m_ports.at(i).name, portDraftSummary(m_ports.at(i))));
        item->setSizeHint(QSize(0, 58));
    }
    m_portList->setCurrentRow(qMin(row, int(m_ports.size() - 1)));
    m_portCount->setText(QString::number(m_ports.size()));
}

void MainWindow::selectPort(int row)
{
    const bool valid = row >= 0 && row < m_ports.size();
    m_selector->setEnabled(valid);
    if (valid) {
        m_selector->setDraft(m_ports.at(row));
        m_ports[row] = m_selector->draft();
    }
    m_selectedTitle->setText(valid ? tr("正在配置  %1").arg(m_ports.at(row).name) : tr("添加端口开始配置"));
}

void MainWindow::synchronizePorts()
{
    m_traffic->setPorts(m_ports);
    m_sender->setPorts(m_ports);
    const int row = m_portList->currentRow();
    m_selectedTitle->setText(row >= 0 ? tr("正在配置  %1").arg(m_ports.at(row).name) : tr("添加端口开始配置"));
}

void MainWindow::loadExample() { m_traffic->loadExample(m_ports); }
void MainWindow::showRoutes() { m_sender->showRoutes(); }
