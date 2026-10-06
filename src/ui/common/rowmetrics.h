#ifndef ROWMETRICS_H
#define ROWMETRICS_H

#include <QBoxLayout>
#include <QSizePolicy>
#include <QWidget>

// 把一行里"不该被压缩"的控件钉在自己的 sizeHint 上：横向策略设为 Fixed。
//
// 一行放得下的下限，本来就等于"其中每个控件都能完整显示自己"之和（加间距）。
// 与其让调用方把这个和算出来，不如让**控件自己声明**：Fixed 的语义就是"布局按我的
// sizeHint 给我位置，别缩我"，于是行容器的 minimumSizeHint 由 Qt 在布局时算出来，
// 天然就是正确答案，且随字体 / DPI / 翻译 / 文案变化自动跟随。
//
// 手工求和的代价是必须把内边距、间距、边框、面板自身的留白一项不漏地算进去——
// 漏掉任何一项就少几个像素，症状正是"窗口已经到了最小宽度，这一行最后一个控件
// 还是探出右缘"。那种账目错误不会报错，只会在某个窗口宽度上悄悄显形。
//
// 与 combometrics.h::fitComboToItems 是同一手法（那边是单个下拉框的版本，注释里
// 也说了 Fixed 的用意）。**弹性控件（stretch > 0，例如过滤条的关键词输入框）跳过**：
// 它们靠拉伸吸收余量，下限另有说明（见 FilterBar）。
inline void pinRowToContents(QBoxLayout *row)
{
    if (!row) return;
    for (int i = 0; i < row->count(); ++i) {
        if (row->stretch(i) > 0) continue;
        QLayoutItem *item = row->itemAt(i);
        QWidget *widget = item ? item->widget() : nullptr;
        if (!widget) continue; // 纯 spacer：它本来就不产生宽度需求
        // 纵向策略原样保留：这里只管"放得下"，不管"长多高"。
        widget->setSizePolicy(QSizePolicy::Fixed, widget->sizePolicy().verticalPolicy());
    }
}

#endif
