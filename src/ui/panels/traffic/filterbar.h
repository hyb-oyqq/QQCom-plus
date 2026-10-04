#ifndef FILTERBAR_H
#define FILTERBAR_H

#include <QStringList>
#include <QWidget>

class QAction;
class QLineEdit;
class QPushButton;

// 过滤条的完整状态：与 TrafficFilter::Options 同构（方向/内容/匹配选项），
// 但不依赖它——条件条只是"收集条件"，不引入反向依赖，也便于单测直接构造。
struct FilterBarState
{
    bool rx = true;
    bool tx = true;
    bool states = true;
    bool warnings = true;
    bool errors = true;
    bool caseSensitive = false;
    bool regex = false;
    QStringList keywords;
    QStringList excludes;
};

// 监视区第二行：过滤条件条。
//
// 形态约束（用户钦定）：**恒为一行**、宽度铺满、永不换行、不出现横向滚动条；
// 纵向成本就是一行控件高度，数据区不会被它挤动。因此承载形式是"可直接点选的小按钮"
// 而不是"标签 + 下拉框"——方向五个按钮加起来的宽度比一个下拉框还窄。
//
// 职责边界：这里只做**收集与呈现**条件，一行匹配逻辑都不写；
// 判定唯一实现在 TrafficFilter::Matcher。
class FilterBar : public QWidget
{
    Q_OBJECT
public:
    explicit FilterBar(QWidget *parent = nullptr);

    // 条件快照：与 TrafficFilter::Options 一一对应，由调用方组装。
    FilterBarState state() const;

    // 不等于"全开 / 不限"的维度个数：顶栏折叠按钮用它显示"当前在过滤"。
    int activeConditionCount() const;

    // 恢复条件：内部抑制 changed，避免加载半成品状态时把已存的值覆盖掉。
    void restore(const FilterBarState &state);

    // 一键回到不过滤（重置按钮与外部入口共用）。
    void clearAll();

    // 键盘入口：Ctrl+F 聚焦关键词输入并全选，替换掉原顶栏搜索框的快捷键位。
    void focusKeywords();

    // 空格分隔的词表：去掉空项与重复项，chip 与判定共用同一份解析。
    static QStringList splitWords(const QString &text);

signals:
    // 任一条件变化即发：调用方据此重建 Matcher 并落盘。
    void changed();

private:
    void notifyChanged();
    void applyToggleIcon(QPushButton *button, const QString &iconPath);
    void rebuildKeywordChips();
    void removeKeyword(const QString &keyword);
    void refreshValidation();
    void refreshResetState();
    QLineEdit *makeInput(const QString &iconPath, const QString &placeholder, const QString &tip,
        const QString &objectName);
    QPushButton *makeDirection(const QString &objectName, const QString &iconPath, const QString &tip);

    QLineEdit *m_keywords = nullptr;
    QLineEdit *m_excludes = nullptr;
    QWidget *m_chipHost = nullptr; // 关键词 chip 的容器：紧跟在输入框之后
    QPushButton *m_rx = nullptr;
    QPushButton *m_tx = nullptr;
    QPushButton *m_state = nullptr;
    QPushButton *m_warning = nullptr;
    QPushButton *m_error = nullptr;
    QPushButton *m_more = nullptr;
    QPushButton *m_reset = nullptr;
    QAction *m_caseAction = nullptr;
    QAction *m_regexAction = nullptr;
    bool m_restoring = false; // 恢复期间抑制 changed
};

#endif
