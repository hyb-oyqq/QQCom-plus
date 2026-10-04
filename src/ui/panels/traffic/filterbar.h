#ifndef FILTERBAR_H
#define FILTERBAR_H

#include <QStringList>
#include <QWidget>

class QLineEdit;
class QPushButton;

// 条件条的完整状态：与 TrafficFilter::Options 的内容条件一一对应，不依赖它——
// 条件条只是"收集条件"，不引入反向依赖，也便于单测直接构造。
// 方向与事件开关（rx/tx/states/warnings/errors）**不在这里**：它们常驻顶栏，
// 由 `FilterFlags` 持有（用户钦定：条件条收起来时这排开关仍然要看得见）。
struct FilterBarState
{
    bool caseSensitive = false;
    bool regex = false;
    QStringList keywords;
    QStringList excludes;
};

// 监视区第二行：过滤条件条（关键词 / 排除词 / 匹配选项 / 重新过滤 / 重置）。
//
// 形态约束（用户钦定）：**恒为一行**、宽度铺满、永不换行、不出现横向滚动条；
// 纵向成本就是一行控件高度，数据区不会被它挤动。因此承载形式是"可直接点选的小按钮"
// 而不是"标签 + 下拉框"——一个布尔条件用一个按钮，比下拉框省一半宽度。
//
// 职责边界：这里只做**收集与呈现**条件，一行匹配逻辑都不写；
// 判定唯一实现在 TrafficFilter::Matcher。
class FilterBar : public QWidget
{
    Q_OBJECT
public:
    explicit FilterBar(QWidget *parent = nullptr);

    // 条件快照：与 TrafficFilter::Options 的同名内容条件一一对应。
    FilterBarState state() const;

    // 本行生效的内容条件个数（关键词 / 排除词 / 大小写 / 正则）。
    // 方向与事件开关常驻顶栏，由面板把它们的计数并进来（见 setResetAvailable）。
    int activeConditionCount() const;

    // 恢复条件：内部抑制 changed，避免加载半成品状态时把已存的值覆盖掉。
    void restore(const FilterBarState &state);

    // 一键回到不过滤（重置按钮与外部入口共用）：只清本行的内容条件。
    void clearAll();

    // 顶栏那排方向/事件开关不在本控件内，但"重置"要能把它们一并复位：
    // 面板通过这个入口说明"外面还有条件在用"，否则只关掉 RX 时重置按钮会是灰的。
    void setResetAvailable(bool available);

    // 模型里有没有可回溯的记录：没有就没什么可"重新过滤"的，按钮置灰。
    void setRebuildAvailable(bool available);

    // 键盘入口：Ctrl+F 聚焦关键词输入并全选，替换掉原顶栏搜索框的快捷键位。
    void focusKeywords();

    // 逗号分隔的条件表：去空项、去重、逐项 trim。**不用空格分隔**——
    // HEX 条件本身常含空格（"AA 55"），拿空格当分隔符会把一条条件拆成两条更宽的条件。
    static QStringList splitConditions(const QString &text);

signals:
    // 任一条件变化即发：调用方据此重建 Matcher 并落盘。
    void changed();
    // 点了重置：本行已自行清空，调用方负责把常驻顶栏的开关一并复位。
    void resetRequested();
    // 点了重新过滤：把当前条件应用到已经收到的记录上（显式回溯，见设计稿 §5）。
    void rebuildRequested();

private:
    void notifyChanged();
    void refreshValidation();
    void refreshReset();
    QLineEdit *makeInput(const QString &iconPath, const QString &placeholder, const QString &tip,
        const QString &objectName);
    QPushButton *makeOption(const QString &objectName, const QString &iconPath, const QString &text,
        const QString &tip);

    QLineEdit *m_keywords = nullptr;
    QLineEdit *m_excludes = nullptr;
    QPushButton *m_case = nullptr;
    QPushButton *m_regex = nullptr;
    QPushButton *m_rebuild = nullptr;
    QPushButton *m_reset = nullptr;
    bool m_resetAvailable = false;   // 外部（顶栏方向/事件开关）是否还有生效条件
    bool m_rebuildAvailable = false; // 模型里还有记录可回溯
    bool m_restoring = false;        // 恢复期间抑制 changed
};

#endif
