#include "filterbar.h"
#include "ui/common/stayopenmenu.h"
#include "ui/common/tintedicon.h"

#include <QAction>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyle>

namespace {

// 与顶栏同一套字形尺寸：两行并排出现，尺寸不一致会一眼看出来。
constexpr int BarIconSize = 16;

// 浅色表面上未勾选字形的静音灰、勾选后跟随 QSS 选中态文字色的琥珀深色。
const QColor MutedGlyph(0x64, 0x75, 0x90);
const QColor ActiveGlyph(0x70, 0x51, 0x1c);

// chip 区宽度必须有上限：恒单行的前提是"chip 不会把行撑宽"。
// 超出的词用一个不可点的 +N 收口，完整内容仍在输入框里。
constexpr int MaxKeywordChips = 3;

QFrame *makeDivider(QWidget *parent)
{
    auto *divider = new QFrame(parent);
    divider->setObjectName("FilterDivider");
    divider->setFrameShape(QFrame::VLine);
    return divider;
}

} // namespace

FilterBar::FilterBar(QWidget *parent) : QWidget(parent)
{
    setObjectName("filterBar");
    // 恒为一行：容器自己不换行也不滚动，纵向尺寸就是一行控件的高度。
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);

    // 关键词输入是整条唯一的弹性项，吸收全部剩余宽度——先长输入框，再加 chip。
    m_keywords = makeInput(QStringLiteral(":/icons/magnify.svg"),
        tr("关键词（空格分隔 · 任一命中，可搜文本 / HEX / 对端）"),
        tr("关键词：空格分隔多个词，命中任一即显示。回车或失焦后成为一个可删除的 chip"),
        QStringLiteral("filterKeywords"));
    row->addWidget(m_keywords, 1);

    m_chipHost = new QWidget(this);
    auto *chips = new QHBoxLayout(m_chipHost);
    chips->setContentsMargins(0, 0, 0, 0);
    chips->setSpacing(4);
    row->addWidget(m_chipHost);

    row->addWidget(makeDivider(this));

    m_excludes = makeInput(QStringLiteral(":/icons/minus-circle-outline.svg"),
        tr("排除（命中即隐藏）"), tr("排除词：命中任一即隐藏该行；优先于关键词生效"),
        QStringLiteral("filterExcludes"));
    row->addWidget(m_excludes);

    row->addWidget(makeDivider(this));

    // 方向维度：图标按钮而非"标签 + 下拉框"。五个按钮合计宽度只有一个下拉框大小，
    // 这是把多行条件压成一行的手段。字形本身即语义（箭头、信息、警告、错误），
    // 因此只给 tooltip 不给文字。
    m_rx = makeDirection(QStringLiteral("filterRx"), QStringLiteral(":/icons/arrow-down-bold.svg"),
        tr("显示接收 RX"));
    m_tx = makeDirection(QStringLiteral("filterTx"), QStringLiteral(":/icons/arrow-up-bold.svg"),
        tr("显示发送 TX"));
    m_state = makeDirection(QStringLiteral("filterState"), QStringLiteral(":/icons/information-outline.svg"),
        tr("显示状态迁移（端口打开 / 关闭 / 重连）"));
    m_warning = makeDirection(QStringLiteral("filterWarning"), QStringLiteral(":/icons/alert-outline.svg"),
        tr("显示警告日志"));
    m_error = makeDirection(QStringLiteral("filterError"), QStringLiteral(":/icons/alert-circle-outline.svg"),
        tr("显示错误日志"));
    for (auto *button : {m_rx, m_tx, m_state, m_warning, m_error}) row->addWidget(button);

    row->addWidget(makeDivider(this));

    // 低频维度与匹配选项：浮层展开、勾选不关闭、不占布局高度。
    // F2 的帧类型 / 长度 / 对端 / 来源直接并进这一个浮层，主条一行都不用动。
    m_more = new QPushButton(tr("更多"), this);
    m_more->setObjectName("filterMore");
    m_more->setIcon(mutedIcon(QStringLiteral(":/icons/tune.svg"), BarIconSize));
    m_more->setToolTip(tr("低频条件与匹配选项（勾选后保持展开）"));
    auto *moreMenu = new StayOpenMenu(m_more);
    m_caseAction = moreMenu->addAction(tr("区分大小写"));
    m_caseAction->setObjectName("filterCaseSensitive");
    m_regexAction = moreMenu->addAction(tr("正则表达式"));
    m_regexAction->setObjectName("filterRegex");
    m_regexAction->setToolTip(tr("关键词与排除词按正则解释；表达式无效时该条件不参与过滤"));
    for (auto *action : {m_caseAction, m_regexAction}) {
        action->setCheckable(true);
        connect(action, &QAction::toggled, this, &FilterBar::notifyChanged);
    }
    m_more->setMenu(moreMenu);
    row->addWidget(m_more);

    row->addWidget(makeDivider(this));

    m_reset = new QPushButton(this);
    m_reset->setObjectName("filterReset");
    m_reset->setIcon(mutedIcon(QStringLiteral(":/icons/filter-off.svg"), BarIconSize));
    m_reset->setToolTip(tr("清除全部过滤条件，回到不过滤"));
    connect(m_reset, &QPushButton::clicked, this, &FilterBar::clearAll);
    row->addWidget(m_reset);

    connect(m_keywords, &QLineEdit::textChanged, this, &FilterBar::notifyChanged);
    connect(m_excludes, &QLineEdit::textChanged, this, &FilterBar::notifyChanged);

    rebuildKeywordChips();
    refreshResetState();
}

