#include "portselectorwidget.h"
#include "domain/networkaddress.h"
#include "ui/common/combometrics.h"
#include "ui/common/endpointformat.h"
#include "ui/common/payloadcodec.h"
#include "ui/common/tintedicon.h"

#include <QCheckBox>
#include <QComboBox>
#include <QAbstractItemView>
#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <limits>

namespace {

// The advanced toggle is icon-only, so the glyph has to carry the whole affordance. 20px is
// the tallest glyph that still fits the row: with the button's own 4px padding that makes the
// toggle 28px high, exactly the height of the connect/listen button beside it, and the icon
// fills the button's whole content box.
constexpr int AdvancedIconSize = 20;

// Action buttons keep their text; the status glyph rides beside it at the usual icon size.
constexpr int ActionIconSize = 16;

class BaudRateValidator final : public QValidator
{
public:
    explicit BaudRateValidator(QObject *parent) : QValidator(parent) {}

    State validate(QString &input, int &) const override
    {
        if (input.isEmpty()) return Intermediate;
        for (const auto c : input)
            if (c < QLatin1Char('0') || c > QLatin1Char('9')) return Invalid;
        bool ok = false;
        const auto value = input.toULongLong(&ok);
        if (!ok || value > std::numeric_limits<qint32>::max()) return Invalid;
        return value > 0 ? Acceptable : Intermediate;
    }
};

// QAbstractSpinBox::sizeHint() counts the range text and the padding but not the stylesheet's
// border, so the edit field ends up a couple of pixels narrower than the widest value ("64"
// for the client limit, "65535" for a port). Rather than pinning a pixel width that would stop
// following the font, report the width the range really needs: measure the box the style draws
// around the field and add it to the widest value of the range, which covers prefix and suffix
// too, so a value typed after the widget was built stays readable.
class PortSpinBox final : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;

    QSize sizeHint() const override
    {
        const QSize base = QSpinBox::sizeHint();
        QStyleOptionSpinBox option;
        option.initFrom(this);
        option.rect = QRect(QPoint(0, 0), base);
        const QRect field = style()->subControlRect(
            QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxEditField, this);
        const int chrome = option.rect.width() - field.width();
        const auto widthOf = [this](int value) {
            return fontMetrics().horizontalAdvance(prefix() + QString::number(value) + suffix());
        };
        const int widest = qMax(widthOf(minimum()), widthOf(maximum()));
        return QSize(qMax(base.width(), widest + chrome), base.height());
    }
};

// Material Design Icons are recoloured through src/ui/common/tintedicon.h; the advanced toggle
// keeps its own two colours because it sits on the row background rather than a filled button.
QIcon tintedIcon(const QString &path, const QColor &color)
{
    QIcon icon;
    // Display size plus a 2x variant so scaled displays stay crisp.
    for (const int size : {AdvancedIconSize, AdvancedIconSize * 2})
        icon.addPixmap(tintedPixmap(path, color, size));
    return icon;
}

} // namespace


