#include "filterbar.h"
#include "ui/common/buttonmetrics.h"
#include "ui/common/rowmetrics.h"
#include "ui/common/tintedicon.h"

#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>

namespace {

// 与顶栏同一套字形尺寸：两行并排出现，尺寸不一致会一眼看出来。
constexpr int BarIconSize = 14;

// 过滤输入框与顶栏按钮现在同高（约 22px）。前导图标取 14，和框内 12px 文字成比例。
constexpr int FilterInputIconSize = 14;

// 关键词框（整行唯一的**弹性项**）的宽度下限，用半角字符数表示：它会被剩余空间撑开，
// 下限只要保证"看得见自己写的两三个条件"就够了（约 15 个半角字符 ≈ 七八个汉字）。
// 以字符数为准而不是像素值，换字体 / 改 DPI / 切语言时下限跟着字宽一起走。
// （排除框**不是**弹性项，宽度就取下限，所以它的下限另按**提示文本**的宽度定，见构造里那段。）
constexpr int KeywordsMinChars = 15;

QFrame *makeDivider(QWidget *parent)
{
    auto *divider = new QFrame(parent);
    divider->setObjectName("FilterDivider");
    divider->setFrameShape(QFrame::VLine);
    return divider;
}

// 字符宽度下限：跟着控件自己的字体走，不引入第二个"字体从哪来"的来源。
int charWidth(const QWidget *widget)
{
    return widget->fontMetrics().horizontalAdvance(QLatin1Char('0'));
}

} // namespace

