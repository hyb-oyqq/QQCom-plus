#include "filterbar.h"
#include "ui/common/tintedicon.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyle>

namespace {

// 与顶栏同一套字形尺寸：两行并排出现，尺寸不一致会一眼看出来。
constexpr int BarIconSize = 16;

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

    // 关键词输入是整条唯一的弹性项，吸收全部剩余宽度。
    // 输入框不带清除按钮：clearButton 的 ✕ 与"回车即生效"两个入口会互相干扰，
    // 条件本身又短，全选重输比瞄准一个小叉更快。
    m_keywords = makeInput(QStringLiteral(":/icons/magnify.svg"),
        tr("关键词（逗号分隔 · 任一命中，可搜文本 / HEX / 对端）"),
        tr("关键词：逗号分隔多个条件，命中任一即显示。例：心跳,AA 55,error"),
        QStringLiteral("filterKeywords"));
    row->addWidget(m_keywords, 1);

    row->addWidget(makeDivider(this));

    m_excludes = makeInput(QStringLiteral(":/icons/minus-circle-outline.svg"),
        tr("排除（逗号分隔 · 命中即隐藏）"),
        tr("排除条件：逗号分隔多个条件，命中任一即隐藏该行；优先于关键词生效"),
        QStringLiteral("filterExcludes"));
    row->addWidget(m_excludes);

    row->addWidget(makeDivider(this));

    // 匹配选项就摆在条上（用户钦定：不做下拉列表）。两个布尔条件 = 两个按钮，
    // 比"更多 ▾ + 浮层"少一次点击，也让"当前是不是在按正则匹配"一眼可见。
    m_case = makeOption(QStringLiteral("filterCaseSensitive"),
        QStringLiteral(":/icons/format-letter-case.svg"), tr("区分大小写"),
        tr("关键词与排除词区分大小写"));
    m_regex = makeOption(QStringLiteral("filterRegex"), QStringLiteral(":/icons/regex.svg"),
        tr("正则表达式"),
        tr("关键词与排除词按正则解释；表达式无效时该条件不参与过滤"));
    row->addWidget(m_case);
    row->addWidget(m_regex);

    row->addWidget(makeDivider(this));

    // 应用过滤：把当前条件应用到**已经收到**的全部记录上。增量路径只管新行，
    // 追溯过去必须走这里——所以它是"改完条件马上看到结果"的唯一显式入口。
    // 加上「应用」这两个字就是用户对它的描述：为当前所有已到达的数据应用一次。
    m_rebuild = new QPushButton(tr("应用过滤"), this);
    m_rebuild->setObjectName("filterRebuild");
    m_rebuild->setIcon(mutedIcon(QStringLiteral(":/icons/filter-check-outline.svg"), BarIconSize));
    m_rebuild->setToolTip(tr("把当前过滤条件应用到已经收到的全部记录（缓冲内全量重放，不设条数上限）"));
    m_rebuild->setEnabled(false); // 还没收到任何记录时没什么可应用的
    connect(m_rebuild, &QPushButton::clicked, this, &FilterBar::rebuildRequested);
    row->addWidget(m_rebuild);

    m_reset = new QPushButton(this);
    m_reset->setObjectName("filterReset");
    m_reset->setIcon(mutedIcon(QStringLiteral(":/icons/filter-off.svg"), BarIconSize));
    m_reset->setToolTip(tr("清除全部过滤条件，回到不过滤"));
    connect(m_reset, &QPushButton::clicked, this, [this] {
        clearAll();
        // 方向与事件开关常驻顶栏、不在本行里：请面板把它们一并复位，
        // 否则"重置"会留下半套条件，比不做还难解释。
        emit resetRequested();
    });
    row->addWidget(m_reset);

    connect(m_keywords, &QLineEdit::textChanged, this, &FilterBar::notifyChanged);
    connect(m_excludes, &QLineEdit::textChanged, this, &FilterBar::notifyChanged);

    refreshReset();
}

QLineEdit *FilterBar::makeInput(const QString &iconPath, const QString &placeholder, const QString &tip,
    const QString &objectName)
{
    auto *input = new QLineEdit(this);
    input->setObjectName(objectName);
    input->setPlaceholderText(placeholder);
    input->setToolTip(tip);
    // 前导字形当"字段名"用：比一个文字标签省三四十像素，恒单行就是靠这些省出来的。
    input->addAction(mutedIcon(iconPath, BarIconSize), QLineEdit::LeadingPosition);
    return input;
}