PortSelectorWidget::PortSelectorWidget(QWidget *parent) : QWidget(parent)
{
    m_loading = true;
    setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 5, 12, 5);
    layout->setSpacing(4);

    // Main row: every parameter needed to connect stays on one line, no selection step.
    auto *row = new QHBoxLayout;
    row->setSpacing(7);
    m_badge = new QLabel;
    m_badge->setObjectName("EndpointBadge");
    m_badge->setAlignment(Qt::AlignCenter);
    m_badge->setFixedSize(24, 24);
    row->addWidget(m_badge);

    m_type = new QComboBox;
    m_type->setObjectName("portType");
    // 类型身份绑定到 itemData 的枚举值，而不是条目位置：排序、翻译或插入
    // 分组都不会改变已保存配置的含义（ARCHITECTURE §6.4 的映射规则）。
    for (int kind = PortDraft::Serial; kind <= PortDraft::Udp; ++kind)
        m_type->addItem(portKindName(kind), kind);
    fitComboToItems(m_type);
    m_type->setToolTip(tr("端点类型；切换后参数随之更换"));
    row->addWidget(m_type);

    m_pages = new QStackedWidget;
    row->addWidget(m_pages, 1);

    // The leading label of every type shares one width, so A and B align despite different text.
    QList<QLabel *> leadLabels;
    const auto addInlineLabel = [&leadLabels](QHBoxLayout *target, const QString &text,
                                             QWidget *field, bool lead = false) {
        auto *label = new QLabel(text);
        label->setObjectName(field->objectName() + "Label");
        label->setBuddy(field);
        if (lead) {
            label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            leadLabels.append(label);
        }
        target->addWidget(label);
        target->addWidget(field, lead ? 1 : 0);
    };
    auto makePage = [this](QStackedWidget *stack) {
        auto *page = new QWidget;
        auto *pageLayout = new QHBoxLayout(page);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        pageLayout->setSpacing(8);
        stack->addWidget(page);
        return pageLayout;
    };

    auto *serial = makePage(m_pages);
    m_serialPorts = addChoice("serial.port", {});
    m_serialPorts->setPlaceholderText(tr("请选择串口"));
    // Grows with the row so a long friendly name stays fully readable.
    m_serialPorts->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_serialPorts->setMinimumWidth(200);
    addInlineLabel(serial, tr("串口"), m_serialPorts, true);
    // 刷新按钮已移除：扫描由组合根的定时器周期驱动，设备插拔自动出现在列表里。
    auto *baud = addChoice("serial.baud", {"9600", "38400", "115200", "230400", "921600"}, true);
    baud->setValidator(new BaudRateValidator(baud));
    baud->lineEdit()->setMaxLength(10);
    baud->lineEdit()->setInputMethodHints(Qt::ImhDigitsOnly);
    // Editable, so it sizes itself from the preset rates instead of a frozen pixel width.
    baud->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    addInlineLabel(serial, tr("波特率"), baud);
    serial->addStretch();

    auto *client = makePage(m_pages);
    auto *host = addAddress("client.host", true);
    host->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    host->setMinimumWidth(200);
    addInlineLabel(client, tr("远端地址"), host, true);
    addInlineLabel(client, tr("端口"), addNumber("client.port", 1, 65535));

    auto *server = makePage(m_pages);
    auto *bind = addAddress("server.address");
    bind->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    bind->setMinimumWidth(200);
    addInlineLabel(server, tr("本地地址"), bind, true);
    addInlineLabel(server, tr("监听端口"), addNumber("server.port", 0, 65535));

    auto *udp = makePage(m_pages);
    auto *udpBind = addAddress("udp.address");
    udpBind->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    udpBind->setMinimumWidth(130);
    addInlineLabel(udp, tr("本地地址"), udpBind, true);
    addInlineLabel(udp, tr(":"), addNumber("udp.port", 0, 65535));
    auto *udpTarget = addAddress("udp.target");
    udpTarget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    udpTarget->setMinimumWidth(130);
    addInlineLabel(udp, tr("目标地址"), udpTarget, true);
    addInlineLabel(udp, tr(":"), addNumber("udp.targetPort", 1, 65535));

    int leadWidth = 0;
    for (auto *label : leadLabels) {
        label->ensurePolished();
        leadWidth = qMax(leadWidth, label->fontMetrics().horizontalAdvance(label->text()));
    }
    for (auto *label : leadLabels)
        label->setFixedWidth(leadWidth);

    m_advanced = new QToolButton;
    m_advanced->setObjectName("advancedToggle");
    m_advanced->setCheckable(true);
    m_advanced->setIconSize(QSize(AdvancedIconSize, AdvancedIconSize));
    // The glyph replaces the word, so the label moves to the accessible name.
    m_advanced->setAccessibleName(tr("高级参数"));
    // Fills the row like the action button, so both keep the same height and top edge.
    m_advanced->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    // A tool button paints its arrow instead of its icon, so the open/closed state is
    // carried by the icon colour and the panel itself, not by a second glyph.
    m_advancedIconIdle = tintedIcon(QStringLiteral(":/icons/tune.svg"), QColor("#647590"));
    m_advancedIconActive = tintedIcon(QStringLiteral(":/icons/tune.svg"), QColor("#3465d5"));
    updateAdvancedToggle();
    row->addWidget(m_advanced);

    // The button is the state: its text and colour cover closed/open, so no status label.
    m_action = new QPushButton;
    m_action->setObjectName("endpointAction");
    m_action->setMinimumWidth(78);
    m_action->setEnabled(false);
    m_action->setIconSize(QSize(ActionIconSize, ActionIconSize));
    // 动作只表达"意图"：开/关/取消都走 actionRequested，方向由组合根决定。
    connect(m_action, &QPushButton::clicked, this, &PortSelectorWidget::actionRequested);
    // The row is as tall as its tallest control; filling it instead of centring keeps the
    // button's top edge on the same line as every other control in the bar.
    m_action->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    row->addWidget(m_action);
    layout->addLayout(row);

    // Advanced row: opens in place under this endpoint, never moves the data area.
    m_advancedPanel = new QWidget;
    m_advancedPanel->setObjectName("advancedPanel");
    // 纵向：上排是各类型专属参数，下排是与类型无关的接收组帧。
    auto *advancedLayout = new QVBoxLayout(m_advancedPanel);
    advancedLayout->setContentsMargins(34, 0, 0, 2);
    advancedLayout->setSpacing(4);
    m_advancedPages = new QStackedWidget;
    advancedLayout->addWidget(m_advancedPages);

    auto *serialMore = makePage(m_advancedPages);
    addInlineLabel(serialMore, tr("数据位"), addChoice("serial.bits", {"5", "6", "7", "8"}));
    addInlineLabel(serialMore, tr("校验"), addChoice("serial.parity", {"None", "Even", "Odd", "Mark", "Space"}));
    addInlineLabel(serialMore, tr("停止位"), addChoice("serial.stop", {"1", "1.5", "2"}));
    addInlineLabel(serialMore, tr("流控"), addChoice("serial.flow", {"None", "RTS/CTS", "XON/XOFF"}));
    serialMore->addStretch();

    auto *clientMore = makePage(m_advancedPages);
    auto *timeout = addNumber("client.timeout", 100, 60000);
    timeout->setSuffix(tr(" ms"));
    addInlineLabel(clientMore, tr("连接超时"), timeout);
    clientMore->addStretch();

    auto *serverMore = makePage(m_advancedPages);
    addInlineLabel(serverMore, tr("客户端上限"), addNumber("server.clients", 1, 64));
    m_peerHint = new QLabel(tr("暂无客户端；启动监听后在此管理连接与发送目标。"));
    m_peerHint->setObjectName("Muted");
    serverMore->addWidget(m_peerHint);
    serverMore->addStretch();

    auto *udpMore = makePage(m_advancedPages);
    auto *udpHint = new QLabel(tr("目标地址可留空，表示只接收；UDP 保留数据报边界。"));
    udpHint->setObjectName("Muted");
    udpMore->addWidget(udpHint);
    udpMore->addStretch();

    // 接收组帧与端点类型无关（数据报端点不经过它），因此作为通用行挂在
    // 类型专属页之下：串口/TCP 客户端用它把字节流切成协议帧。
    advancedLayout->addWidget(buildFramingRow());

    layout->addWidget(m_advancedPanel);
    m_advancedPanel->hide();
    connect(m_advanced, &QToolButton::toggled, this, [this](bool checked) {
        m_advancedPanel->setVisible(checked);
        updateAdvancedToggle();
        updateGeometry();
    });

    // A stack otherwise reserves room for the widest hidden page.
    const auto trackPageSize = [](QStackedWidget *stack, int index) {
        for (int i = 0; i < stack->count(); ++i)
            stack->widget(i)->setSizePolicy(QSizePolicy::Expanding,
                i == index ? QSizePolicy::Preferred : QSizePolicy::Ignored);
        stack->updateGeometry();
    };
    const auto updatePageSize = [this, trackPageSize](int index) {
        trackPageSize(m_pages, index);
        trackPageSize(m_advancedPages, index);
        updateGeometry();
    };
    connect(m_type, &QComboBox::currentIndexChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_type, &QComboBox::currentIndexChanged, m_advancedPages, &QStackedWidget::setCurrentIndex);
    connect(m_type, &QComboBox::currentIndexChanged, this, updatePageSize);
    updatePageSize(m_pages->currentIndex());

    connect(m_type, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_loading) return;
        m_draft.kind = m_type->currentData().toInt();
        updateActionLabels();
        notifyChange();
    });
    setAddressHistory({}, {});
    setDraft(PortDraft{});
    m_initialised = true;
}

