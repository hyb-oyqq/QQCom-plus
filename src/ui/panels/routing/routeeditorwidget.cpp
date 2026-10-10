#include "routeeditorwidget.h"
#include "ui/common/combometrics.h"
#include "ui/common/endpointformat.h"
#include "ui/common/tintedicon.h"

#include <QComboBox>
#include <QFrame>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// The bridge button keeps its text; the glyph rides beside it like on the action buttons.
constexpr int BridgeIconSize = 14;

// The direction options carry a pictogram each: the text arrows depend on font fallback and
// come out faint, while the icons state the direction unambiguously.
constexpr int DirectionIconSize = 14;

} // namespace

RouteEditorWidget::RouteEditorWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName("BridgeRow");
    // No styled background and no fixed width: the block blends into the endpoint bar and
    // sizes itself from its content, so the controls stay as narrow as the widest one.
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The endpoint column is "row / divider / row". Mirroring that structure here — two halves
    // of equal stretch with the same divider between them, each control filling its half — is
    // what lines this block up with the connect and listen buttons by construction, without
    // measuring anything.
    auto *directionRow = new QWidget;
    auto *directionLayout = new QVBoxLayout(directionRow);
    directionLayout->setContentsMargins(8, 2, 8, 2);

    // Direction is the summary: the options name the pair themselves, so no title or label is
    // needed. Each option carries a pictogram because the text arrows render faintly
    // depending on the font fallback.
    m_direction = new QComboBox;
    m_direction->setObjectName("routeDirection");
    m_direction->setIconSize(QSize(DirectionIconSize, DirectionIconSize));
    m_direction->addItem(mutedIcon(QStringLiteral(":/icons/arrow-right.svg"), DirectionIconSize),
                         tr("A → B"));
    m_direction->addItem(mutedIcon(QStringLiteral(":/icons/arrow-left.svg"), DirectionIconSize),
                         tr("B → A"));
    m_direction->addItem(mutedIcon(QStringLiteral(":/icons/swap-horizontal.svg"), DirectionIconSize),
                         tr("A ↔ B"));
    m_direction->setCurrentIndex(2);
    fitComboToItems(m_direction);
    m_direction->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    directionLayout->addWidget(m_direction);
    layout->addWidget(directionRow, 1);

    auto *divider = new QFrame;
    divider->setObjectName("EndpointDivider");
    divider->setFrameShape(QFrame::HLine);
    layout->addWidget(divider);

    auto *bridgeRow = new QWidget;
    auto *bridgeLayout = new QVBoxLayout(bridgeRow);
    bridgeLayout->setContentsMargins(8, 2, 8, 2);
    m_bridge = new QPushButton(tr("建立桥接"));
    m_bridge->setObjectName("bridgeButton");
    m_bridge->setIconSize(QSize(BridgeIconSize, BridgeIconSize));
    m_bridge->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    bridgeLayout->addWidget(m_bridge);
    layout->addWidget(bridgeRow, 1);

    connect(m_direction, &QComboBox::currentIndexChanged, this, &RouteEditorWidget::updateBridgeState);
    connect(m_bridge, &QPushButton::clicked, this, [this] {
        if (!m_active && !m_ready) return;
        m_active = !m_active;
        emit bridgeChanged(link());
        updateBridgeState();
    });
    updateBridgeState();
}

void RouteEditorWidget::setEndpoints(const QList<PortDraft> &ports, bool ready)
{
    // Any endpoint edit invalidates a running bridge; the link must be re-established.
    bool changed = ports.size() != m_ports.size();
    for (int i = 0; !changed && i < ports.size(); ++i)
        changed = ports.at(i).kind != m_ports.at(i).kind || ports.at(i).values != m_ports.at(i).values;
    m_ports = ports;
    m_ready = ready && ports.size() == 2;
    if (m_active && (changed || !m_ready)) {
        m_active = false;
        emit bridgeChanged(link());
    }
    updateBridgeState();
}

BridgeLink RouteEditorWidget::link() const
{
    BridgeLink link;
    link.enabled = m_active;
    link.direction = m_direction ? m_direction->currentIndex() : int(BridgeDirection::Both);
    return link;
}

QString RouteEditorWidget::bridgeSummary() const
{
    if (m_ports.size() != 2) return tr("端点未就绪");
    static const QStringList arrows{QStringLiteral("→"), QStringLiteral("←"), QStringLiteral("↔")};
    return tr("%1  %2  %3").arg(endpointLabel(0, m_ports.at(0).kind),
                                arrows.value(m_direction->currentIndex(), QStringLiteral("↔")),
                                endpointLabel(1, m_ports.at(1).kind));
}

void RouteEditorWidget::updateBridgeState()
{
    m_direction->setEnabled(!m_active);
    m_bridge->setEnabled(m_active || m_ready);
    m_bridge->setText(m_active ? tr("停止桥接") : tr("建立桥接"));
    m_bridge->setIcon(actionStateIcon(QStringLiteral(":/icons/swap-horizontal.svg"), BridgeIconSize));
    // 已建立和已连接端口用同一套红：蓝是还没开始的动作，红是正在运行、再点就停。
    m_bridge->setProperty("open", m_active);
    m_bridge->style()->unpolish(m_bridge);
    m_bridge->style()->polish(m_bridge);
    // The pair label is gone; the tooltip carries the full endpoint names and the direction.
    m_bridge->setToolTip(m_active ? tr("停止 A、B 之间的转发")
                       : m_ready ? bridgeSummary() : tr("先补全 A、B 的端点参数"));
}