QLineEdit *FilterBar::makeInput(const QString &iconPath, const QString &placeholder, const QString &tip,
    const QString &objectName)
{
    auto *input = new QLineEdit(this);
    input->setObjectName(objectName);
    input->setPlaceholderText(placeholder);
    input->setClearButtonEnabled(true);
    input->setToolTip(tip);
    // 前导字形当"字段名"用：比一个文字标签省三四十像素，恒单行就是靠这些省出来的。
    input->addAction(mutedIcon(iconPath, BarIconSize), QLineEdit::LeadingPosition);
    return input;
}

QPushButton *FilterBar::makeDirection(const QString &objectName, const QString &iconPath, const QString &tip)
{
    auto *button = new QPushButton(this);
    button->setObjectName(objectName);
    button->setCheckable(true);
    button->setChecked(true); // 默认全开：不过滤是这个条子的初始语义
    button->setToolTip(tip);
    applyToggleIcon(button, iconPath);
    // 最后才接 changed：勾选状态在构造期就要定下来，而监听方在条子尚未建完时
    // 读到的是半成品（图标、chip、校验都还没就位）。
    connect(button, &QPushButton::toggled, this, &FilterBar::notifyChanged);
    return button;
}

void FilterBar::applyToggleIcon(QPushButton *button, const QString &iconPath)
{
    // QPushButton 的图标状态位跟的是"按下"而不是"勾选"，所以勾选换色必须显式做：
    // 未勾选用静音灰，勾选用 QSS 选中态文字色——琥珀底上不再出现灰字。
    const QIcon off = tintedIcon(iconPath, MutedGlyph, BarIconSize);
    const QIcon on = tintedIcon(iconPath, ActiveGlyph, BarIconSize);
    const auto refresh = [button, off, on](bool checked) { button->setIcon(checked ? on : off); };
    connect(button, &QPushButton::toggled, this, refresh);
    refresh(button->isChecked());
}

void FilterBar::notifyChanged()
{
    if (m_restoring) return;
    rebuildKeywordChips();
    refreshResetState();
    emit changed();
}

QStringList FilterBar::splitWords(const QString &text)
{
    QStringList words;
    const auto parts = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const auto &part : parts)
        if (!words.contains(part)) words.append(part);
    return words;
}