void PortSelectorWidget::setEndpointLabel(const QString &label)
{
    m_badge->setText(label);
    m_badge->setToolTip(tr("端点 %1").arg(label));
}

void PortSelectorWidget::setConnectionOpen(bool open)
{
    if (m_open == open) return;
    m_open = open;
    // 打开期间锁定类型与参数：连接中的端点不接受"边连边改"，
    // 想换配置先关闭——状态机因此只需要处理干净的状态边界。
    m_type->setEnabled(!open);
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it)
        it.value()->setEnabled(!open);
    updateActionLabels();
    updateValidation();
}

void PortSelectorWidget::updateActionLabels()
{
    const int kind = m_type->currentData().toInt();
    m_action->setText(portActionName(kind, m_open));
    m_action->setIcon(actionStateIcon(portActionIconName(kind, m_open), ActionIconSize));
    // Blue invites the connect action; red marks the one that tears it down.
    m_action->setProperty("open", m_open);
    m_action->style()->unpolish(m_action);
    m_action->style()->polish(m_action);
    const auto current = draft();
    m_badge->setToolTip(tr("端点 %1\n%2")
                            .arg(m_badge->text(), portDraftDetails(current)));
}

void PortSelectorWidget::updateAdvancedToggle()
{
    const bool expanded = m_advanced->isChecked();
    const QIcon icon = expanded ? m_advancedIconActive : m_advancedIconIdle;
    // Without the SVG image plugin the glyph cannot be rendered, so the word comes back
    // instead of leaving an empty, unlabelled button.
    m_advanced->setToolButtonStyle(icon.isNull() ? Qt::ToolButtonTextOnly : Qt::ToolButtonIconOnly);
    m_advanced->setText(tr("高级"));
    m_advanced->setIcon(icon);
    m_advanced->setToolTip(expanded ? tr("收起该类型的高级参数") : tr("展开该类型的高级参数"));
}

