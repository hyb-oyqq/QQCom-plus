#include "routeeditorwidget.h"

#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

RouteEditorWidget::RouteEditorWidget(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(10);
    auto *editor = new QHBoxLayout;
    m_source = new QComboBox;
    m_source->setObjectName("routeSource");
    m_direction = new QComboBox;
    m_direction->addItems({tr("单向  →"), tr("双向  ↔")});
    m_target = new QComboBox;
    m_target->setObjectName("routeTarget");
    m_targetMode = new QComboBox;
    m_targetMode->addItems({tr("指定客户端（待选择）"), tr("全部客户端")});
    m_add = new QPushButton(tr("添加规则"));
    m_add->setObjectName("addRoute");
    editor->addWidget(new QLabel(tr("来源")));
    editor->addWidget(m_source, 1);
    editor->addWidget(m_direction);
    editor->addWidget(new QLabel(tr("目标")));
    editor->addWidget(m_target, 1);
    editor->addWidget(m_targetMode);
    editor->addWidget(m_add);
    layout->addLayout(editor);
    m_hint = new QLabel;
    m_hint->setObjectName("Muted");
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);
    m_table = new QTableWidget(0, 5);
    m_table->setObjectName("routeTable");
    m_table->setHorizontalHeaderLabels({tr("源端口"), tr("方向"), tr("目标端口"), tr("发送目标"), tr("状态")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(32);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(m_table, 1);
    auto *bottom = new QHBoxLayout;
    auto *note = new QLabel(tr("规则保存为本次预览的草稿，尚未转发任何数据。"));
    note->setObjectName("Muted");
    auto *remove = new QPushButton(tr("删除选中"));
    remove->setEnabled(false);
    auto *enable = new QPushButton(tr("启用转发"));
    enable->setObjectName("PrimaryButton");
    enable->setEnabled(false);
    enable->setToolTip(tr("当前为界面预览"));
    bottom->addWidget(note, 1);
    bottom->addWidget(remove);
    bottom->addWidget(enable);
    layout->addLayout(bottom);
    connect(m_source, &QComboBox::currentIndexChanged, this, &RouteEditorWidget::updateEditor);
    connect(m_target, &QComboBox::currentIndexChanged, this, &RouteEditorWidget::updateEditor);
    connect(m_direction, &QComboBox::currentIndexChanged, this, &RouteEditorWidget::updateEditor);
    connect(m_add, &QPushButton::clicked, this, [this] {
        RuleDraft draft{m_source->currentData().toUuid(), m_target->currentData().toUuid(),
                        m_direction->currentIndex() == 1, !m_targetMode->isHidden() ? m_targetMode->currentText() : tr("端口默认目标")};
        for (const auto &rule : m_rules) {
            if (rule.source == draft.source && rule.target == draft.target && rule.duplex == draft.duplex) {
                m_hint->setText(tr("已有相同方向的规则，请编辑或删除原规则。"));
                return;
            }
        }
        if (m_rules.size() >= 128) { m_hint->setText(tr("最多保留 128 条规则草稿")); return; }
        m_rules.append(draft);
        rebuildTable();
        m_table->selectRow(m_rules.size() - 1);
        emit draftCountChanged(m_rules.size());
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this, remove] {
        remove->setEnabled(m_table->currentRow() >= 0);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        if (row < 0 || row >= m_rules.size()) return;
        m_rules.removeAt(row);
        rebuildTable();
        emit draftCountChanged(m_rules.size());
    });
}

void RouteEditorWidget::setPorts(const QList<PortDraft> &ports)
{
    m_ports = ports;
    const auto source = m_source->currentData().toUuid();
    const auto target = m_target->currentData().toUuid();
    {
        const QSignalBlocker sourceBlock(m_source), targetBlock(m_target);
        m_source->clear();
        m_target->clear();
        for (const auto &p : ports) { m_source->addItem(p.name, p.id); m_target->addItem(p.name, p.id); }
        m_source->setCurrentIndex(source.isNull() ? (ports.isEmpty() ? -1 : 0) : m_source->findData(source));
        m_target->setCurrentIndex(target.isNull() ? (ports.size() > 1 ? 1 : -1) : m_target->findData(target));
    }
    updateEditor();
    rebuildTable();
}

void RouteEditorWidget::updateEditor()
{
    const bool valid = m_source->currentIndex() >= 0 && m_target->currentIndex() >= 0
                       && m_source->currentData() != m_target->currentData();
    m_add->setEnabled(valid);
    const int targetIndex = m_target->currentIndex();
    const bool tcpServer = targetIndex >= 0 && m_ports.value(targetIndex).kind == 2;
    m_targetMode->setVisible(tcpServer);
    if (!valid) m_hint->setText(tr("请选择两个不同的端口。"));
    else if (m_ports.value(targetIndex).kind == 3)
        m_hint->setText(tr("流 → UDP 需要分包；建议 1200 字节上限、10 ms 尾部等待。当前仅建立规则草稿。"));
    else if (tcpServer || m_ports.value(m_source->currentIndex()).kind == 2)
        m_hint->setText(tr("TCP 服务器需明确客户端；多个客户端向串口汇聚时，应先选择单一来源。"));
    else m_hint->setText(tr("按原始字节转发；双向桥将由两条单向规则组成。"));
}

void RouteEditorWidget::rebuildTable()
{
    auto name = [this](const QUuid &id) {
        for (const auto &p : m_ports) if (p.id == id) return p.name;
        return tr("端口已删除");
    };
    auto exists = [this](const QUuid &id) {
        for (const auto &p : m_ports) if (p.id == id) return true;
        return false;
    };
    m_table->setRowCount(m_rules.size());
    for (int row = 0; row < m_rules.size(); ++row) {
        const auto &r = m_rules.at(row);
        const QStringList values{name(r.source), r.duplex ? tr("双向 ↔") : tr("单向 →"), name(r.target),
            r.targetMode, exists(r.source) && exists(r.target) ? tr("草稿 · 未启用") : tr("端口失效")};
        for (int column = 0; column < values.size(); ++column)
            m_table->setItem(row, column, new QTableWidgetItem(values.at(column)));
    }
}
