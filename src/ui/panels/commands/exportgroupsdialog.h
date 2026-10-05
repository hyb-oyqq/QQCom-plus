#ifndef EXPORTGROUPSDIALOG_H
#define EXPORTGROUPSDIALOG_H

#include "app/commandformats.h"
#include "domain/commandset.h"

#include <QDialog>
#include <QList>

class QCheckBox;
class QComboBox;
class QListWidget;

// 导出分组对话框：勾选要导出的分组 + 选择目标格式。
// 格式必须显式选：JCom 顶层是数组、blemsg 顶层是对象，两边解析器都按顶层类型硬判别，
// 任何"混合体"都会被两边同时拒绝，所以导出侧做不到一文件兼容多格式（导入侧才嗅探）。
class ExportGroupsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExportGroupsDialog(const QList<CommandSet> &sets, QWidget *parent = nullptr);

    // 勾选到的分组（按原顺序）
    QList<CommandSet> selectedSets() const;
    CommandFormats::Format format() const;

private:
    void setupUi();
    void applyDialogStyle();
    void onSelectAllToggled(bool checked);

    QList<CommandSet> m_allSets;
    QCheckBox *m_selectAll = nullptr;
    QListWidget *m_groupList = nullptr;
    QComboBox *m_formatCombo = nullptr;
};

#endif