QComboBox *PortSelectorWidget::addChoice(const QString &key, const QStringList &items, bool editable)
{
    auto *field = new QComboBox;
    field->setObjectName(key);
    field->addItems(items);
    field->setEditable(editable);
    field->setInsertPolicy(QComboBox::NoInsert);
    field->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // Fixed choice lists are sized from their own items; dynamic (serial ports) and editable
    // fields take a size policy from their owner instead.
    if (!editable && !items.isEmpty()) fitComboToItems(field);
    m_fields.insert(key, field);
    connect(field, &QComboBox::currentTextChanged, this, &PortSelectorWidget::notifyChange);
    return field;
}

QComboBox *PortSelectorWidget::addAddress(const QString &key, bool allowHostName)
{
    auto *field = addChoice(key, {}, true);
    field->setMaxVisibleItems(10);
    auto *edit = field->lineEdit();
    edit->setPlaceholderText(allowHostName ? tr("输入或选择 IP / 主机名") : tr("输入或选择 IPv4 地址"));
    edit->setMaxLength(allowHostName ? 253 : 15);
    field->setValidator(new QRegularExpressionValidator(
        allowHostName ? hostNamePattern() : ipv4AddressPattern(), field));
    connect(edit, &QLineEdit::textEdited, this, [this, key] { m_editedAddresses.insert(key); });
    connect(edit, &QLineEdit::editingFinished, this, [this, key] {
        if (m_editedAddresses.contains(key)) rememberAddress(key);
    });
    connect(field, &QComboBox::activated, this, [this, key] { rememberAddress(key); });
    return field;
}

void PortSelectorWidget::setAddressHistory(const QStringList &remote, const QStringList &local)
{
    for (const auto &key : {QStringLiteral("client.host"), QStringLiteral("server.address"),
                            QStringLiteral("udp.address"), QStringLiteral("udp.target")}) {
        auto *combo = qobject_cast<QComboBox *>(m_fields.value(key));
        const QSignalBlocker comboBlocker(combo), editBlocker(combo->lineEdit());
        const auto text = combo->currentText();
        const auto cursor = combo->lineEdit()->cursorPosition();
        const bool localAddress = key == "server.address" || key == "udp.address";
        QStringList choices;
        for (const auto &address : (localAddress ? local : remote))
            if (isValidNetworkAddress(address, key == "client.host") && !choices.contains(address))
                choices.append(address);
        for (const auto &address : (localAddress ? QStringList{"127.0.0.1", "0.0.0.0"} : QStringList{"127.0.0.1"}))
            if (!choices.contains(address)) choices.append(address);
        combo->clear();
        combo->addItems(choices);
        combo->setCurrentIndex(-1);
        combo->setEditText(text);
        combo->lineEdit()->setCursorPosition(cursor);
    }
}

