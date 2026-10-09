#include "sendpanel.h"
#include "filetransferwidget.h"
#include "app/endpointruntime.h"
#include "ui/common/buttonmetrics.h"
#include "ui/common/combometrics.h"
#include "ui/common/payloadcodec.h"
#include "ui/common/tintedicon.h"
#include "persistence/preferencesstore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// 页签、发送按钮与文件页动作共用这一档，和监视区顶栏的 14px 字形对齐。
constexpr int GlyphSize = 14;

// 三页共用一个高度，取内容最高的那一页。跟当前页走的话，手动发送和文件页
// 差出一两行，切换标签时分隔条就会跳。
int tallestPageHeight(const QTabWidget *tabs)
{
    int page = 0;
    if (!tabs) return 0;
    for (int i = 0; i < tabs->count(); ++i) {
        const QWidget *widget = tabs->widget(i);
        if (!widget) continue;
        page = qMax(page, qMax(widget->sizeHint().height(), widget->minimumSizeHint().height()));
    }
    return page;
}

class ModeTabs final : public QTabWidget
{
public:
    explicit ModeTabs(QWidget *parent = nullptr) : QTabWidget(parent) {}

    QSize sizeHint() const override
    {
        const int page = tallestPageHeight(this);
        int width = 0;
        for (int i = 0; i < count(); ++i) {
            if (const QWidget *widget = this->widget(i))
                width = qMax(width, qMax(widget->sizeHint().width(), widget->minimumSizeHint().width()));
        }
        const int bar = tabBar() ? tabBar()->sizeHint().height() : 0;
        return QSize(width, page + bar + 4);
    }

    QSize minimumSizeHint() const override { return sizeHint(); }
};

} // namespace

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

// 构造期先按四行占位。QPlainTextEdit 的默认 sizeHint 会把窗口撑得很高，
// 真正的高度在 syncManualToFilePages() 里跟文件页对齐。
constexpr int EditorVisibleLines = 4;

} // namespace