FilterBar::FilterBar(QWidget *parent) : QWidget(parent)
{
    setObjectName("filterBar");
    // 恒为一行：容器自己不换行也不滚动，纵向尺寸就是一行控件的高度。
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);

    // 关键词输入是整条唯一的弹性项，吸收全部剩余宽度。
    // 输入框不带清除按钮：clearButton 的 ✕ 与"回车即生效"两个入口会互相干扰，
    // 条件本身又短，全选重输比瞄准一个小叉更快。
    //
    // 文案分工（回车是**新加**的入口，必须让人知道，所以它进 placeholder）：
    //   placeholder —— 只有"条件怎么分隔 / 何时命中 / **回车即应用**"这三条最该被看到的；
    //   tooltip —— 能搜哪些字段（文本 / HEX / 对端地址）与示例，属"想细看才看"的信息。
    // placeholder 在窄窗口下会被截断、输入内容后还会整个消失，所以它只留最基本的三条。
    m_keywords = makeInput(QStringLiteral(":/icons/magnify.svg"),
        tr("关键词（逗号分隔 · 任一命中 · 回车即应用）"),
        tr("关键词：逗号分隔多个条件，命中任一即显示，可搜文本 / HEX / 对端地址。"
           "例：心跳,AA 55,error。输入后按回车即应用过滤"),
        QStringLiteral("filterKeywords"));
    // 弹性项（stretch=1）：窗口不够宽时它先缩到下限，之后整行宁可放不下，也不再把它
    // 压成一条缝。下限**不按提示文本定**——那样会把窗口最小宽度顶得很高；它会被剩余
    // 空间撑开，正常宽度下提示自然完整。
    m_keywords->setMinimumWidth(charWidth(m_keywords) * KeywordsMinChars);
    row->addWidget(m_keywords, 1);

    row->addWidget(makeDivider(this));

    // 排除框与关键词框同款：回车同样即应用（两个入口行为一致，见下面 returnPressed 的接线）。
    // 文案比关键词框短——排除的语义一句话就能说清，而"能搜什么"只对关键词成立。
    m_excludes = makeInput(QStringLiteral(":/icons/minus-circle.svg"),
        tr("排除（命中即隐藏 · 回车即应用）"),
        tr("排除条件：逗号分隔多个条件，命中任一即隐藏该行；优先于关键词生效。"
           "输入后按回车即应用过滤"),
        QStringLiteral("filterExcludes"));
    // 排除框**不是弹性项**，宽度就取它自己的下限，所以下限必须让**整句提示**放得下：
    // QLineEdit 的 sizeHint 只按字体算固定字数（约 17 个半角），装不下
    // "命中即隐藏 · 回车即应用"这类说明，不补偿就会截掉后半句（实测只显示到"回车即"，
    // 而用户要看的正是这几个字）。
    //
    // 补偿量 = 提示文本比 sizeHint 里那份默认文本**宽出来的部分**。这样不用去猜
    // "图标 + 左右内边距"各占多少（sizeHint 里已经算过一遍了），字体/DPI/翻译变化时
    // 也跟着走。那 17 是 Qt 自己 sizeHint 用的字符数，这里是拿它做基准而非复刻算法。
    {
        const QFontMetrics fm(m_excludes->fontMetrics());
        const int extra = fm.horizontalAdvance(m_excludes->placeholderText())
            - fm.horizontalAdvance(QStringLiteral("x")) * 17;
        if (extra > 0)
            m_excludes->setMinimumWidth(m_excludes->sizeHint().width() + extra);
    }
    // 与关键词框**同权重**（都是 stretch 1）：两框在整条里平分剩余宽度，宽度因此一致
    //（用户要求"排除框、查找输入框宽度尽量相同"）。各自的下限仍然生效——空间实在不够时
    // 先压到下限、再整体变宽窗口，不会把哪一个压成一条缝。
    row->addWidget(m_excludes, 1);

    row->addWidget(makeDivider(this));

    // 匹配选项：区分大小写。
    m_case = makeOption(QStringLiteral("filterCaseSensitive"),
        QStringLiteral(":/icons/format-letter-case.svg"), tr("区分大小写"),
        tr("关键词与排除词区分大小写"));
    row->addWidget(m_case);

    row->addWidget(makeDivider(this));

    // 应用过滤：把当前条件应用到**已经收到**的全部记录上。增量路径只管新行，
    // 追溯过去必须走这里——所以它是"改完条件马上看到结果"的唯一显式入口。
    // 加上「应用」这两个字就是用户对它的描述：为当前所有已到达的数据应用一次。
    m_rebuild = new QPushButton(tr("应用过滤"), this);
    m_rebuild->setObjectName("filterRebuild");
    setMutedIcon(m_rebuild, QStringLiteral(":/icons/filter-check-outline.svg"), BarIconSize);
    m_rebuild->setToolTip(tr("把当前过滤条件应用到缓冲内已经收到的记录，不设条数上限。纯数据时仍只显示其中的接收数据。"));
    m_rebuild->setEnabled(false); // 还没收到任何记录时没什么可应用的
    connect(m_rebuild, &QPushButton::clicked, this, [this] {
        m_suspended = false; // 点「应用过滤」= 明确要用当前条件，停用状态结束
        emit rebuildRequested();
    });
    row->addWidget(m_rebuild);

    // 与「应用过滤」一样带文案（用户要求）：一对动作按钮，一个只有图标会让人猜。
    m_reset = new QPushButton(tr("清除过滤"), this);
    m_reset->setObjectName("filterReset");
    setMutedIcon(m_reset, QStringLiteral(":/icons/filter-off.svg"), BarIconSize);
    m_reset->setToolTip(tr("停用关键词过滤，方向和事件开关全部勾回。已输入的词保留。纯数据时仍只显示接收。"));
    connect(m_reset, &QPushButton::clicked, this, [this] {
        // **只停用过滤，不丢用户输入的条件**：清掉输入框等于让用户重敲一遍
        //（用户反馈"当我清除过滤时，又得重新输入，很麻烦"）。条件留在框里，随时点
        //「应用过滤」就能恢复；想彻底清空时手动删掉即可。
        {
            const QSignalBlocker blocker(m_case); // 免得这一下触发 notifyChanged 把停用又撤掉
            m_case->setChecked(false);
        }
        m_suspended = true;
        // 方向开关（顶栏）、事件开关（显示设置菜单）都不在本行里：请面板把它们一并复位，
        // 否则"重置"会留下半套条件，比不做还难解释。
        emit resetRequested();
    });
    row->addWidget(m_reset);

    connect(m_keywords, &QLineEdit::textChanged, this, &FilterBar::notifyChanged);
    connect(m_excludes, &QLineEdit::textChanged, this, &FilterBar::notifyChanged);

    // 回车即应用（用户钦定）：焦点在任一输入框里时按 Enter，等同于点「应用过滤」。
    // 出口与按钮**完全同一个**（rebuildRequested），并同样尊重按钮的启用状态——
    // 按钮禁用时（还没收到任何记录）回车也必须无效，不让一条入口绕开另一条的判断。
    for (auto *input : {m_keywords, m_excludes}) {
        connect(input, &QLineEdit::returnPressed, this, [this] {
            if (m_rebuild->isEnabled()) emit rebuildRequested();
        });
    }

    // 行尾两个动作按钮（应用过滤 / 重置）宽度取齐：它们只差一个图标，并排不等宽很难看。
    // 「应用过滤 •」还会因为待应用记号而变宽，取齐后这个变化不会顶动旁边的按钮。
    // 不设全局最小宽度——按钮自己有 QSS 内边距算出的 sizeHint，这里只求"彼此一致"。
    unifyButtonWidths({m_rebuild, m_reset});

    // 除输入框外一律按自己的 sizeHint 占位（输入框有 stretch，会被本函数跳过）：
    // 这一行"最少要多宽"因此由布局自己算，不由任何一处分像素求和（见 rowmetrics.h）。
    pinRowToContents(row);

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
    // 用「上移」版图标：Qt 落 action 图标的位置比文字视觉中线低约 2 逻辑像素，
    // mutedIcon 无法表达这个修正（见 tintedicon.h::liftedMutedIcon 的说明）。
    auto *leading = input->addAction(liftedMutedIcon(iconPath, FilterInputIconSize),
                                     QLineEdit::LeadingPosition);
    QObject::connect(AppTheme::instance(), &AppTheme::schemeChanged, input, [leading, iconPath] {
        leading->setIcon(liftedMutedIcon(iconPath, FilterInputIconSize));
    });
    // **垂直方向钉成 Fixed**（水平仍由调用方的 stretch 决定）：QLineEdit 的高度一旦被
    // 布局拉伸，多出来的空间会全部堆在底面——QSS 的 padding 是固定值，Qt 不会把文本与
    // 前导图标重新居中，于是两者都贴向顶部（实测框高 46px 而自然高只有 ~35px，图标与
    // placeholder 一起偏上 5px）。钉成 Fixed 后高度恒等于 sizeHint，图标自然居中。
    input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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
    // 用户动了条件条上的任何东西（输入框 / 开关）——「清除过滤」的停用状态到此结束，
    // 条件重新生效。这正是"清除后改一下就能接着用"的手感。
    m_suspended = false;
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

