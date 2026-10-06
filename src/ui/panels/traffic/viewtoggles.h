#ifndef VIEWTOGGLES_H
#define VIEWTOGGLES_H

#include <QWidget>

class QAction;
class QPushButton;

// 顶栏的**显示开关**组（用户钦定）：纯数据模式 / 同时显示 HEX / 自动换行。
//
// 与左邻的 FilterFlags 严格分工——那排管"哪些行进来"（过滤），本组管"每行长什么样"
// （外观）。两者同住顶栏第一行，改动却走两条不同的路径：过滤只作用新行，要补到历史
// 得点「应用过滤」；外观一变就必须排队重投影，把已有内容一起换过来（设计稿 §5）。
// 摆在一起是因为**调用频次都高**，不是因为它们是一类条件。
//
// 状态所有权：这三个开关的唯一真源是传入的 QAction（显示设置菜单里的同名项），
// 本控件只提供"常驻、一眼看得见、一点就换"的门面。按钮自己不持有任何被读取的状态
// ——面板、落盘、重投影一律读 QAction。因此两处永远一致，也不存在"哪边为准"的问题。
class ViewToggles : public QWidget
{
    Q_OBJECT
public:
    explicit ViewToggles(QWidget *parent = nullptr);

    // 把三个按钮分别挂到命令上：文案与提示取自命令（不在此处另抄一份），勾选与可用性
    // 双向同步。必须在 QAction 建好之后调用一次——按钮的显示状态由命令初始化。
    void bind(QAction *pureData, QAction *hexEcho, QAction *lineWrap);

private:
    QPushButton *m_pure = nullptr;
    QPushButton *m_hex = nullptr;
    QPushButton *m_wrap = nullptr;
};

#endif
