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
    applyDialogStyle();
    setupUi();
}

void ExportGroupsDialog::applyDialogStyle()
{
    // 应用与主题一致的对话框样式，与 StyleDialogs 保持统一
    const QString style = R"(
        QDialog {
            background: #ffffff;
            color: #182334;
        }
        QLabel {
            color: #182334;
            font-size: 12px;
        }
        QPushButton {
            background: #f4f6fa;
            border: 1px solid #a9b5c9;
            border-radius: 4px;
            padding: 2px 12px;
            min-height: 16px;
            min-width: 70px;
            color: #1f2a3d;
        }
        QPushButton:hover {
            color: #14326f;
            border-color: #6d87b8;
            background: #e8eefb;
        }
        QPushButton:pressed {
            background: #d7e2f7;
            border-color: #4d6ba3;
        }
        QPushButton:default {
            background: #1d4ed8;
            border-color: #1a45c0;
            color: #ffffff;
            font-size: 12px;
            font-weight: 600;
        }
        QPushButton:default:hover {
            background: #163fb3;
            border-color: #163fb3;
        }
        QComboBox {
            background: #ffffff;
            border: 1px solid #a9b5c9;
            border-radius: 4px;
            padding: 2px 6px;
            min-height: 16px;
        }
        QComboBox:hover {
            border-color: #76869f;
        }
        QComboBox:focus {
            border-color: #1d4ed8;
        }
        QListWidget {
            background: #ffffff;
            border: 1px solid #a9b5c9;
            border-radius: 5px;
        }
        QListWidget::item {
            padding: 4px 8px;
        }
        QListWidget::item:hover {
            background: #e8eefb;
        }
        QListWidget::item:selected {
            background: #d3e0fa;
            color: #14326f;
        }
        QCheckBox {
            spacing: 4px;
            color: #1f2a3d;
        }
        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #8b99b0;
            border-radius: 4px;
            background: #ffffff;
        }
        QCheckBox::indicator:hover {
            border-color: #1d4ed8;
        }
        QCheckBox::indicator:checked {
            background: #1d4ed8;
            border-color: #1a45c0;
        }
    )";

    setStyleSheet(style);
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