QPushButton *FilterBar::makeOption(const QString &objectName, const QString &iconPath,
    const QString &text, const QString &tip)
{
    auto *button = new QPushButton(text, this);
    button->setObjectName(objectName);
    button->setCheckable(true);
    button->setToolTip(tip);
    bindToggleIcon(button, iconPath, BarIconSize);
    // 最后才接 changed：勾选状态在构造期就要定下来，而监听方在控件尚未建完时
    // 读到的是半成品（图标都还没换色）。
    connect(button, &QPushButton::toggled, this, &FilterBar::notifyChanged);
    return button;
}

void FilterBar::notifyChanged()
{
    if (m_restoring) return;
    refreshValidation();
    refreshReset();
    emit changed();
}

QStringList FilterBar::splitConditions(const QString &text)
{
    QStringList conditions;
    for (const auto &part : text.split(QLatin1Char(','))) {
        const auto trimmed = part.trimmed();
        if (!trimmed.isEmpty() && !conditions.contains(trimmed)) conditions.append(trimmed);
    }
    return conditions;
}

void FilterBar::refreshValidation()
{
    // 非法正则不静默失败：输入框标红并给出原因；判定侧同时把这条条件丢掉，
    // 退化的方向是"不过滤"——宁可多显示，绝不因为写错一个括号吞掉整屏数据。
    QString problem;
    if (m_regex->isChecked()) {
        for (const auto &condition : splitConditions(m_keywords->text())) {
            const QRegularExpression pattern(condition);
            if (!pattern.isValid()) {
                problem = pattern.errorString();
                break;
            }
        }
    }
    m_keywords->setProperty("invalid", !problem.isEmpty());
    m_keywords->setToolTip(problem.isEmpty()
            ? tr("关键词：逗号分隔多个条件，命中任一即显示。例：心跳,AA 55,error")
            : tr("正则无效（该条件不参与过滤）：%1").arg(problem));
    // 动态属性变化必须重新求值 QSS，否则红框不会出现。
    m_keywords->style()->unpolish(m_keywords);
    m_keywords->style()->polish(m_keywords);
}

void FilterBar::refreshReset()
{
    // 外部（顶栏常驻的方向/事件开关）还有条件时重置也要可用——只关掉 RX 时
    // 重置按钮必须是亮的，否则那半套条件就没有回到默认的入口。
    m_reset->setEnabled(m_resetAvailable || activeConditionCount() > 0);
}

void FilterBar::setResetAvailable(bool available)
{
    m_resetAvailable = available;
    refreshReset();
}

void FilterBar::setRebuildAvailable(bool available)
{
    // 每次入模批次都会问一次，所以先比再看：值没变就不碰控件（热路径开销为零）。
    if (m_rebuildAvailable == available) return;
    m_rebuildAvailable = available;
    m_rebuild->setEnabled(available);
}

void FilterBar::setApplyPending(bool pending)
{
    // 同 setRebuildAvailable：这是每次入模都会被问到的路径，值没变就直接返回。
    if (m_applyPending == pending) return;
    m_applyPending = pending;
    // 只在按钮文字末尾加一个记号，不改文字本身：按钮的位置与宽度基本不动，
    // 而"有件事还没做"这件事一眼就能看见。文字长度变化由关键词输入的弹性吸收。
    m_rebuild->setText(pending ? tr("应用过滤 •") : tr("应用过滤"));
    m_rebuild->setToolTip(pending
            ? tr("过滤条件已改，还没作用于已收到的记录。点这里把条件应用到全部已到达的数据")
            : tr("把当前过滤条件应用到已经收到的全部记录（缓冲内全量重放，不设条数上限）"));
}

FilterBarState FilterBar::state() const
{
    FilterBarState state;
    state.caseSensitive = m_case->isChecked();
    state.regex = m_regex->isChecked();
    state.keywords = splitConditions(m_keywords->text());
    state.excludes = splitConditions(m_excludes->text());
    return state;
}

int FilterBar::activeConditionCount() const
{
    const auto current = state();
    int count = 0;
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
    m_case->setChecked(state.caseSensitive);
    m_regex->setChecked(state.regex);
    m_keywords->setText(state.keywords.join(QStringLiteral(", ")));
    m_excludes->setText(state.excludes.join(QStringLiteral(", ")));
    m_restoring = false;
    refreshValidation();
    refreshReset();
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
