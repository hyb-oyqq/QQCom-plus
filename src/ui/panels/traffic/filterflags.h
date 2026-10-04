#ifndef FILTERFLAGS_H
#define FILTERFLAGS_H

#include <QWidget>

class QPushButton;

// 方向与事件开关的完整状态：与 TrafficFilter::Options 的同名成员一一对应。
struct FilterFlagsState
{
    bool rx = true;
    bool tx = true;
    bool states = true;
    bool warnings = true;
    bool errors = true;
};

// 方向（RX / TX）与事件类型（状态 / 警告 / 错误）五个开关。
//
// **常驻顶栏**（用户钦定）：它们不在可折叠的条件条里，而是紧挨着 `过滤` 按钮——
// 条件条收起来时，这五个开关仍然看得见、点得动。理由是它们调用的频次远高于
// 关键词与排除词：想"只留错误日志"时不该先展开一条条件条。
//
// 形态：图标 + 两字文案。方向与事件此前只有字形（省宽度），挪到顶栏后横向余量充足，
// 补上文案让"这排按钮在做什么"不需要悬停才能确认；两字是宽度与可读性的平衡点。
class FilterFlags : public QWidget
{
    Q_OBJECT
public:
    explicit FilterFlags(QWidget *parent = nullptr);

    FilterFlagsState state() const;
    // 不等于默认（全开）的维度个数：面板用它汇总"当前在过滤"的生效条件数。
    int activeCount() const;
    // 恢复开关状态：内部抑制 changed，避免加载半成品状态时把已存的值覆盖掉。
    void restore(const FilterFlagsState &state);

signals:
    void changed();

private:
    QPushButton *makeFlag(const QString &objectName, const QString &iconPath, const QString &text,
        const QString &tip);
    void applyToggleIcon(QPushButton *button, const QString &iconPath);

    QPushButton *m_rx = nullptr;
    QPushButton *m_tx = nullptr;
    QPushButton *m_state = nullptr;
    QPushButton *m_warning = nullptr;
    QPushButton *m_error = nullptr;
    bool m_restoring = false;
};

#endif
