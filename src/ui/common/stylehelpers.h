#ifndef STYLEHELPERS_H
#define STYLEHELPERS_H

#include <QStyle>
#include <QVariant>
#include <QWidget>

// 改动态属性并让样式表**立刻**重新求值。
//
// 为什么需要这一步：QSS 的属性选择器（`QPushButton[sending="true"]`）只在控件被 polish
// 时求值一次。单改属性不会触发重绘，界面一点变化都没有——而代码读起来完全正确，
// 是最容易白查半天的一类"改了但没生效"。unpolish + polish 把控件重新过一遍样式表，
// `update()` 保证立即重绘（不必等下一条无关的绘制事件把它带出来）。
//
// 值相同就直接返回：调用点常常在热路径或每次状态变化时都调一次，属性没变还去
// unpolish 会让整棵子树重新求值样式，纯属白烧 CPU。
inline void setStyledProperty(QWidget *widget, const char *name, const QVariant &value)
{
    if (!widget) return;
    if (widget->property(name) == value) return;
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

#endif