void FilterBar::refreshReset()
{
    // 外部（顶栏的方向开关 / 菜单里的事件开关）还有条件时重置也要可用——只关掉 RX 时
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
            ? tr("过滤条件已改，还没作用于已收到的记录。点这里应用到缓冲内的记录。纯数据时仍只显示其中的接收数据。")
            : tr("把当前过滤条件应用到缓冲内已经收到的记录，不设条数上限。纯数据时仍只显示其中的接收数据。"));
}

FilterBarState FilterBar::state() const
{
    // 「清除过滤」之后条件处于**停用**状态：输入框内容原样保留（用户不必重敲），
    // 但对过滤不生效——所以这里返回空条件，视图随即显示全部。
    if (m_suspended) return FilterBarState();
    return storedState();
}

FilterBarState FilterBar::storedState() const
{
    FilterBarState state;
    state.caseSensitive = m_case->isChecked();
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
    return count;
}

void FilterBar::restore(const FilterBarState &state)
{
    // 恢复期间抑制 changed：半成品状态会触发落盘，把已存的值覆盖掉。
    m_restoring = true;
    m_case->setChecked(state.caseSensitive);
    m_keywords->setText(state.keywords.join(QStringLiteral(", ")));
    m_excludes->setText(state.excludes.join(QStringLiteral(", ")));
    m_restoring = false;
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
