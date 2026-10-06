#include "sendpanel.h"
#include "ui/common/buttonmetrics.h"
#include "ui/common/payloadcodec.h"
#include "persistence/preferencesstore.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// 正文字输入框是等宽字体的代码输入区，全局 QSS 的 9px 内边距偏宽；
// 用控件自身的样式表把内边距压到 3px，再配合 2px 文档边距，文字距边框约 6px。
// 只覆盖这一个控件，不改 workbench.qss（控件自身样式表优先于应用级规则）。
constexpr int EditorPaddingPx = 3;
constexpr int EditorDocumentMarginPx = 2;

// 字节数的强调色。它与预览数据同在**一个标签**里，颜色只能由富文本给（QSS 不作用于 span），
// 所以在这里具名——与文件里既有的预览错误色（下方 updatePreview 里的 #c44343 / #738097）
// 是同一类做法。取一个浅底与深底上都读得清的中性蓝。
constexpr const char *ByteCountColor = "#2563eb";

// 正文行数（用户钦定）：四行足够看清常见指令（例如 AA 06 05 01 02 B2 这类带空格的 HEX），
// 五行会让发送区白白高出一行——那行高度从数据显示区借，不值得。
constexpr int EditorVisibleLines = 4;

} // namespace

SendPanel::SendPanel(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("Surface");
    // 主布局是**左右两栏**（用户钦定："发送按钮放到发送框的右边，定时发送、编码选择也集成到右边"）：
    // 正文在左，编码 / 定时 / 目标 / 发送收进右侧一列。
    // 原来三行纵向铺开（正文 → 格式+末尾+预览 → 定时+目标+发送）要约 200px 高，
    // 而同样的控件并进右侧三行就装下了——省下的纵向空间全部归数据显示区。
    auto *root = new QHBoxLayout(this);
    // **左右必须是 6**：数据区外边距 6 + 这里 6 = 12，与端口条内容、监视区内容的
    // 左缩进对齐（用户反馈"边框距离内部元素没有对齐"）。上下 8：发送区是自适应的
    // 卡片，贴着内容收紧即可，不必再留原来的 10。
    root->setContentsMargins(6, 8, 6, 8);
    root->setSpacing(10);

    // 目标：A / B / AB 三选一，沿用数据显示区端点开关那套可勾选按钮（QPushButton:checked 样式）。
    // 用互斥按钮组而不是独立开关：发送必须有一个目标，不留“都不选”的空状态。
    // 按钮本身放在动作行的发送按钮旁边，顶部不再单占一行，正文因此更高。
    m_targetA = new QPushButton(tr("A"));
    m_targetA->setObjectName("sendTargetA");
    m_targetA->setToolTip(tr("只发送到端点 A"));
    m_targetB = new QPushButton(tr("B"));
    m_targetB->setObjectName("sendTargetB");
    m_targetB->setToolTip(tr("只发送到端点 B"));
    m_targetBoth = new QPushButton(tr("AB"));
    m_targetBoth->setObjectName("sendTargetAB");
    m_targetBoth->setToolTip(tr("同时发送到端点 A 和 B"));
    auto *targetGroup = new QButtonGroup(this);
    targetGroup->setExclusive(true);
    for (auto *button : {m_targetA, m_targetB, m_targetBoth}) {
        button->setCheckable(true);
        targetGroup->addButton(button);
    }
    m_targetA->setChecked(true);

    m_editor = new QPlainTextEdit;
    m_editor->setObjectName("sendEditor");
    // 字体**不在这里设**：全局 QSS 的 `QWidget { font-family: ... }` 会覆盖代码里的
    // setFont()（实测 font().family() 拿到的仍是 QSS 的字体、pointSize 变回 -1），
    // 写在这里等于没写、还会让人以为"已经统一成等宽了"。
    // 字节显示的字族/字号/字重一律由 QSS 保证，用例 hexDisplayFontIsUniform 守着这条契约。
    // 占位示例与初始内容共用同一份字符串，经共享编解码渲染，不会各写一份漂移。
    const QString sample = PayloadCodec::render(QByteArray::fromHex("010300000002c40b"), PayloadCodec::Format::Hex);
    m_editor->setPlaceholderText(tr("输入待发送内容，例如：%1").arg(sample));
    m_editor->setPlainText(sample);
    m_editor->setStyleSheet(QStringLiteral("QPlainTextEdit { padding: %1px; }").arg(EditorPaddingPx));
    m_editor->document()->setDocumentMargin(EditorDocumentMarginPx);
    // 正文固定约四行：行高取自等宽字体自身，不写死像素。输入框不再纵向伸展，
    // 发送区因此贴着内容收紧，纵向空间让给数据显示区（见 MainWindow 的分隔条分配）。
    m_editor->setFixedHeight(m_editor->fontMetrics().lineSpacing() * EditorVisibleLines
        + 2 * (EditorPaddingPx + EditorDocumentMarginPx + m_editor->frameWidth()));

    // 下面先把右侧一列与预览行的控件**全部构造出来**，最后统一组装成左右两栏
    // （两栏都要用到这些控件，穿插着摆放会让依赖顺序变得难读）。

    // 格式/末尾：编码与行尾是"怎么发"，与正文分离后收进右栏第一行。
    // 格式列表来自 PayloadCodec，与数据显示区共用同一套编解码规则。
    m_format = new QComboBox;
    m_format->setObjectName("sendFormat");
    for (const auto format : PayloadCodec::formats())
        m_format->addItem(PayloadCodec::formatName(format));
    m_suffix = new QComboBox;
    m_suffix->setObjectName("sendSuffix");
    // 末尾文案用转义字面量（与指令面板的结尾符列同一套写法），比 CR/LF/CRLF 更直白：
    // 用户看到的就是实际会追加的字节。顺序必须与下方 lineEndings 一一对应，不能调换。
    m_suffix->addItems({tr("不追加"), QStringLiteral("+\\r"), QStringLiteral("+\\n"), QStringLiteral("+\\r\\n")});

    // 字节数（"25B"）与预览数据**合并在同一个标签里**：它们本来是一句话的两半，拆成两个控件后
    // 无论怎么对齐，在分数 DPR 下都可能有 1 物理像素的错位（用户反馈"没有底对齐"，并建议合并
    // ——同一个标签里的文字共享同一条基线，从原理上不可能偏）。
    // 这同时省掉了"两个标签高度必须一致"那类补丁（QLabel 开了可选中后 sizeHint 会高 2px）。
    m_preview = new QLabel;
    m_preview->setObjectName("payloadPreview");
    m_preview->setMinimumWidth(0);
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_preview->setTextFormat(Qt::RichText); // 字节数要着色，见 updatePreview()
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_repeat = new QCheckBox(tr("定时发送"));
    m_repeat->setObjectName("repeatSending");
    m_repeat->setToolTip(tr("按固定周期重复发送主输入框的内容；取消勾选即停止"));
    m_interval = new QSpinBox;
    m_interval->setObjectName("repeatInterval"); // 用例要能定位它（"始终可输入、只收数字"）
    m_interval->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_interval->setRange(10, 3600000);
    m_interval->setValue(1000);
    // 纯数字输入（用户钦定）：不加 " ms" 后缀——后缀会把框读成"值+单位"的复合体，
    // 键盘输入时也容易被当成内容的一部分。单位改由 tooltip 交代，数字本身就够读。
    m_interval->setToolTip(tr("发送间隔（毫秒）"));
    // **始终可输入**（用户钦定）：勾选前就能把间隔设好，勾上即按该值跑。
    // 它是 QSpinBox——非数字键（含粘贴的非法文本）会被它自带的校验器挡掉，不必另做处理。
    m_sendButton = new QPushButton(tr("发送"));
    m_sendButton->setObjectName("PrimaryButton");
    m_sendButton->setEnabled(false);
    m_sendButton->setToolTip(tr("端口未连接"));
    // 目标 A/B/AB 三选一，不再带「目标」二字的小标签（用户钦定）：
    // 三个端点字母本身已经说清了这一排是什么，标签只是重复占宽。
    // 目标按钮 trio：三个并排按钮宽度统一（AB 比 A/B 宽，不统一的话中间那个会明显鼓出来）。
    // 「发送」是主操作，宽度跟着这一排最宽者——既不至于被文字截断，也不会长短参差。
    // 见 unifyActionWidths()：这里不写死像素，字体/DPI/翻译变化时重算即可。
    m_repeatTimer = new QTimer(this);

    // ---- 组装：左栏（正文 + 预览） / 右栏（编码 / 定时 / 目标 + 发送）----

    auto *leftColumn = new QVBoxLayout;
    leftColumn->setSpacing(6);
    leftColumn->addWidget(m_editor);
    // 字节数与最终内容预览贴着正文下方：它们回答的是"我到底要发什么"，与正文同属
    // "内容"一侧。预览文本长度不可控，放进右栏会把那一栏撑得比正文还高。
    // 预览行现在只有一个标签（字节数与预览已合并，见上），不再需要"摆两个控件再对齐"。
    auto *previewRow = new QHBoxLayout;
    previewRow->addWidget(m_preview, 1);
    leftColumn->addLayout(previewRow);

    // 右栏三行的语义顺序：怎么发（编码/末尾）→ 发几拍（定时/间隔）→ 发给谁（目标 + 发送）。
    // 行高与左侧四行正文相当，两栏自然齐平，不需要额外撑高。
    auto *rightColumn = new QVBoxLayout;
    rightColumn->setSpacing(6);

    auto *encodeRow = new QHBoxLayout;
    encodeRow->setSpacing(6);
    encodeRow->addWidget(new QLabel(tr("格式")), 0, Qt::AlignVCenter);
    encodeRow->addWidget(m_format, 0, Qt::AlignVCenter);
    encodeRow->addSpacing(6);
    encodeRow->addWidget(new QLabel(tr("末尾")), 0, Qt::AlignVCenter);
    encodeRow->addWidget(m_suffix, 0, Qt::AlignVCenter);
    encodeRow->addStretch();

    auto *repeatRow = new QHBoxLayout;
    repeatRow->setSpacing(6);
    repeatRow->addWidget(m_repeat, 0, Qt::AlignVCenter);
    repeatRow->addWidget(m_interval, 0, Qt::AlignVCenter);
    repeatRow->addStretch();

    auto *actionRow = new QHBoxLayout;
    actionRow->setSpacing(6);
    for (auto *button : {m_targetA, m_targetB, m_targetBoth})
        actionRow->addWidget(button, 0, Qt::AlignVCenter);
    actionRow->addWidget(m_sendButton, 0, Qt::AlignVCenter);

    rightColumn->addLayout(encodeRow);
    rightColumn->addLayout(repeatRow);
    rightColumn->addLayout(actionRow);

    // 左栏拿全部剩余宽度（正文越宽越少折行），右栏取自身内容宽度。
    root->addLayout(leftColumn, 1);
    root->addLayout(rightColumn, 0);

    // 送进统一宽度：A/B/AB 与「发送」同属这一排，用一个宽度。必须在布局挂好之后调，
    // 这样 sizeHint 才带得上它自己那一身 QSS（内边距/圆角），量出来的宽才是真的。
    unifyActionWidths();

    // 定时发送 = 按周期持续发主输入框的内容，勾选即开始、取消即停（不再设次数上限：
    // 用户要的就是"每隔 N 毫秒发一次当前内容"，一个次数输入框只会让人误以为要填内容）。
    // 每拍与手动发送走同一条 issueSend 路径。周期用尽力调度，不承诺硬实时精度。
    m_repeatTimer->setTimerType(Qt::CoarseTimer);
    connect(m_repeat, &QCheckBox::toggled, this, [this](bool checked) {
        // 间隔输入框**不随勾选启用/禁用**：它随时可改（见构造处的说明）。
        if (checked) {
            issueSend(true); // 勾选立即发第一拍，之后按周期
            m_repeatTimer->start(m_interval->value());
        } else {
            m_repeatTimer->stop();
        }
    });
    connect(m_repeatTimer, &QTimer::timeout, this, [this] { issueSend(true); });
    connect(m_interval, &QSpinBox::valueChanged, this, [this](int ms) {
        if (m_repeatTimer->isActive()) m_repeatTimer->start(ms);
    });
    connect(m_sendButton, &QPushButton::clicked, this, [this] {
        issueSend(false);
        // 单次发送不打断进行中的定时任务：两者目标相同、语义一致。
    });
    connect(m_editor, &QPlainTextEdit::textChanged, this, &SendPanel::updatePreview);
    connect(m_format, &QComboBox::currentIndexChanged, this, &SendPanel::updatePreview);
    connect(m_suffix, &QComboBox::currentIndexChanged, this, &SendPanel::updatePreview);
    // 两个选择器任一变化即落盘；加载偏好期间由 m_loading 抑制（见 loadPreferences）。
    connect(m_format, &QComboBox::currentIndexChanged, this, &SendPanel::savePreferences);
    connect(m_suffix, &QComboBox::currentIndexChanged, this, &SendPanel::savePreferences);
    for (auto *button : {m_targetA, m_targetB, m_targetBoth})
        connect(button, &QPushButton::toggled, this, &SendPanel::updateSendAvailability);
    updatePreview();
}

