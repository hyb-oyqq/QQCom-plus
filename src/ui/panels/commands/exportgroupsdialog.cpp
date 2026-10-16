#include "exportgroupsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

ExportGroupsDialog::ExportGroupsDialog(const QList<CommandSet> &sets, QWidget *parent)
    : QDialog(parent), m_allSets(sets)
{
    setWindowTitle(tr("导出分组"));
    setMinimumWidth(400);
    setMinimumHeight(300);
    setupUi();
}

void ExportGroupsDialog::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    mainLayout->addWidget(new QLabel(tr("选择要导出的分组"), this));

    m_selectAll = new QCheckBox(tr("全选"), this);
    mainLayout->addWidget(m_selectAll);

    m_groupList = new QListWidget(this);
    for (const auto &set : m_allSets) {
        auto *item = new QListWidgetItem(set.name.isEmpty() ? tr("未命名分组") : set.name,
                                         m_groupList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    mainLayout->addWidget(m_groupList, 1);

    // 全选默认打开：多数场景就是整份导出，少勾反而容易漏
    connect(m_selectAll, &QCheckBox::toggled, this, &ExportGroupsDialog::onSelectAllToggled);
    m_selectAll->setChecked(true);

    auto *formatLayout = new QHBoxLayout();
    formatLayout->addWidget(new QLabel(tr("格式:"), this));
    m_formatCombo = new QComboBox(this);
    for (CommandFormats::Format format : CommandFormats::allFormats()) {
        m_formatCombo->addItem(CommandFormats::label(format), static_cast<int>(format));
    }
    formatLayout->addWidget(m_formatCombo, 1);
    mainLayout->addLayout(formatLayout);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox->button(QDialogButtonBox::Ok)->setText(tr("导出"));
    mainLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ExportGroupsDialog::onSelectAllToggled(bool checked)
{
    const Qt::CheckState state = checked ? Qt::Checked : Qt::Unchecked;
    for (int i = 0; i < m_groupList->count(); ++i) {
        m_groupList->item(i)->setCheckState(state);
    }
}

QList<CommandSet> ExportGroupsDialog::selectedSets() const
{
    QList<CommandSet> result;
    // 列表行序与传入的分组顺序一致，直接按下标取回完整数据
    for (int i = 0; i < m_groupList->count(); ++i) {
        if (m_groupList->item(i)->checkState() == Qt::Checked) {
            result.append(m_allSets.at(i));
        }
    }
    return result;
}

CommandFormats::Format ExportGroupsDialog::format() const
{
    return static_cast<CommandFormats::Format>(m_formatCombo->currentData().toInt());
}
