#ifndef STYLEDIALOGS_H
#define STYLEDIALOGS_H

#include <QMessageBox>
#include <QString>

class QWidget;

// 统一的对话框入口：父窗口取顶层窗口，避免空父窗口把模态框关掉。
// 消息框和输入框不另写颜色，吃应用级样式表，跟当前浅色/深色一起走。
// 文件框保持系统原生框：给它套样式表会改走 Qt 自绘框，系统深色就失效了。
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
