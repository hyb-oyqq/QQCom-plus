#include "sendpanel.h"
#include "ui/common/payloadcodec.h"
#include "persistence/preferencesstore.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// 正文字输入框是等宽字体的代码输入区，全局 QSS 的 9px 内边距偏宽；
// 用控件自身的样式表把内边距压到 3px，再配合 2px 文档边距，文字距边框约 6px。
// 只覆盖这一个控件，不改 workbench.qss（控件自身样式表优先于应用级规则）。
constexpr int EditorPaddingPx = 3;
constexpr int EditorDocumentMarginPx = 2;

} // namespace

SendPanel::SendPanel(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("Surface");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(10);

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
    auto mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(11);
    m_editor->setFont(mono);
    // 占位示例与初始内容共用同一份字符串，经共享编解码渲染，不会各写一份漂移。
    const QString sample = PayloadCodec::render(QByteArray::fromHex("010300000002c40b"), PayloadCodec::Format::Hex);
    m_editor->setPlaceholderText(tr("输入待发送内容，例如：%1").arg(sample));
    m_editor->setPlainText(sample);
    m_editor->setStyleSheet(QStringLiteral("QPlainTextEdit { padding: %1px; }").arg(EditorPaddingPx));
    m_editor->document()->setDocumentMargin(EditorDocumentMarginPx);
    // 正文固定约三行：行高取自等宽字体自身，不写死像素。输入框不再纵向伸展，
    // 发送区因此贴着内容收紧，纵向空间让给数据显示区（见 MainWindow 的分隔条分配）。
    m_editor->setFixedHeight(m_editor->fontMetrics().lineSpacing() * 3
        + 2 * (EditorPaddingPx + EditorDocumentMarginPx + m_editor->frameWidth()));
    layout->addWidget(m_editor);

    // 格式/末尾贴在正文下方：与字节数、最终内容预览共用一行，省掉一整行的纵向开销。
    // 格式列表来自 PayloadCodec，与数据显示区共用同一套编解码规则。
    auto *options = new QHBoxLayout;
    options->setSpacing(8);
    options->setAlignment(Qt::AlignVCenter);
    m_format = new QComboBox;
    m_format->setObjectName("sendFormat");
    for (const auto format : PayloadCodec::formats())
        m_format->addItem(PayloadCodec::formatName(format));
    m_suffix = new QComboBox;
    m_suffix->setObjectName("sendSuffix");
    // 末尾文案用转义字面量（与指令面板的结尾符列同一套写法），比 CR/LF/CRLF 更直白：
    // 用户看到的就是实际会追加的字节。顺序必须与下方 lineEndings 一一对应，不能调换。
    m_suffix->addItems({tr("不追加"), QStringLiteral("+\\r"), QStringLiteral("+\\n"), QStringLiteral("+\\r\\n")});
    options->addWidget(new QLabel(tr("格式")), 0, Qt::AlignVCenter);
    options->addWidget(m_format, 0, Qt::AlignVCenter);
    options->addWidget(new QLabel(tr("末尾")), 0, Qt::AlignVCenter);
    options->addWidget(m_suffix, 0, Qt::AlignVCenter);
    options->addSpacing(18);
    m_length = new QLabel;
    m_length->setObjectName("ByteCount");
    m_preview = new QLabel;
    m_preview->setObjectName("payloadPreview");
    m_preview->setMinimumWidth(0);
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_preview->setTextFormat(Qt::PlainText);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    options->addWidget(m_length, 0, Qt::AlignVCenter);
    options->addWidget(m_preview, 1, Qt::AlignVCenter);
    layout->addLayout(options);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(6);
    actions->setAlignment(Qt::AlignVCenter);
    m_repeat = new QCheckBox(tr("定时发送"));
    m_repeat->setObjectName("repeatSending");
    m_repeat->setToolTip(tr("按固定周期重复发送主输入框的内容；取消勾选即停止"));
    m_interval = new QSpinBox;
    m_interval->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_interval->setRange(10, 3600000);
    m_interval->setValue(1000);
    m_interval->setSuffix(" ms");
    m_interval->setEnabled(false);
    m_sendButton = new QPushButton(tr("发送"));
    m_sendButton->setObjectName("PrimaryButton");
    m_sendButton->setMinimumWidth(90);
    m_sendButton->setEnabled(false);
    m_sendButton->setToolTip(tr("端口未连接"));
    // 「目标」是个小标签，做成与 A/B/AB 按钮同族的浅底小徽章，而不是裸文字：
    // 裸文字紧挨着三个圆角按钮，字号与边界都不一样，看着像没排完版（用户反馈）。
    auto *targetLabel = new QLabel(tr("目标"));
    targetLabel->setObjectName("TargetLabel");
    m_repeatTimer = new QTimer(this);
    actions->addWidget(m_repeat, 0, Qt::AlignVCenter);
    actions->addWidget(m_interval, 0, Qt::AlignVCenter);
    actions->addStretch();
    actions->addWidget(targetLabel, 0, Qt::AlignVCenter);
    for (auto *button : {m_targetA, m_targetB, m_targetBoth})
        actions->addWidget(button, 0, Qt::AlignVCenter);
    actions->addWidget(m_sendButton, 0, Qt::AlignVCenter);
    layout->addLayout(actions);

    // 定时发送 = 按周期持续发主输入框的内容，勾选即开始、取消即停（不再设次数上限：
    // 用户要的就是"每隔 N 毫秒发一次当前内容"，一个次数输入框只会让人误以为要填内容）。
    // 每拍与手动发送走同一条 issueSend 路径。周期用尽力调度，不承诺硬实时精度。
    m_repeatTimer->setTimerType(Qt::CoarseTimer);
    connect(m_repeat, &QCheckBox::toggled, this, [this](bool checked) {
        m_interval->setEnabled(checked);
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
    m_length->setText(parsed.valid() ? tr("%1 字节").arg(parsed.bytes.size()) : tr("输入错误"));
    const auto text = parsed.valid() ? (parsed.bytes.isEmpty() ? tr("最终发送内容将在这里预览")
        : PayloadCodec::render(parsed.bytes.left(64), PayloadCodec::Format::Hex)
            + (parsed.bytes.size() > 64 ? " …" : "")) : parsed.error;
    m_preview->setText(text);
    m_preview->setToolTip(text);
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