void FilterBar::rebuildKeywordChips()
{
    auto *chips = m_chipHost->layout();
    while (QLayoutItem *item = chips->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    refreshValidation();

    // chip 只是关键词的"可视 + 可删"视图，唯一事实仍是输入框文本：
    // 点 chip 等于把那个词从文本里拿掉，不引入第二份状态。
    const auto words = splitWords(m_keywords->text());
    const int shown = qMin(words.size(), MaxKeywordChips);
    for (int i = 0; i < shown; ++i) {
        const auto word = words.at(i);
        auto *chip = new QPushButton(QStringLiteral("%1 ✕").arg(word), m_chipHost);
        chip->setObjectName("filterChip");
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(tr("点击移除“%1”").arg(word));
        connect(chip, &QPushButton::clicked, this, [this, word] { removeKeyword(word); });
        chips->addWidget(chip);
    }
    if (words.size() > shown) {
        auto *overflow = new QLabel(tr("+%1").arg(words.size() - shown), m_chipHost);
        overflow->setObjectName("filterChipOverflow");
        overflow->setToolTip(tr("还有 %1 个关键词，在输入框里可见").arg(words.size() - shown));
        chips->addWidget(overflow);
    }
}

void FilterBar::removeKeyword(const QString &keyword)
{
    auto words = splitWords(m_keywords->text());
    words.removeAll(keyword);
    // 改文本即改条件：textChanged 会重建 chip 并发出 changed。
    m_keywords->setText(words.join(QLatin1Char(' ')));
}

void FilterBar::refreshValidation()
{
    // 非法正则不静默失败：输入框标红并给出原因；判定侧同时把这条条件丢掉，
    // 退化的方向是"不过滤"——宁可多显示，绝不因为写错一个括号吞掉整屏数据。
    QString problem;
    if (m_regexAction->isChecked()) {
        for (const auto &word : splitWords(m_keywords->text())) {
            const QRegularExpression pattern(word);
            if (!pattern.isValid()) {
                problem = pattern.errorString();
                break;
            }
        }
    }
    m_keywords->setProperty("invalid", !problem.isEmpty());
    m_keywords->setToolTip(problem.isEmpty()
            ? tr("关键词：空格分隔多个词，命中任一即显示。回车或失焦后成为一个可删除的 chip")
            : tr("正则无效（该条件不参与过滤）：%1").arg(problem));
    // 动态属性变化必须重新求值 QSS，否则红框不会出现。
    m_keywords->style()->unpolish(m_keywords);
    m_keywords->style()->polish(m_keywords);
}

void FilterBar::refreshResetState()
{
    const int advanced = (m_caseAction->isChecked() ? 1 : 0) + (m_regexAction->isChecked() ? 1 : 0);
    // 浮层里生效的条件数写在按钮上：主条看不见浮层内部，这个数字是它唯一的可见出口。
    m_more->setText(advanced > 0 ? tr("更多 (%1)").arg(advanced) : tr("更多"));
    m_reset->setEnabled(activeConditionCount() > 0);
}

FilterBarState FilterBar::state() const
{
    FilterBarState state;
    state.rx = m_rx->isChecked();
    state.tx = m_tx->isChecked();
    state.states = m_state->isChecked();
    state.warnings = m_warning->isChecked();
    state.errors = m_error->isChecked();
    state.caseSensitive = m_caseAction->isChecked();
    state.regex = m_regexAction->isChecked();
    state.keywords = splitWords(m_keywords->text());
    state.excludes = splitWords(m_excludes->text());
    return state;
}

int FilterBar::activeConditionCount() const
{
    const auto current = state();
    int count = 0;
    // 方向与事件默认全开：关掉任何一个都算"在过滤"。
    if (!current.rx) ++count;
    if (!current.tx) ++count;
    if (!current.states) ++count;
    if (!current.warnings) ++count;
    if (!current.errors) ++count;
    if (!current.keywords.isEmpty()) ++count;
    if (!current.excludes.isEmpty()) ++count;
    if (current.caseSensitive) ++count;
    if (current.regex) ++count;
    return count;
}

void FilterBar::restore(const FilterBarState &state)
{
    // 恢复期间抑制 changed：半成品状态会触发落盘，把已存的值覆盖掉。
    m_restoring = true;
    m_rx->setChecked(state.rx);
    m_tx->setChecked(state.tx);
    m_state->setChecked(state.states);
    m_warning->setChecked(state.warnings);
    m_error->setChecked(state.errors);
    m_caseAction->setChecked(state.caseSensitive);
    m_regexAction->setChecked(state.regex);
    m_keywords->setText(state.keywords.join(QLatin1Char(' ')));
    m_excludes->setText(state.excludes.join(QLatin1Char(' ')));
    m_restoring = false;
    rebuildKeywordChips();
    refreshResetState();
}

void FilterBar::clearAll()
{
    restore(FilterBarState());
    emit changed();
}

void FilterBar::focusKeywords()
{
    m_keywords->setFocus(Qt::ShortcutFocusReason);
    m_keywords->selectAll();
}
