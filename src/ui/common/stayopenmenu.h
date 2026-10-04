#ifndef STAYOPENMENU_H
#define STAYOPENMENU_H

#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>

// 勾选型菜单：点条目只切换勾选、不关闭菜单，便于连续调整多个开关；
// 只有点击菜单外、再点按钮或按 Esc 才关闭。非勾选条目保持系统默认行为。
//
// 顶栏"显示设置"与过滤条"更多条件"共用这一份实现——两处的交互语言必须一致，
// 否则同一套勾选动作在不同入口表现不同。无信号槽需求，故不需要 Q_OBJECT，
// 成员函数定义在类体内天然 inline，可以安全地放进头文件被多个 TU 包含。
class StayOpenMenu final : public QMenu
{
public:
    using QMenu::QMenu;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (auto *action = activeAction(); action && action->isEnabled() && action->isCheckable()
            && !action->menu()) {
            action->setChecked(!action->isChecked());
            update();
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        // 键盘激活（Return/Space）同样只切换勾选、不关闭。
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Space)
            && activeAction() && activeAction()->isEnabled() && activeAction()->isCheckable()
            && !activeAction()->menu()) {
            activeAction()->setChecked(!activeAction()->isChecked());
            update();
            return;
        }
        QMenu::keyPressEvent(event);
    }
};

#endif