void PortSelectorWidget::rememberAddress(const QString &key)
{
    if (m_loading) return;
    m_editedAddresses.remove(key);
    const auto *combo = qobject_cast<QComboBox *>(m_fields.value(key));
    const auto address = combo->currentText();
    if (isValidNetworkAddress(address, key == "client.host"))
        emit addressCommitted(address, key == "server.address" || key == "udp.address");
}

void PortSelectorWidget::commitAddressHistory()
{
    const auto edited = m_editedAddresses;
    for (const auto &key : edited) rememberAddress(key);
}

void PortSelectorWidget::setPeerList(const QStringList &peers)
{
    if (!m_peerHint) return;
    if (peers.isEmpty()) {
        m_peerHint->setText(tr("暂无客户端；启动监听后在此管理连接与发送目标。"));
        m_peerHint->setToolTip(QString());
        return;
    }
    m_peerHint->setText(tr("已连接 %1 个客户端").arg(peers.size()));
    m_peerHint->setToolTip(peers.join('\n'));
}

void PortSelectorWidget::selectSerialPort(const QString &portName)
{
    const QSignalBlocker blocker(m_serialPorts);
    m_serialPorts->clear();
    for (const auto &port : m_availablePorts) {
        const auto friendlyName = !port.description.isEmpty() ? port.description
            : !port.manufacturer.isEmpty() ? port.manufacturer : tr("串口设备");
        const auto label = port.portName + " · " + friendlyName;
        m_serialPorts->addItem(label, port.portName);
        const int index = m_serialPorts->count() - 1;
        m_serialPorts->setItemData(index, true, Qt::UserRole + 1);
        m_serialPorts->setItemData(index, label + "\n" + port.systemLocation, Qt::ToolTipRole);
    }
    int index = m_serialPorts->findData(portName);
    if (index < 0 && !portName.isEmpty()) {
        m_serialPorts->addItem(portName + (m_serialScanComplete ? tr(" · 设备不可用") : tr(" · 待扫描")), portName);
        index = m_serialPorts->count() - 1;
        m_serialPorts->setItemData(index, false, Qt::UserRole + 1);
    }
    m_serialPorts->setCurrentIndex(index);
    m_serialPorts->setPlaceholderText(m_serialScanComplete && m_availablePorts.isEmpty()
        ? tr("未发现串口，等待自动扫描") : tr("请选择串口"));
    int popupWidth = m_serialPorts->width();
    for (int i = 0; i < m_serialPorts->count(); ++i)
        popupWidth = qMax(popupWidth, m_serialPorts->fontMetrics().horizontalAdvance(m_serialPorts->itemText(i)) + 32);
    m_serialPorts->view()->setMinimumWidth(qMin(popupWidth, 640));
    // The row elides long names; the tooltip always carries the full label.
    m_serialPorts->setToolTip(m_serialPorts->currentText());
}

void PortSelectorWidget::setSerialPorts(const QList<SerialPortDescriptor> &ports)
{
    const auto selected = m_serialPorts->currentData().toString();
    m_availablePorts = ports;
    m_serialScanComplete = true;
    selectSerialPort(selected);
    updateActionLabels();
    updateValidation();
    // A scan can invalidate the draft (device removed); owners gate the bridge on this.
    emit configurationChanged();
}

QSpinBox *PortSelectorWidget::addNumber(const QString &key, int minimum, int maximum)
{
    auto *field = new PortSpinBox;
    field->setObjectName(key);
    field->setButtonSymbols(QAbstractSpinBox::NoButtons);
    field->setRange(minimum, maximum);
    // PortSpinBox::sizeHint() covers the range text plus the stylesheet box, so Fixed can ask
    // the layout for exactly that and the two endpoint rows stay aligned without a pixel width.
    field->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto numericLocale = QLocale::c();
    numericLocale.setNumberOptions(QLocale::RejectGroupSeparator);
    field->setLocale(numericLocale);
    field->setInputMethodHints(Qt::ImhDigitsOnly);
    m_fields.insert(key, field);
    connect(field, &QSpinBox::valueChanged, this, &PortSelectorWidget::notifyChange);
    // 行内文本变化（清空、非法中间态）只需要重算校验；值本身没变时
    // 不必再走 notifyChange 触发第二次 configurationChanged 广播。
    connect(field->findChild<QLineEdit *>(), &QLineEdit::textChanged, this, &PortSelectorWidget::updateValidation);
    return field;
}

