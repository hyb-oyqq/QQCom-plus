#include "filterflags.h"
#include "ui/common/rowmetrics.h"
#include "ui/common/tintedicon.h"

#include <QHBoxLayout>
#include <QPushButton>

namespace {

// 与顶栏其余按钮同一套字形尺寸：同一行并排出现，尺寸不一致会一眼看出来。
constexpr int GlyphSize = 14;

} // namespace

FilterFlags::FilterFlags(QWidget *parent) : QWidget(parent)
{
    setObjectName("filterFlags");
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);

    m_rx = makeFlag(QStringLiteral("filterRx"), QStringLiteral(":/icons/arrow-down-bold.svg"),
        tr("RX"), tr("显示接收。点一下就作用到已经收到的行"));
    m_tx = makeFlag(QStringLiteral("filterTx"), QStringLiteral(":/icons/arrow-up-bold.svg"),
        tr("TX"), tr("显示发送。点一下就作用到已经收到的行"));
    // 紧挨 TX：桥接转发在数据区就是一条 TX+FWD。关掉它只藏这一类行，收发本身不受影响。
    m_forward = makeFlag(QStringLiteral("filterForward"), QStringLiteral(":/icons/swap-horizontal.svg"),
        tr("转发"), tr("显示桥接转发的数据。点一下就作用到已经收到的行。关闭后仍继续转发"));

    for (auto *button : {m_rx, m_tx, m_forward}) row->addWidget(button);
    // 开关都按自己的 sizeHint 占位：顶栏那一行的下限因此不需要外部求和
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
    state.forward = m_forward->isChecked();
    return state;
}

void FilterFlags::restore(const DirectionFlags &state)
{
    m_restoring = true;
    m_rx->setChecked(state.rx);
    m_tx->setChecked(state.tx);
    m_forward->setChecked(state.forward);
    m_restoring = false;
}
