#include "sendpanel.h"
#include "payloadpreview.h"
#include "routeeditorwidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

SendPanel::SendPanel(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("Surface");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_tabs = new QTabWidget;
    m_tabs->setObjectName("sendTabs");
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(createSendPage(), tr("发送数据"));
    m_tabs->addTab(createCommandsPage(), tr("快捷命令"));
    m_routes = new RouteEditorWidget;
    m_tabs->addTab(m_routes, tr("转发规则"));
    connect(m_routes, &RouteEditorWidget::draftCountChanged, this, &SendPanel::routeDraftCountChanged);
    layout->addWidget(m_tabs);
    updatePreview();
}

QWidget *SendPanel::createSendPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(10);
    auto *options = new QHBoxLayout;
    m_target = new QComboBox;
    m_target->setObjectName("sendTarget");
    m_target->setMinimumWidth(135);
    m_peer = new QComboBox;
    m_peer->setEnabled(false);
    m_peer->setToolTip(tr("连接后选择发送目标"));
    m_mode = new QComboBox;
    m_mode->setObjectName("sendMode");
    m_mode->addItems({tr("HEX"), tr("文本"), tr("转义字节")});
    m_encoding = new QComboBox;
    m_encoding->setObjectName("sendEncoding");
    m_encoding->addItems({"UTF-8", "Latin-1"});
    m_suffix = new QComboBox;
    m_suffix->setObjectName("sendSuffix");
    m_suffix->addItems({tr("不追加"), "CR", "LF", "CRLF"});
    options->addWidget(new QLabel(tr("目标")));
    options->addWidget(m_target);
    options->addWidget(m_peer, 1);
    options->addWidget(new QLabel(tr("格式")));
    options->addWidget(m_mode);
    options->addWidget(m_encoding);
    options->addWidget(new QLabel(tr("末尾")));
    options->addWidget(m_suffix);
    layout->addLayout(options);

    m_editor = new QPlainTextEdit;
    m_editor->setObjectName("sendEditor");
    auto mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(11);
    m_editor->setFont(mono);
    m_editor->setPlaceholderText(tr("输入待发送内容，例如：01 03 00 00 00 02 C4 0B"));
    m_editor->setPlainText("01 03 00 00 00 02 C4 0B");
    m_editor->setMinimumHeight(65);
    layout->addWidget(m_editor, 1);
    auto *previewRow = new QHBoxLayout;
    m_length = new QLabel;
    m_length->setObjectName("ByteCount");
    m_preview = new QLabel;
    m_preview->setObjectName("payloadPreview");
    m_preview->setMinimumWidth(0);
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_preview->setTextFormat(Qt::PlainText);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    previewRow->addWidget(m_length);
    previewRow->addWidget(m_preview, 1);
    layout->addLayout(previewRow);

    auto *actions = new QHBoxLayout;
    auto *repeat = new QCheckBox(tr("定时发送"));
    repeat->setObjectName("repeatSending");
    auto *interval = new QSpinBox;
    interval->setRange(10, 3600000);
    interval->setValue(1000);
    interval->setSuffix(" ms");
    interval->setEnabled(false);
    auto *times = new QSpinBox;
    times->setRange(0, 999999);
    times->setValue(0);
    times->setSpecialValueText(tr("持续"));
    times->setSuffix(tr(" 次"));
    times->setEnabled(false);
    m_save = new QPushButton(tr("存为命令"));
    m_save->setObjectName("saveCommand");
    auto *send = new QPushButton(tr("发送"));
    send->setObjectName("PrimaryButton");
    send->setMinimumWidth(90);
    send->setEnabled(false);
    send->setToolTip(tr("当前为界面预览，尚未连接端口"));
    actions->addWidget(repeat);
    actions->addWidget(interval);
    actions->addWidget(times);
    actions->addStretch();
    auto *disconnected = new QLabel(tr("端口未连接"));
    disconnected->setObjectName("Muted");
    actions->addWidget(disconnected);
    actions->addWidget(m_save);
    actions->addWidget(send);
    layout->addLayout(actions);

    connect(repeat, &QCheckBox::toggled, interval, &QSpinBox::setEnabled);
    connect(repeat, &QCheckBox::toggled, times, &QSpinBox::setEnabled);
    connect(m_target, &QComboBox::currentIndexChanged, this, &SendPanel::updateTarget);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &SendPanel::updatePreview);
    connect(m_mode, &QComboBox::currentIndexChanged, this, &SendPanel::updatePreview);
    connect(m_encoding, &QComboBox::currentIndexChanged, this, &SendPanel::updatePreview);
    connect(m_suffix, &QComboBox::currentIndexChanged, this, &SendPanel::updatePreview);
    connect(m_save, &QPushButton::clicked, this, [this] {
        if (m_commands.size() >= 512) return;
        bool ok = false;
        auto name = QInputDialog::getText(this, tr("保存快捷命令"), tr("命令名称"), QLineEdit::Normal, tr("自定义命令"), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        m_commands.append({name.left(64), m_editor->toPlainText(), m_mode->currentIndex(), m_encoding->currentIndex(), m_suffix->currentIndex()});
        rebuildCommands();
        m_tabs->setCurrentIndex(1);
        m_commandTable->selectRow(m_commands.size() - 1);
    });
    return page;
}