QWidget *PortSelectorWidget::buildFramingRow()
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *label = new QLabel(tr("接收组帧"));
    label->setObjectName("Muted");
    layout->addWidget(label);

    m_framingMode = new QComboBox;
    m_framingMode->setObjectName(QStringLiteral("framingMode"));
    m_framingMode->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_framingMode->addItem(tr("仅兜底输出"), int(ReceiveFraming::Passthrough));
    m_framingMode->addItem(tr("结束符拆分"), int(ReceiveFraming::Delimiter));
    m_framingMode->addItem(tr("长度前缀 u16 小端"), int(ReceiveFraming::LengthPrefixU16LE));
    layout->addWidget(m_framingMode);

    // 结束符按 HEX 输入（0D 0A / 0D0A 均可）：不可见字节必须能被精确表达。
    m_framingDelimiter = new QLineEdit;
    m_framingDelimiter->setObjectName(QStringLiteral("framingDelimiter"));
    m_framingDelimiter->setPlaceholderText(tr("结束符 HEX，如 0D 0A"));
    m_framingDelimiter->setMaximumWidth(170);
    layout->addWidget(m_framingDelimiter);

    // 用 PortSpinBox（sizeHint 覆盖量程两端）：写死宽度会让最大值显示不全，
    // tests/ui 的 portControlsShowTheirOwnContent 就是这条底线。
    auto numericLocale = QLocale::c();
    numericLocale.setNumberOptions(QLocale::RejectGroupSeparator);
    m_framingIdle = new PortSpinBox;
    m_framingIdle->setObjectName(QStringLiteral("framingIdle"));
    m_framingIdle->setRange(10, 60000);
    m_framingIdle->setSuffix(tr(" ms"));
    m_framingIdle->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_framingIdle->setLocale(numericLocale);
    m_framingIdle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(m_framingIdle);

    m_framingThreshold = new PortSpinBox;
    m_framingThreshold->setObjectName(QStringLiteral("framingThreshold"));
    m_framingThreshold->setRange(16, 65536);
    m_framingThreshold->setSuffix(tr(" 字节"));
    m_framingThreshold->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_framingThreshold->setLocale(numericLocale);
    m_framingThreshold->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(m_framingThreshold);

    m_framingKeepBoundary = new QCheckBox(tr("保留结束符"));
    m_framingKeepBoundary->setObjectName(QStringLiteral("framingKeepBoundary"));
    layout->addWidget(m_framingKeepBoundary);

    m_framingLogHints = new QCheckBox(tr("记录组帧提示日志"));
    m_framingLogHints->setObjectName(QStringLiteral("framingLogHints"));
    m_framingLogHints->setToolTip(tr("兜底输出（空闲超时、缓冲阈值、关闭收尾）时发一条警告事件"));
    layout->addWidget(m_framingLogHints);
    layout->addStretch();

    const auto changed = [this] { notifyChange(); };
    connect(m_framingMode, &QComboBox::currentIndexChanged, this, [this] {
        syncFramingControls();
        notifyChange();
    });
    connect(m_framingDelimiter, &QLineEdit::textChanged, this, changed);
    connect(m_framingIdle, &QSpinBox::valueChanged, this, changed);
    connect(m_framingThreshold, &QSpinBox::valueChanged, this, changed);
    connect(m_framingKeepBoundary, &QCheckBox::toggled, this, changed);
    connect(m_framingLogHints, &QCheckBox::toggled, this, changed);
    return row;
}

void PortSelectorWidget::syncFramingControls()
{
    const auto mode = ReceiveFraming::Mode(m_framingMode->currentData().toInt());
    const bool delimiter = mode == ReceiveFraming::Delimiter;
    const bool length = mode == ReceiveFraming::LengthPrefixU16LE;
    m_framingDelimiter->setVisible(delimiter);
    m_framingKeepBoundary->setVisible(delimiter || length);
    // 同一个开关在两种模式下语义不同：结束符模式下是"保留结束符"，
    // 长度前缀模式下是"保留长度帧头"。
    m_framingKeepBoundary->setText(delimiter ? tr("保留结束符") : tr("保留长度帧头"));
}

