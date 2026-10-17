#include "viewtoggles.h"
#include "ui/common/rowmetrics.h"
#include "ui/common/tintedicon.h"

#include <QAction>
#include <QHBoxLayout>
#include <QPushButton>

namespace {

// 与顶栏其余按钮同一套字形尺寸：同一行并排出现，尺寸不一致会一眼看出来。
constexpr int GlyphSize = 14;

// 按钮 ↔ 命令 的桥。QPushButton 没有 QToolButton::setDefaultAction，而这里正需要那套
// 语义：命令是唯一真源，按钮只是它在顶栏的门面。
//
// 两个方向都带"值相同就不动手"的前置判断，因此不构成回路：
//   用户点击 → 按钮已自行翻转 → 把同值推给命令 → 命令 toggled 回来时按钮已是该值 → 返回。
//   程序改命令（恢复偏好、纯数据模式联动置灰）→ 按钮跟上 → 推回命令时两侧相等 → 返回。
// 两侧都不加 QSignalBlocker：按钮的 toggled 还要驱动图标换色（bindToggleIcon），
// 拦住它反而漏刷新——那正是"琥珀底上出现灰字形"这类错字的来源。
void bindToggleButton(QPushButton *button, QAction *action, const QString &shortText)
{
    if (!button || !action) return;
    // 顶栏必须用短名。菜单项和这里是同一个 QAction，菜单写完整说明后不能把长文案带上来。
    button->setText(shortText);
    button->setToolTip(action->toolTip());
    button->setChecked(action->isChecked());
    button->setEnabled(action->isEnabled());
    // 命令 → 按钮：程序侧改勾选。
    QObject::connect(action, &QAction::toggled, button, [button](bool on) {
        if (button->isChecked() != on) button->setChecked(on);
    });
    // 命令 → 按钮：可用性。关掉「前缀」会把行号、时间那批开关置灰，顶栏按钮必须同步，
    // 否则会出现"按钮亮着但点了没反应"的死开关。
    QObject::connect(action, &QAction::changed, button,
        [button, action] { button->setEnabled(action->isEnabled()); });
    // 按钮 → 命令：用户点击。
    QObject::connect(button, &QPushButton::toggled, action, [action](bool on) {
        if (action->isChecked() != on) action->setChecked(on);
    });
}

QPushButton *makeToggle(QWidget *parent, const QString &objectName, const QString &iconPath)
{
    auto *button = new QPushButton(parent);
    button->setObjectName(objectName);
    button->setCheckable(true);
    bindToggleIcon(button, iconPath, GlyphSize);
    return button;
}

} // namespace

ViewToggles::ViewToggles(QWidget *parent) : QWidget(parent)
{
    setObjectName("viewToggles");
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);

    // 文案与提示都在 bind 里由命令侧覆盖；这里只保证构造期按钮是可用的形状。
    // 点亮表示正在显示前缀：每行先是行首标记，再是正文。
    m_prefix = makeToggle(this, QStringLiteral("viewPrefix"),
        QStringLiteral(":/icons/prefix.svg"));
    // 点亮表示按颜色显示，并藏起 ESC[36;22m 这类前后缀。
    m_showAnsi = makeToggle(this, QStringLiteral("viewAnsiColor"),
        QStringLiteral(":/icons/format-letter-case.svg"));
    m_alwaysShowHex = makeToggle(this, QStringLiteral("viewAlwaysShowHex"),
        QStringLiteral(":/icons/numeric.svg"));
    m_autoWrap = makeToggle(this, QStringLiteral("viewAutoWrap"), QStringLiteral(":/icons/wrap.svg"));

    for (auto *button : {m_prefix, m_showAnsi, m_alwaysShowHex, m_autoWrap}) row->addWidget(button);
    // 四个开关按自己的 sizeHint 占位，这一行的下限由布局自己算（见 rowmetrics.h）。
    pinRowToContents(row);
}

void ViewToggles::bind(QAction *prefix, QAction *showAnsi, QAction *alwaysShowHex, QAction *autoWrap)
{
    bindToggleButton(m_prefix, prefix, QStringLiteral("前缀"));
    bindToggleButton(m_showAnsi, showAnsi, QStringLiteral("颜色"));
    bindToggleButton(m_alwaysShowHex, alwaysShowHex, QStringLiteral("HEX"));
    bindToggleButton(m_autoWrap, autoWrap, QStringLiteral("换行"));
}
