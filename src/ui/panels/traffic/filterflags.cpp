#include "filterflags.h"
#include "ui/common/tintedicon.h"

#include <QHBoxLayout>
#include <QPushButton>

namespace {

// 与顶栏其余按钮同一套字形尺寸：同一行并排出现，尺寸不一致会一眼看出来。
constexpr int GlyphSize = 16;

// 浅色表面上未勾选字形的静音灰、勾选后跟随 QSS 选中态文字色的琥珀深色。
const QColor MutedGlyph(0x64, 0x75, 0x90);
const QColor ActiveGlyph(0x70, 0x51, 0x1c);

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
    m_state = makeFlag(QStringLiteral("filterState"), QStringLiteral(":/icons/information-outline.svg"),
        tr("状态"), tr("显示状态迁移（端口打开 / 关闭 / 重连）"));
    m_warning = makeFlag(QStringLiteral("filterWarning"), QStringLiteral(":/icons/alert-outline.svg"),
        tr("警告"), tr("显示警告日志"));
    m_error = makeFlag(QStringLiteral("filterError"), QStringLiteral(":/icons/alert-circle-outline.svg"),
        tr("错误"), tr("显示错误日志"));

    for (auto *button : {m_rx, m_tx, m_state, m_warning, m_error}) row->addWidget(button);
}

QPushButton *FilterFlags::makeFlag(const QString &objectName, const QString &iconPath,
    const QString &text, const QString &tip)
{
    auto *button = new QPushButton(text, this);
    button->setObjectName(objectName);
    button->setCheckable(true);
    button->setChecked(true); // 默认全开：不过滤是这排开关的初始语义
    button->setToolTip(tip);
    applyToggleIcon(button, iconPath);
    // 最后才接 changed：勾选状态在构造期就要定下来，而监听方在控件尚未建完时
    // 读到的是半成品（图标都还没换色）。
    connect(button, &QPushButton::toggled, this, [this] {
        if (!m_restoring) emit changed();
    });
    return button;
}

void FilterFlags::applyToggleIcon(QPushButton *button, const QString &iconPath)
{
    // QPushButton 的图标状态位跟的是"按下"而不是"勾选"，所以勾选换色必须显式做：
    // 未勾选用静音灰，勾选用 QSS 选中态文字色——琥珀底上不再出现灰字。
    const QIcon off = tintedIcon(iconPath, MutedGlyph, GlyphSize);
    const QIcon on = tintedIcon(iconPath, ActiveGlyph, GlyphSize);
    const auto refresh = [button, off, on](bool checked) { button->setIcon(checked ? on : off); };
    connect(button, &QPushButton::toggled, this, refresh);
    refresh(button->isChecked());
}

FilterFlagsState FilterFlags::state() const
{
    FilterFlagsState state;
    state.rx = m_rx->isChecked();
    state.tx = m_tx->isChecked();
    state.states = m_state->isChecked();
    state.warnings = m_warning->isChecked();
    state.errors = m_error->isChecked();
    return state;
}

int FilterFlags::activeCount() const
{
    // 默认全开：关掉任何一个都算"在过滤"。
    const auto current = state();
    return (current.rx ? 0 : 1) + (current.tx ? 0 : 1) + (current.states ? 0 : 1)
        + (current.warnings ? 0 : 1) + (current.errors ? 0 : 1);
}

void FilterFlags::restore(const FilterFlagsState &state)
{
    m_restoring = true;
    m_rx->setChecked(state.rx);
    m_tx->setChecked(state.tx);
    m_state->setChecked(state.states);
    m_warning->setChecked(state.warnings);
    m_error->setChecked(state.errors);
    m_restoring = false;
}
