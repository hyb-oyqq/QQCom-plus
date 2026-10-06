#ifndef FILTERBAR_H
#define FILTERBAR_H

#include <QStringList>
#include <QWidget>

class QLineEdit;
class QPushButton;

// 条件条的完整状态：与 TrafficFilter::Options 的内容条件一一对应，不依赖它——
// 条件条只是"收集条件"，不引入反向依赖，也便于单测直接构造。
// 方向与事件开关（rx/tx/states/warnings/errors）**不在这里**：它们管的是"哪些行进来"
// 而不是"行里的内容长什么样"。方向开关常驻顶栏（FilterFlags），事件开关在显示设置菜单
// （用户钦定）——两者都在本控件之外，但"重置"要能把它们一并复位，见 setResetAvailable。
struct FilterBarState
{
    bool caseSensitive = false;
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
    // 输入框里的**原始条件**。与 state() 的唯一差别：**不受「清除过滤」的停用影响**。
    // 持久化用这一份——按下「清除过滤」只是停用当前过滤，敲进去的关键词/排除词该被记住
    //（用户反馈"关键词过滤与排除是否可以记忆，当我清除过滤时，又得重新输入，很麻烦"）。
    FilterBarState storedState() const;

    // 本行生效的内容条件个数（关键词 / 排除词 / 大小写）。
    // 方向与事件开关都在本控件之外，由面板把它们的计数并进来（见 setResetAvailable）。
    int activeConditionCount() const;

    // 恢复条件：内部抑制 changed，避免加载半成品状态时把已存的值覆盖掉。
    void restore(const FilterBarState &state);

    // 一键回到不过滤（重置按钮与外部入口共用）：只清本行的内容条件。
    void clearAll();

    // 顶栏那排方向开关（与显示设置里的事件开关）不在本控件内，但"重置"要能把它们一并复位：
    // 面板通过这个入口说明"外面还有条件在用"，否则只关掉 RX 时重置按钮会是灰的。
    void setResetAvailable(bool available);

    // 模型里有没有可回溯的记录：没有就没什么可"重新过滤"的，按钮置灰。
    void setRebuildAvailable(bool available);

    // "条件已改、但还没补到已有记录上"：按钮上要留个记号。
    // 不记的话，用户改完条件发现屏幕没动，只会以为过滤坏了。
    void setApplyPending(bool pending);

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
    QPushButton *m_rebuild = nullptr;
    QPushButton *m_reset = nullptr;
    bool m_resetAvailable = false;   // 外部（方向开关 / 事件开关）是否还有生效条件
    bool m_rebuildAvailable = false; // 模型里还有记录可回溯
    bool m_applyPending = false;     // 条件改了但还没补到已有记录上
    bool m_restoring = false;        // 恢复期间抑制 changed
    // 「清除过滤」按下后为真：条件**停用**但**输入框内容保留**。它让 state() 返回空条件
    // （于是显示全部），而 storedState() 照旧拿到用户敲的内容。用户一改输入框或点
    // 「应用过滤」即恢复生效。
    bool m_suspended = false;
};

#endif
