#include "styledialogs.h"
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QWidget>

namespace {

// 获取有效的父窗口：确保对话框模态行为正确，避免崩溃。
// QInputDialog 和 QMessageBox 在某些情况下父窗口为 nullptr 会崩溃，
// 这里统一处理，找到最顶层的可见窗口作为父窗口。
QWidget* safeParent(QWidget *parent)
{
    if (parent) {
        // 如果传入的是 QWidget 但不是顶层窗口，找到它的顶层窗口
        return parent->window();
    }
    return nullptr;
}

} // namespace

namespace StyleDialogs
{

void information(QWidget *parent, const QString &title, const QString &text)
{
    QMessageBox msgBox(safeParent(parent));
    msgBox.setWindowTitle(title);
    msgBox.setText(text);
    msgBox.setIcon(QMessageBox::Information);
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();
}

void warning(QWidget *parent, const QString &title, const QString &text)
{
    QMessageBox msgBox(safeParent(parent));
    msgBox.setWindowTitle(title);
    msgBox.setText(text);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();
}

void critical(QWidget *parent, const QString &title, const QString &text)
{
    QMessageBox msgBox(safeParent(parent));
    msgBox.setWindowTitle(title);
    msgBox.setText(text);
    msgBox.setIcon(QMessageBox::Critical);
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();
}

bool question(QWidget *parent, const QString &title, const QString &text)
{
    QMessageBox msgBox(safeParent(parent));
    msgBox.setWindowTitle(title);
    msgBox.setText(text);
    msgBox.setIcon(QMessageBox::Question);
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msgBox.setDefaultButton(QMessageBox::No);
    return msgBox.exec() == QMessageBox::Yes;
}

QString getText(QWidget *parent, const QString &title, const QString &label,
                const QString &defaultText, bool *ok)
{
    QInputDialog dialog(safeParent(parent));
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setTextValue(defaultText);
    dialog.setInputMode(QInputDialog::TextInput);

    const int ret = dialog.exec();
    if (ok) *ok = (ret == QDialog::Accepted);
    return ret == QDialog::Accepted ? dialog.textValue() : QString();
}

QString getMultiLineText(QWidget *parent, const QString &title, const QString &label,
                         const QString &defaultText, bool *ok)
{
    QInputDialog dialog(safeParent(parent));
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setTextValue(defaultText);
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setOption(QInputDialog::UsePlainTextEditForTextInput, true);

    const int ret = dialog.exec();
    if (ok) *ok = (ret == QDialog::Accepted);
    return ret == QDialog::Accepted ? dialog.textValue() : QString();
}

QString getOpenFileName(QWidget *parent, const QString &caption,
                        const QString &dir, const QString &filter)
{
    QFileDialog dialog(safeParent(parent), caption, dir, filter);
    dialog.setFileMode(QFileDialog::ExistingFile);

    if (dialog.exec() == QDialog::Accepted) {
        const QStringList files = dialog.selectedFiles();
        return files.isEmpty() ? QString() : files.first();
    }
    return QString();
}

QString getSaveFileName(QWidget *parent, const QString &caption,
                        const QString &dir, const QString &filter)
{
    QFileDialog dialog(safeParent(parent), caption, dir, filter);
    dialog.setAcceptMode(QFileDialog::AcceptSave);

    if (dialog.exec() == QDialog::Accepted) {
        const QStringList files = dialog.selectedFiles();
        return files.isEmpty() ? QString() : files.first();
    }
    return QString();
}

QString getExistingDirectory(QWidget *parent, const QString &caption, const QString &dir)
{
    QFileDialog dialog(safeParent(parent), caption, dir);
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::ShowDirsOnly, true);

    if (dialog.exec() == QDialog::Accepted) {
        const QStringList files = dialog.selectedFiles();
        return files.isEmpty() ? QString() : files.first();
    }
    return QString();
}

} // namespace StyleDialogs
