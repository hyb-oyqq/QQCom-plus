#include "filterflags.h"
#include "ui/common/rowmetrics.h"
#include "ui/common/tintedicon.h"

#include <QHBoxLayout>
#include <QPushButton>

namespace {

// 与顶栏其余按钮同一套字形尺寸：同一行并排出现，尺寸不一致会一眼看出来。
constexpr int GlyphSize = 16;

} // namespace

FilterFlags::FilterFlags(QWidget *parent) : QWidget(parent)
{
    setObjectName("filterFlags");
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);

    m_rx = makeFlag(QStringLiteral("filterRx"), QStringLiteral(":/icons/arrow-down-bold.svg"),
        tr("RX"), tr("显示接收 RX"));
    m_tx = makeFlag(QStringLiteral("filterTx"), QStringLiteral(":/icons/arrow-up-bold.svg"),
        tr("TX"), tr("显示发送 TX"));

    for (auto *button : {m_rx, m_tx}) row->addWidget(button);
    // 两个方向开关都按自己的 sizeHint 占位：顶栏那一行的下限因此不需要外部求和
    // （见 rowmetrics.h）。
    pinRowToContents(row);
}

QPushButton *FilterFlags::makeFlag(const QString &objectName, const QString &iconPath,
    const QString &text, const QString &tip)
{
    auto *button = new QPushButton(text, this);
    button->setObjectName(objectName);
    button->setCheckable(true);
    button->setChecked(true); // 默认全开：不过滤是这排开关的初始语义
    button->setToolTip(tip);
    bindToggleIcon(button, iconPath, GlyphSize);
    // 最后才接 changed：勾选状态在构造期就要定下来，而监听方在控件尚未建完时
    // 读到的是半成品（图标都还没换色）。
    connect(button, &QPushButton::toggled, this, [this] {
        if (!m_restoring) emit changed();
    });
    return button;
}

DirectionFlags FilterFlags::directions() const
{
    DirectionFlags state;
    state.rx = m_rx->isChecked();
    state.tx = m_tx->isChecked();
    return state;
}

void FilterFlags::restore(const DirectionFlags &state)
{
    m_restoring = true;
    m_rx->setChecked(state.rx);
    m_tx->setChecked(state.tx);
    m_restoring = false;
}
