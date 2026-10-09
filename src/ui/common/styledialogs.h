#ifndef STYLEDIALOGS_H
#define STYLEDIALOGS_H

#include <QMessageBox>
#include <QString>

class QWidget;

// 统一样式的对话框封装：全局弹窗样式统一，自适应浅色与暗色主题。
// 所有对话框都通过这个类创建，确保父窗口正确设置、样式一致。
namespace StyleDialogs
{
    // 信息提示框
    void information(QWidget *parent, const QString &title, const QString &text);

    // 警告提示框
    void warning(QWidget *parent, const QString &title, const QString &text);

    // 错误提示框
    void critical(QWidget *parent, const QString &title, const QString &text);

    // 询问对话框：返回 true 表示用户点击了 Yes
    bool question(QWidget *parent, const QString &title, const QString &text);

    // 单行文本输入对话框
    QString getText(QWidget *parent, const QString &title, const QString &label,
                    const QString &defaultText = QString(), bool *ok = nullptr);

    // 多行文本输入对话框
    QString getMultiLineText(QWidget *parent, const QString &title, const QString &label,
                             const QString &defaultText = QString(), bool *ok = nullptr);

    // 打开文件对话框
    QString getOpenFileName(QWidget *parent, const QString &caption,
                            const QString &dir = QString(),
                            const QString &filter = QString());

    // 保存文件对话框
    QString getSaveFileName(QWidget *parent, const QString &caption,
                            const QString &dir = QString(),
                            const QString &filter = QString());

    // 选择已有目录
    QString getExistingDirectory(QWidget *parent, const QString &caption,
                                 const QString &dir = QString());
}

#endif // STYLEDIALOGS_H