void PortSelectorWidget::applyFramingToDraft(PortDraft &draft) const
{
    const auto mode = ReceiveFraming::Mode(m_framingMode->currentData().toInt());
    draft.framing.mode = mode;
    draft.framing.idleTimeoutMs = m_framingIdle->value();
    draft.framing.bufferThresholdBytes = m_framingThreshold->value();
    draft.framing.includeDelimiter = m_framingKeepBoundary->isChecked();
    draft.framing.includeLengthPrefix = m_framingKeepBoundary->isChecked();
    draft.framing.logHints = m_framingLogHints->isChecked();
    // 结束符按 HEX 解析；解析失败时留空（校验会拦住结束符模式），不写半个序列。
    const auto parsed = PayloadCodec::parse(m_framingDelimiter->text(), PayloadCodec::Format::Hex, {});
    draft.framing.delimiter = parsed.valid() ? parsed.bytes : QByteArray();
}

void PortSelectorWidget::setDraft(const PortDraft &draft)
{
    commitAddressHistory();
    m_loading = true;
    m_draft = draft;
    m_framingMode->setCurrentIndex(m_framingMode->findData(int(draft.framing.mode)));
    m_framingDelimiter->setText(QString::fromLatin1(draft.framing.delimiter.toHex(' ').toUpper()));
    m_framingIdle->setValue(draft.framing.idleTimeoutMs);
    m_framingThreshold->setValue(draft.framing.bufferThresholdBytes);
    m_framingKeepBoundary->setChecked(draft.framing.mode == ReceiveFraming::Delimiter
        ? draft.framing.includeDelimiter : draft.framing.includeLengthPrefix);
    m_framingLogHints->setChecked(draft.framing.logHints);
    syncFramingControls();
    // 通过 itemData 定位类型，草稿里的 kind 与下拉框顺序解耦；越界值回落串口。
    const int typeIndex = m_type->findData(draft.kind);
    m_type->setCurrentIndex(typeIndex >= 0 ? typeIndex : 0);
    const QVariantMap defaults{{"serial.port", ""}, {"serial.baud", "115200"},
        {"serial.bits", "8"}, {"serial.parity", "None"}, {"serial.stop", "1"}, {"serial.flow", "None"},
        {"client.host", "127.0.0.1"}, {"client.port", 9000}, {"client.timeout", 5000},
        {"server.address", "127.0.0.1"}, {"server.port", 9000}, {"server.clients", 64},
        {"udp.address", "127.0.0.1"}, {"udp.port", 9001}, {"udp.target", "127.0.0.1"}, {"udp.targetPort", 9002}};
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) {
        const auto value = draft.values.value(it.key(), defaults.value(it.key()));
        if (it.key() == "serial.port")
            selectSerialPort(value.toString());
        else if (auto *combo = qobject_cast<QComboBox *>(it.value()))
            combo->setCurrentText(value.toString());
        else if (auto *edit = qobject_cast<QLineEdit *>(it.value()))
            edit->setText(value.toString());
        else if (auto *spin = qobject_cast<QSpinBox *>(it.value())) {
            bool ok = false;
            const QString text = value.toString();
            const int number = text.toInt(&ok);
            if (ok && number >= spin->minimum() && number <= spin->maximum()
                && QRegularExpression(QStringLiteral("^[0-9]+$")).match(text).hasMatch()) {
                spin->setValue(number);
            } else {
                // Keep invalid imported values visible; clamping could silently change a target.
                spin->findChild<QLineEdit *>()->setText(text + spin->suffix());
            }
        }
    }
    m_loading = false;
    updateActionLabels();
    updateValidation();
}

PortDraft PortSelectorWidget::draft() const
{
    auto result = m_draft;
    result.kind = m_type->currentData().toInt();
    applyFramingToDraft(result);
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) {
        if (it.key() == "serial.port")
            result.values.insert(it.key(), m_serialPorts->currentData().toString());
        else if (auto *combo = qobject_cast<QComboBox *>(it.value()))
            result.values.insert(it.key(), combo->currentText());
        else if (auto *edit = qobject_cast<QLineEdit *>(it.value()))
            result.values.insert(it.key(), edit->text());
        else if (auto *spin = qobject_cast<QSpinBox *>(it.value()))
            result.values.insert(it.key(), spin->hasAcceptableInput() ? QVariant(spin->value())
                                                                    : QVariant(spin->cleanText()));
    }
    return result;
}

void PortSelectorWidget::notifyChange()
{
    if (!m_loading) {
        updateActionLabels();
        updateValidation();
        emit configurationChanged();
    }
}