QWidget *SendPanel::createCommandsPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 12, 16, 12);
    auto *top = new QHBoxLayout;
    auto *label = new QLabel(tr("常用指令，一次配置，重复使用"));
    label->setObjectName("Muted");
    top->addWidget(label, 1);
    auto *load = new QPushButton(tr("载入编辑"));
    auto *up = new QPushButton(tr("上移"));
    auto *down = new QPushButton(tr("下移"));
    auto *remove = new QPushButton(tr("删除"));
    top->addWidget(load);
    top->addWidget(up);
    top->addWidget(down);
    top->addWidget(remove);
    layout->addLayout(top);
    m_commandTable = new QTableWidget(0, 4);
    m_commandTable->setObjectName("commandTable");
    m_commandTable->setHorizontalHeaderLabels({tr("名称"), tr("格式"), tr("内容"), tr("末尾")});
    m_commandTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_commandTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_commandTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_commandTable->setAlternatingRowColors(true);
    m_commandTable->setShowGrid(false);
    m_commandTable->verticalHeader()->hide();
    m_commandTable->verticalHeader()->setDefaultSectionSize(32);
    m_commandTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_commandTable->setColumnWidth(0, 160);
    m_commandTable->setColumnWidth(1, 100);
    m_commandTable->setColumnWidth(3, 100);
    layout->addWidget(m_commandTable, 1);
    auto *tip = new QLabel(tr("双击命令填入发送区；修改后可存为新命令。当前命令仅保留在本次预览。"));
    tip->setObjectName("Muted");
    layout->addWidget(tip);
    m_commands = {{tr("读取寄存器 · 示例"), "01 03 00 00 00 02 C4 0B", 0, 0, 0},
                  {tr("AT 查询 · 示例"), "AT", 1, 0, 3},
                  {tr("换行测试 · 示例"), "hello\\r\\n", 2, 0, 0}};
    rebuildCommands();
    m_commandTable->selectRow(0);
    connect(load, &QPushButton::clicked, this, [this] { loadCommand(m_commandTable->currentRow()); });
    connect(m_commandTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { loadCommand(row); });
    connect(up, &QPushButton::clicked, this, [this] { moveCommand(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveCommand(1); });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_commandTable->currentRow();
        if (row >= 0 && row < m_commands.size()) {
            m_commands.removeAt(row);
            rebuildCommands();
            if (!m_commands.isEmpty()) m_commandTable->selectRow(qMin(row, int(m_commands.size() - 1)));
        }
    });
    return page;
}

void SendPanel::setPorts(const QList<PortDraft> &ports)
{
    m_ports = ports;
    const auto selected = m_target->currentData().toUuid();
    {
        const QSignalBlocker blocker(m_target);
        m_target->clear();
        for (const auto &port : ports) m_target->addItem(port.name, port.id);
        m_target->setCurrentIndex(selected.isNull() ? (ports.isEmpty() ? -1 : 0) : m_target->findData(selected));
    }
    updateTarget();
    m_routes->setPorts(ports);
}

void SendPanel::updateTarget()
{
    m_peer->clear();
    const int index = m_target->currentIndex();
    if (index < 0) { m_peer->addItem(tr("请选择目标端口")); return; }
    const auto &port = m_ports.at(index);
    if (port.kind == 2) m_peer->addItem(tr("暂无已连接客户端"));
    else if (port.kind == 3) m_peer->addItem(port.values.value("udp.target", "127.0.0.1").toString()
                                           + ":" + port.values.value("udp.targetPort", 9002).toString());
    else m_peer->addItem(tr("端口默认目标"));
}

void SendPanel::updatePreview()
{
    m_encoding->setEnabled(m_mode->currentIndex() != 0);
    const auto result = encodePreview(m_editor->toPlainText(), m_mode->currentIndex(), m_encoding->currentIndex(), m_suffix->currentIndex());
    m_length->setText(result.valid() ? tr("%1 字节").arg(result.bytes.size()) : tr("输入错误"));
    const auto text = result.valid() ? (result.bytes.isEmpty() ? tr("最终发送内容将在这里预览")
        : QString(result.bytes.left(64).toHex(' ').toUpper()) + (result.bytes.size() > 64 ? " …" : "")) : result.error;
    m_preview->setText(text);
    m_preview->setToolTip(text);
    m_preview->setStyleSheet(result.valid() ? "color: #738097;" : "color: #c44343;");
    m_save->setEnabled(result.valid() && !result.bytes.isEmpty() && m_commands.size() < 512);
}

void SendPanel::setPayload(const QByteArray &bytes)
{
    m_mode->setCurrentIndex(0);
    m_suffix->setCurrentIndex(0);
    m_editor->setPlainText(bytes.toHex(' ').toUpper());
    m_tabs->setCurrentIndex(0);
    m_editor->setFocus();
}

void SendPanel::rebuildCommands()
{
    m_commandTable->setRowCount(m_commands.size());
    for (int row = 0; row < m_commands.size(); ++row) {
        const auto &c = m_commands.at(row);
        const QStringList values{c.name, QStringList{tr("HEX"), tr("文本"), tr("转义字节")}.value(c.mode),
                                 c.text, QStringList{tr("无"), "CR", "LF", "CRLF"}.value(c.suffix)};
        for (int column = 0; column < values.size(); ++column)
            m_commandTable->setItem(row, column, new QTableWidgetItem(values.at(column)));
    }
}

void SendPanel::loadCommand(int row)
{
    if (row < 0 || row >= m_commands.size()) return;
    const auto &c = m_commands.at(row);
    m_mode->setCurrentIndex(c.mode);
    m_encoding->setCurrentIndex(c.encoding);
    m_suffix->setCurrentIndex(c.suffix);
    m_editor->setPlainText(c.text);
    m_tabs->setCurrentIndex(0);
}

void SendPanel::moveCommand(int offset)
{
    const int row = m_commandTable->currentRow(), next = row + offset;
    if (row < 0 || next < 0 || next >= m_commands.size()) return;
    m_commands.swapItemsAt(row, next);
    rebuildCommands();
    m_commandTable->selectRow(next);
}

void SendPanel::showRoutes() { m_tabs->setCurrentIndex(2); }
