#ifndef BUTTONMETRICS_H
#define BUTTONMETRICS_H

#include <QAbstractButton>
#include <QList>
#include <QWidget>

// 让同排的一批按钮宽度取齐——统一跟随其中 sizeHint 最宽的那个。
//
// 动机：同一行里"发送" / "按序发送勾选项" / "应用过滤" 这类主操作按钮，字体与图标
// 数量不同，sizeHint 天然不等宽；并排时参差不齐很难看，而宽度不足的按钮还会被 Qt
// 把文字截成 "按序发送勾选…"（用户反馈）。做法是给每个按钮设 minimumWidth = 最宽者，
// 而不是写死像素：字体 / DPI / 翻译变化时，最宽者自己会变，这里重算即可自动跟随。
//
// 只在**同一排**按钮间调用；跨排不要混用（否则会把毫不相关的宽度绑在一起）。
inline void unifyButtonWidths(const QList<QAbstractButton *> &buttons)
{
    if (buttons.isEmpty()) return;

    // 先清掉上一次设的 minimumWidth：否则第二次调用时 sizeHint 已受旧下限影响，
    // 宽度只增不减，按钮一旦被撑宽就再也收不回来（字体调小后尤其明显）。
    for (auto *button : buttons) {
        if (button) button->setMinimumWidth(0);
    }

    int widest = 0;
    for (auto *button : buttons) {
        if (!button || button->isHidden()) continue;
        widest = qMax(widest, button->sizeHint().width());
    }
    if (widest <= 0) return;
    for (auto *button : buttons) {
        if (button) button->setMinimumWidth(widest);
    }
}

#endif