void PortSelectorWidget::updateValidation()
{
    QMap<QWidget *, QString> errors;
    const auto requireText = [this, &errors](const QString &key, const QString &message, bool optional = false) {
        auto *field = m_fields.value(key);
        const auto *combo = qobject_cast<QComboBox *>(field);
        const auto *edit = combo ? combo->lineEdit() : qobject_cast<QLineEdit *>(field);
        if (optional && edit->text().isEmpty()) return;
        if (edit->text().trimmed().isEmpty() || !edit->hasAcceptableInput()) errors.insert(field, message);
    };
    switch (m_type->currentData().toInt()) {
    case 0:
        if (m_serialPorts->currentData().toString().isEmpty())
            errors.insert(m_serialPorts, tr("串口：请选择设备；插入设备后将自动扫描"));
        else if (m_serialScanComplete && !m_serialPorts->currentData(Qt::UserRole + 1).toBool())
            errors.insert(m_serialPorts, tr("串口：所选设备不可用，请插入设备或重新选择"));
        requireText("serial.baud", tr("波特率：请输入 1～2147483647 的整数"));
        break;
    case 1: {
        requireText("client.host", tr("远端地址：请输入有效的 IPv4 地址或主机名"));
        auto *host = qobject_cast<QComboBox *>(m_fields.value("client.host"));
        if (!isValidNetworkAddress(host->currentText(), true))
            errors.insert(host, tr("远端地址：请输入有效的 IPv4 地址或主机名"));
        break;
    }
    case 2:
        requireText("server.address", tr("本地地址：请输入有效的 IPv4 地址"));
        break;
    case 3:
        requireText("udp.address", tr("本地地址：请输入有效的 IPv4 地址"));
        requireText("udp.target", tr("目标地址：请输入有效的 IPv4 地址；仅接收时可留空"), true);
        break;
    }
    // 组帧校验与类型无关：结束符模式必须给出可解析且非空的结束符。
    if (ReceiveFraming::Mode(m_framingMode->currentData().toInt()) == ReceiveFraming::Delimiter) {
        const auto parsed = PayloadCodec::parse(m_framingDelimiter->text(),
            PayloadCodec::Format::Hex, {});
        if (!parsed.valid() || parsed.bytes.isEmpty())
            errors.insert(m_framingDelimiter, tr("结束符：请输入 HEX 字节，如 0D 0A"));
    }
    const QString prefix = QStringList{"serial.", "client.", "server.", "udp."}.value(m_type->currentData().toInt());
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) {
        const auto *spin = qobject_cast<QSpinBox *>(it.value());
        if (spin && it.key().startsWith(prefix) && !spin->hasAcceptableInput()) {
            const auto *label = findChild<QLabel *>(it.key() + "Label");
            errors.insert(it.value(), tr("%1：请输入 %2～%3 的整数")
                .arg(label ? label->text() : it.key()).arg(spin->minimum()).arg(spin->maximum()));
        }
    }
    QStringList messages;
    // 红框、提示与校验文案三处同源；组帧控件不在 m_fields 里（它是草稿上的值类型
    // 而不是 values 项），因此单独走同一段处理，错误才不会只显示不生效。
    const auto apply = [&errors, &messages](QWidget *field, const QString &tooltip) {
        const bool invalid = errors.contains(field);
        if (field->property("invalid").toBool() != invalid) {
            field->setProperty("invalid", invalid);
            field->style()->unpolish(field);
            field->style()->polish(field);
            field->update();
        }
        field->setToolTip(invalid ? errors.value(field) : tooltip);
        if (invalid) messages.append(errors.value(field));
    };
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it)
        apply(it.value(), it.value() == m_serialPorts ? m_serialPorts->currentText() : QString());
    apply(m_framingDelimiter, QString());
    m_validationError = messages.join('\n');
    // 动作按钮：配置有效才可打开；已打开时保持可点（用于关闭/取消）。
    m_action->setEnabled(m_open || m_validationError.isEmpty());
    m_action->setToolTip(m_open || m_validationError.isEmpty() ? QString() : m_validationError);
    // No inline marker: invalid fields carry a red border, the reason sits on each field.
    // An invalid field hidden behind the collapsed row would leave the message unreachable.
    if (m_initialised && !m_advanced->isChecked()) {
        for (auto *field : errors.keys()) {
            if (m_advancedPanel->isAncestorOf(field)) {
                m_advanced->setChecked(true);
                break;
            }
        }
    }
    updateGeometry();
}