SendPanel::SendPanel(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("Surface");
    // 主布局是**左右两栏**（用户钦定："发送按钮放到发送框的右边，定时发送、编码选择也集成到右边"）：
    // 正文在左，编码 / 定时 / 目标 / 发送收进右侧一列。
    // 右列三行同高、左右缘对齐、行距相等，末行与预览齐平。
    auto *root = new QVBoxLayout(this);
    // **左右必须是 6**：数据区外边距 6 + 这里 6 = 12，与端口条内容、监视区内容的
    // 左缩进对齐（用户反馈"边框距离内部元素没有对齐"）。上下 4：发送区贴着内容收紧。
    root->setContentsMargins(6, 4, 6, 4);
    root->setSpacing(4);

    // 目标：A、B 两个独立开关。只选 A 发给 A，只选 B 发给 B，两个都选中就都发。
    // 不再单设 AB：那一档和“两个都选中”是同一件事。
    // 至少留一个选中，不出现“点了发送却没有目标”的空状态。
    m_targetA = new QPushButton(tr("A"));
    m_targetA->setObjectName("sendTargetA");
    m_targetA->setToolTip(tr("发送到端点 A"));
    m_targetB = new QPushButton(tr("B"));
    m_targetB->setObjectName("sendTargetB");
    m_targetB->setToolTip(tr("发送到端点 B"));
    for (auto *button : {m_targetA, m_targetB}) {
        button->setCheckable(true);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
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
    // 先按四行占位，布局完成后改成和文件页一样高。输入框不纵向伸展，
    // 多出来的高度留给数据显示区（见 MainWindow 的分隔条分配）。
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
    m_preview->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_repeat = new QCheckBox(tr("定时发送"));
    m_repeat->setObjectName("repeatSending");
    m_repeat->setToolTip(tr("按固定周期重复发送主输入框的内容；取消勾选即停止"));
    m_interval = new QSpinBox;
    m_interval->setObjectName("repeatInterval"); // 用例要能定位它（"始终可输入、只收数字"）
    m_interval->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_interval->setRange(10, 3600000);
    m_interval->setValue(1000);
    // 单位在框内（"1000 ms"）。QSpinBox 后缀不进入可编辑区，键盘仍然只收数字。
    m_interval->setSuffix(QStringLiteral(" ms"));
    m_interval->setToolTip(tr("发送间隔，单位毫秒"));
    // **始终可输入**（用户钦定）：勾选前就能把间隔设好，勾上即按该值跑。
    // 它是 QSpinBox——非数字键（含粘贴的非法文本）会被它自带的校验器挡掉，不必另做处理。
    m_sendButton = new QPushButton(tr("发送"));
    m_sendButton->setObjectName("PrimaryButton");
    m_sendButton->setIcon(actionStateIcon(QStringLiteral(":/icons/send.svg"), GlyphSize));
    m_sendButton->setIconSize(QSize(GlyphSize, GlyphSize));
    m_sendButton->setEnabled(false);
    m_sendButton->setToolTip(tr("端口未连接"));
    // 主操作靠颜色和字重区分，高度与同排的 A/B、下拉框一致。这里只负责横向伸展。
    m_sendButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_repeatTimer = new QTimer(this);

    // ---- 组装：手动发送、发送文件、接收文件各是一个标签。方向靠选中的标签表达。 ----

    auto *manualPage = new QWidget;
    manualPage->setObjectName(QStringLiteral("manualSendPage"));

    // 右栏三行：怎么发（编码/末尾）→ 发几拍（定时/间隔）→ 发给谁（目标 + 发送）。
    // 两段等比例弹簧把正文高度减去三行之后的空白平分，所以行距相等。
    // 第一行贴着正文上沿；最后一行落在预览那一行里，和字节数同一高度。
    auto *encodeRow = new QHBoxLayout;
    encodeRow->setContentsMargins(0, 0, 0, 0);
    encodeRow->setSpacing(4);
    encodeRow->addWidget(new QLabel(tr("格式")), 0, Qt::AlignVCenter);
    encodeRow->addWidget(m_format, 0, Qt::AlignVCenter);
    encodeRow->addStretch(1);
    encodeRow->addWidget(new QLabel(tr("末尾")), 0, Qt::AlignVCenter);
    encodeRow->addWidget(m_suffix, 0, Qt::AlignVCenter);

    auto *repeatRow = new QHBoxLayout;
    repeatRow->setContentsMargins(0, 0, 0, 0);
    repeatRow->setSpacing(4);
    // 间隔框与「末尾」下拉框同宽并共用右缘，勾选框紧贴在它左边。
    repeatRow->addStretch(1);
    repeatRow->addWidget(m_repeat, 0, Qt::AlignVCenter);
    repeatRow->addWidget(m_interval, 0, Qt::AlignVCenter);

    auto *actionRow = new QHBoxLayout;
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->setSpacing(4);
    for (auto *button : {m_targetA, m_targetB})
        actionRow->addWidget(button, 0, Qt::AlignVCenter);
    actionRow->addWidget(m_sendButton, 1, Qt::AlignVCenter);

    auto *rightColumn = new QVBoxLayout;
    rightColumn->setContentsMargins(0, 0, 0, 0);
    rightColumn->setSpacing(0);
    rightColumn->addLayout(encodeRow);
    rightColumn->addStretch(1);
    rightColumn->addLayout(repeatRow);
    rightColumn->addStretch(1);
    rightColumn->addLayout(actionRow);

    // 字节数与最终内容预览贴着正文下方：它们回答的是"我到底要发什么"。
    // 富文本标签不理会垂直居中，所以套一层与按钮同高的容器，标签在容器里居中。
    auto *previewHost = new QWidget(manualPage);
    previewHost->setObjectName(QStringLiteral("payloadPreviewRow"));
    previewHost->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto *previewRow = new QHBoxLayout(previewHost);
    previewRow->setContentsMargins(0, 0, 0, 0);
    previewRow->setSpacing(0);
    previewRow->addWidget(m_preview, 0, Qt::AlignVCenter);

    // 左栏拿全部剩余宽度（正文越宽越少折行），右栏取自身内容宽度。
    // 右栏跨正文和预览两行，末行才能和预览齐平。
    auto *content = new QGridLayout(manualPage);
    // 与发送/接收文件页同一上边距，页签下面留出一截空隙。
    content->setContentsMargins(0, 4, 0, 0);
    content->setHorizontalSpacing(8);
    content->setVerticalSpacing(4);
    content->addWidget(m_editor, 0, 0);
    content->addWidget(previewHost, 1, 0);
    content->addLayout(rightColumn, 0, 1, 2, 1);
    content->setColumnStretch(0, 1);

    m_fileTransfer = new FileTransferWidget(this);
    m_tabs = new ModeTabs(this);
    m_tabs->setObjectName(QStringLiteral("sendModeTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(manualPage, tr("手动发送"));
    m_tabs->addTab(m_fileTransfer->sendPage(), tr("发送文件"));
    m_tabs->addTab(m_fileTransfer->receivePage(), tr("接收文件"));
    m_tabs->setIconSize(QSize(GlyphSize, GlyphSize));
    // 选中页用和页签文字同一档蓝，其余保持静音灰。QSS 改不了 QIcon 的颜色。
    const auto refreshTabIcons = [this] {
        if (!m_tabs) return;
        const QColor idle(0x64, 0x75, 0x90);
        const QColor active(0x1d, 0x4e, 0xd8);
        const QStringList paths{QStringLiteral(":/icons/send.svg"),
            QStringLiteral(":/icons/file-upload.svg"),
            QStringLiteral(":/icons/file-download.svg")};
        for (int i = 0; i < m_tabs->count() && i < paths.size(); ++i)
            m_tabs->setTabIcon(i, tintedIcon(paths.at(i),
                i == m_tabs->currentIndex() ? active : idle, GlyphSize));
    };
    refreshTabIcons();
    root->addWidget(m_tabs, 0);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, refreshTabIcons](int) {
        refreshTabIcons();
        if (m_tabs) m_tabs->updateGeometry();
        updateGeometry();
        emit preferredHeightChanged(preferredHeight());
    });

    // A、B 取齐宽度。必须在布局挂好之后调，sizeHint 才带得上 QSS 的内边距。
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
    for (auto *button : {m_targetA, m_targetB}) {
        connect(button, &QPushButton::toggled, this, &SendPanel::updateSendAvailability);
        // clicked 在勾选状态已经翻转之后才到。最后一个被点掉时立刻勾回去。
        // 程序里 setChecked 不会发 clicked，缺端点时取消 B 不会被这里挡回去。
        connect(button, &QPushButton::clicked, this, [this, button] {
            if (button->isChecked() || targetMask() != 0) return;
            const QSignalBlocker blocker(button);
            button->setChecked(true);
            updateSendAvailability();
        });
    }
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
    if (m_syncingHeight) return;
    // 字体 / DPI / 翻译变化后 A、B 的 sizeHint 会变，窗口尺寸变化时重算一次。
    unifyActionWidths();
}

void SendPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 样式抛光之后控件的 sizeHint 才是最终值，这里再对齐一次三页高度。
    unifyActionWidths();
}

