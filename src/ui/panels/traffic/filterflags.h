#ifndef FILTERFLAGS_H
#define FILTERFLAGS_H

#include <QWidget>

class QPushButton;

// 方向开关（RX / TX）的取值：本控件持有的那两个。
//
// 事件类型（信息 / 警告 / 错误 / 状态）同样参与过滤，但它们的开关住在显示设置菜单的 QAction 里
// （用户钦定：顶栏让位给显示开关），不在本控件内，因此也不在这个结构里。两者在
// TrafficPanel::filterFlagsState() 汇总成 FilterFlagsState——"条件住在哪个控件"与
// "判定/落盘要哪几项"是两件事，结构分开就不必假装它们同源。
struct DirectionFlags
{
    bool rx = true;
    bool tx = true;
    // 桥接转发产生的 TX。默认显示：关掉后数据区不再出现转发行，转发本身仍继续。
    bool forward = true;
};

// 过滤开关的完整快照：方向（顶栏）+ 事件（显示设置菜单）。面板内部用它一次搬完五项，
// 免得每处调用都自己记"哪项住在哪"。
struct FilterFlagsState
{
    bool rx = true;
    bool tx = true;
    bool forward = true;
    bool infos = true;
    bool warnings = true;
    bool errors = true;
    bool states = true;
};

// 方向开关（RX / TX / 转发）——顶栏上负责"哪些行进来"的一组按钮。
//
// **常驻顶栏**（用户钦定）：它们不在可折叠的条件条里，而是紧挨着 `过滤` 按钮——
// 条件条收起来时这两个开关仍然看得见、点得动。理由是它们调用的频次远高于
// 关键词与排除词：想"只看发送"时不该先展开一条条件条。
//
// 形态：图标 + 短文案。方向此前只有字形（省宽度），挪到顶栏后横向余量充足，
// 补上文案让"这排按钮在做什么"不需要悬停才能确认。
// 「转发」只管桥接产生的发送行是否出现在数据区，不停止转发。
//
// 事件类型开关（信息 / 警告 / 错误 / 状态）**不在这里**：它们与"行内容"选项同类，
// 收进了显示设置的菜单（用户钦定），顶栏那个位置留给了显示开关（见 ViewToggles）。
class FilterFlags : public QWidget
{
    Q_OBJECT
public:
    explicit FilterFlags(QWidget *parent = nullptr);

    DirectionFlags directions() const;
    // 恢复开关状态：内部抑制 changed，避免加载半成品状态时把已存的值覆盖掉。
    void restore(const DirectionFlags &state);

signals:
    void changed();

private:
    QPushButton *makeFlag(const QString &objectName, const QString &iconPath, const QString &text,
        const QString &tip);

    QPushButton *m_rx = nullptr;
    QPushButton *m_tx = nullptr;
    QPushButton *m_forward = nullptr;
    bool m_restoring = false;
};

#endif