void SendPanel::setPorts(const QList<PortDraft> &ports)
{
    m_ports = ports;
    updateTargets();
}

void SendPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 字体 / DPI / 翻译都可能让"最宽的那个按钮"变宽，而 QSS 是把内边距算进
    // sizeHint 的——控件在真窗口里拿到样式之后 hint 才稳定。宽度变化时重算一次，
    // 保证 A/B/AB 与发送四个按钮始终等宽且不被文字截断。
    unifyActionWidths();
}

void SendPanel::unifyActionWidths()
{
    // 先把这四个按钮的横向策略**钉成 Fixed**：unifyButtonWidths() 只设 minimumWidth，
    // 而 minimum 策略允许被拉宽——右侧栏的宽度由"最宽的那一行"决定，多出来的空间
    // 落到本行时按取整分给各按钮，就会出现 61/62/62/62 这种一像素参差。
    // 钉成 Fixed 后它们的宽度恒等于 minimumWidth（= 其中最宽者），不再受行宽影响。
    // 这不违反"不写死宽度"：宽度仍由 sizeHint 推出来，字体/DPI/翻译变化时会重算。
    for (auto *button : {m_targetA, m_targetB, m_targetBoth, m_sendButton}) {
        if (button) button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    unifyButtonWidths({m_targetA, m_targetB, m_targetBoth, m_sendButton});
}

void SendPanel::setEndpointOpen(int index, bool open)
{
    if (index < 0 || index >= 2) return;
    m_openFlags[index] = open;
    updateSendAvailability();
}

void SendPanel::updateTargets()
{
    // 目标只有端点个数这一个约束：缺 B 时 B/AB 不可选。
    // 任何影响目标可用性的状态变化都必须回调本函数，否则回落逻辑不会重跑。
    m_targetA->setEnabled(m_ports.size() >= 1);
    m_targetB->setEnabled(m_ports.size() >= 2);
    m_targetBoth->setEnabled(m_ports.size() >= 2);
    // 被选中的目标失效时回落到仍然可用的那个，不留“已选但不可用”的空目标。
    const bool chosenAvailable = (m_targetA->isChecked() && m_targetA->isEnabled())
        || (m_targetB->isChecked() && m_targetB->isEnabled())
        || (m_targetBoth->isChecked() && m_targetBoth->isEnabled());
    if (!chosenAvailable && m_targetA->isEnabled()) m_targetA->setChecked(true);
}

void SendPanel::updatePreview()
{
    // “末尾”选项产生的行尾字节交给共享编解码接口统一追加，与数据显示区共用同一套解析规则。
    static const QList<QByteArray> lineEndings = {"", "\r", "\n", "\r\n"};
    const auto parsed = PayloadCodec::parse(m_editor->toPlainText(),
        PayloadCodec::formats().value(m_format->currentIndex(), PayloadCodec::Format::Hex),
        lineEndings.value(m_suffix->currentIndex()));
    m_payloadValid = parsed.valid();
    m_payloadBytes = parsed.bytes;
    // 字节数用 **ASCII 且不加空格**（"25B"）：
    //   · ASCII —— 它就贴在 HEX 左边显示，汉字的字面框天生比数字高（实测「字节」段 16px、
    //     纯 HEX 段 12px），同一行永远对不齐（用户反馈"字体没有统一"，并确认要 ASCII）。
    //   · 不加空格 —— "25" 只有两个字符、后面再跟一个空格就显得 B 孤零零飘在外面
    //     （用户反馈"B 距离 25 比较远"）。紧贴后读作一个整体，与右侧 HEX 的节奏也更接近。
    const QString countText = parsed.valid() ? tr("%1B").arg(parsed.bytes.size()) : tr("输入错误");
    const auto text = parsed.valid() ? (parsed.bytes.isEmpty() ? tr("最终发送内容将在这里预览")
        : PayloadCodec::render(parsed.bytes.left(64), PayloadCodec::Format::Hex)
            + (parsed.bytes.size() > 64 ? " …" : "")) : parsed.error;
    // **字节数与预览文本拼进同一个标签**：同一份文本流共享同一条基线，不存在两个控件之间的
    // 像素级错位。字节数着色用 span（内容级颜色，优先于标签自身的样式表）；两段之间的间隔用
    // 两个不换行空格，宽度与原来布局的 8px 间距相当。用户文本一律 toHtmlEscaped，
    // 免得 HEX 里恰好出现的 '<' / '&' 被当成标记。
    m_preview->setText(QStringLiteral("<span style=\"color:%1\">%2</span>&nbsp;&nbsp;%3")
                           .arg(QLatin1String(ByteCountColor), countText.toHtmlEscaped(),
                                text.toHtmlEscaped()));
    m_preview->setToolTip(QStringLiteral("%1  %2").arg(countText, text));
    // 预览的错误状态只在翻转时改一次样式表，避免每个按键都重新解析样式。
    if (m_previewError != !parsed.valid()) {
        m_previewError = !parsed.valid();
        m_preview->setStyleSheet(m_previewError ? "color: #c44343;" : "color: #738097;");
    }
    updateSendAvailability();
}

void SendPanel::updateSendAvailability()
{
    // 发送条件：内容可解析且非空，且选中目标里至少有一个端点已连接。
    // AB 目标只连了一端时按该端发送，不必强迫用户切目标。
    const bool anySelectedOpen =
        (m_targetA->isChecked() && m_openFlags[0])
        || (m_targetB->isChecked() && m_openFlags[1])
        || (m_targetBoth->isChecked() && (m_openFlags[0] || m_openFlags[1]));
    const bool ready = m_payloadValid && !m_payloadBytes.isEmpty() && anySelectedOpen;
    m_sendButton->setEnabled(ready);
    m_sendButton->setToolTip(ready ? tr("发送 %1 字节").arg(m_payloadBytes.size())
        : anySelectedOpen ? tr("请输入有效内容") : tr("端口未连接"));
}

int SendPanel::targetMask() const
{
    int mask = 0;
    if (m_targetA->isChecked()) mask |= 1;
    if (m_targetB->isChecked()) mask |= 2;
    if (m_targetBoth->isChecked()) mask |= 3;
    return mask;
}

void SendPanel::issueSend(bool scheduled)
{
    if (!m_sendButton->isEnabled()) return;
    emit sendRequested(m_payloadBytes, targetMask(), scheduled);
}

void SendPanel::deliverPayload(const QByteArray &payload)
{
    if (payload.isEmpty()) return;
    // 走的是与手点「发送」完全相同的信号，所以目标、来源标记、失败记录全自动一致。
    emit sendRequested(payload, targetMask(), false);
}

void SendPanel::setPayload(const QByteArray &bytes)
{
    // 阻断三个控件的中间信号，整次"填入发送区"只解析一次。
    const QSignalBlocker formatBlocker(m_format), suffixBlocker(m_suffix), editorBlocker(m_editor);
    m_format->setCurrentIndex(PayloadCodec::formats().indexOf(PayloadCodec::Format::Hex));
    m_suffix->setCurrentIndex(0);
    m_editor->setPlainText(PayloadCodec::render(bytes, PayloadCodec::Format::Hex));
    updatePreview();
    m_editor->setFocus();
}

void SendPanel::setPreferences(PreferencesStore *store)
{
    m_preferences = store;
    loadPreferences();
}

void SendPanel::loadPreferences()
{
    if (!m_preferences) return;
    // 恢复过程中 setCurrentIndex 会联动 savePreferences；不抑制的话，
    // "恢复了格式才轮到行尾"的中间状态会把已存的行尾值覆盖掉（与 TrafficPanel 同一模式）。
    m_loading = true;
    // 存的是格式名而不是下标：格式列表顺序将来变了，旧配置仍然有效。
    const auto format = m_preferences->value("send/format").toString();
    if (m_format->findText(format) >= 0)
        m_format->setCurrentText(format);
    m_suffix->setCurrentIndex(m_preferences->value("send/suffix", 0).toInt());
    m_loading = false;
}

void SendPanel::savePreferences()
{
    if (!m_preferences || m_loading) return;
    m_preferences->setValue("send/format", m_format->currentText());
    m_preferences->setValue("send/suffix", m_suffix->currentIndex());
}