void SendPanel::unifyActionWidths()
{
    const QList<QWidget *> controls{
        m_format, m_suffix, m_interval, m_targetA, m_targetB, m_sendButton};

    // 下拉框按全部条目取宽，字体 / DPI / 翻译变化时跟着走。
    // 这些 sizeHint 按内容计算，不受下面写死的宽高影响，所以不必先清掉固定尺寸
    // （清掉会在 resizeEvent 里来回触发布局）。
    // 格式和末尾取同一个宽度，间隔框也用这个宽度，右缘才能落在同一条线上。
    if (m_format) fitComboToItems(m_format);
    if (m_suffix) fitComboToItems(m_suffix);
    int fieldWidth = 0;
    if (m_format) fieldWidth = qMax(fieldWidth, m_format->sizeHint().width());
    if (m_suffix) fieldWidth = qMax(fieldWidth, m_suffix->sizeHint().width());
    // 间隔框要容下量程最大值和 " ms"，否则单位会被右缘裁掉。
    if (m_interval) fieldWidth = qMax(fieldWidth, m_interval->sizeHint().width());

    int controlHeight = 0;
    for (auto *control : controls) {
        if (!control) continue;
        controlHeight = qMax(controlHeight, control->sizeHint().height());
    }
    if (controlHeight > 0) {
        for (auto *control : controls)
            if (control) control->setFixedHeight(controlHeight);
        // 预览容器与按钮同高，标签在里面垂直居中，底行才是一条直线。
        if (auto *previewRow = findChild<QWidget *>(QStringLiteral("payloadPreviewRow")))
            previewRow->setFixedHeight(controlHeight);
    }
    if (fieldWidth > 0) {
        for (QWidget *field : {static_cast<QWidget *>(m_format),
                 static_cast<QWidget *>(m_suffix), static_cast<QWidget *>(m_interval)}) {
            if (field) field->setFixedWidth(fieldWidth);
        }
    }

    // 只取齐 A、B 的宽度。发送按钮吃掉本行剩余宽度，不跟字母开关绑成同一个宽。
    for (auto *button : {m_targetA, m_targetB}) {
        if (button) button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    unifyButtonWidths({m_targetA, m_targetB});
    syncManualToFilePages();
}

void SendPanel::syncManualToFilePages()
{
    if (m_syncingHeight || !m_editor || !m_tabs || !m_fileTransfer) return;
    auto *manual = findChild<QWidget *>(QStringLiteral("manualSendPage"));
    auto *grid = manual ? qobject_cast<QGridLayout *>(manual->layout()) : nullptr;
    auto *preview = manual ? manual->findChild<QWidget *>(QStringLiteral("payloadPreviewRow")) : nullptr;
    if (!grid || !preview) return;

    const int fileH = qMax(
        m_fileTransfer->sendPage() ? m_fileTransfer->sendPage()->sizeHint().height() : 0,
        m_fileTransfer->receivePage() ? m_fileTransfer->receivePage()->sizeHint().height() : 0);
    if (fileH <= 0) return;

    const QMargins margins = grid->contentsMargins();
    const int previewH = qMax(preview->sizeHint().height(), preview->minimumSizeHint().height());
    int editorH = fileH - margins.top() - margins.bottom() - grid->verticalSpacing() - previewH;
    const int lineFloor = m_editor->fontMetrics().lineSpacing()
        + 2 * (EditorPaddingPx + EditorDocumentMarginPx + m_editor->frameWidth());
    if (editorH < lineFloor) editorH = lineFloor;

    m_syncingHeight = true;
    m_editor->setFixedHeight(editorH);
    // 布局自己还会加一点边，按差一次补回，页高才和文件页重合。
    for (int i = 0; i < 2; ++i) {
        const int delta = fileH - manual->sizeHint().height();
        if (delta == 0) break;
        editorH += delta;
        if (editorH < lineFloor) break;
        m_editor->setFixedHeight(editorH);
    }
    m_syncingHeight = false;

    if (!isVisible()) return;
    const int preferred = preferredHeight();
    if (preferred == m_appliedHeight) return;
    m_appliedHeight = preferred;
    emit preferredHeightChanged(preferred);
}

void SendPanel::setEndpointOpen(int index, bool open)
{
    if (index < 0 || index >= 2) return;
    m_openFlags[index] = open;
    if (m_fileTransfer) m_fileTransfer->setEndpointOpen(index, open);
    updateSendAvailability();
}

void SendPanel::setRuntime(EndpointRuntime *runtime)
{
    if (m_fileTransfer) m_fileTransfer->setRuntime(runtime);
}

void SendPanel::handleRawDataReceived(int index, const Traffic::Source &source,
    const QByteArray &payload)
{
    if (m_fileTransfer) m_fileTransfer->rawDataReceived(index, source, payload);
}

void SendPanel::updateTargets()
{
    // 缺 B 时 B 不可选。选中态跟着可用性走，避免“勾着一个不存在的端点”。
    m_targetA->setEnabled(m_ports.size() >= 1);
    m_targetB->setEnabled(m_ports.size() >= 2);
    if (m_targetB->isChecked() && !m_targetB->isEnabled()) {
        const QSignalBlocker blocker(m_targetB);
        m_targetB->setChecked(false);
    }
    if (targetMask() == 0 && m_targetA->isEnabled()) {
        const QSignalBlocker blocker(m_targetA);
        m_targetA->setChecked(true);
    }
    updateSendAvailability();
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
    // 两个都选中、只连了一端时，按钮仍可用；没连上的那一端会留下失败记录。
    const bool anySelectedOpen =
        (m_targetA->isChecked() && m_openFlags[0])
        || (m_targetB->isChecked() && m_openFlags[1]);
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

int SendPanel::preferredHeight() const
{
    const int page = tallestPageHeight(m_tabs);
    const int bar = m_tabs && m_tabs->tabBar() ? m_tabs->tabBar()->sizeHint().height() : 0;
    const auto margins = layout() ? layout()->contentsMargins() : QMargins();
    return page + bar + margins.top() + margins.bottom() + 8;
}

void SendPanel::setPreferences(PreferencesStore *store)
{
    m_preferences = store;
    if (m_fileTransfer) m_fileTransfer->setPreferences(store);
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
