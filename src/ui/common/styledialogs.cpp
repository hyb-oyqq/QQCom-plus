#include "styledialogs.h"
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
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

// 应用统一的对话框样式：与 workbench.qss 保持一致，自适应浅色与暗色主题。
// 样式表直接设置在对话框实例上，不影响全局。
void applyDialogStyle(QWidget *dialog)
{
    if (!dialog) return;

    // 对话框统一样式：与主题色调一致的背景、边框、按钮样式
    const QString style = R"(
        QDialog {
            background: #ffffff;
            color: #182334;
        }
        QLabel {
            color: #182334;
            font-size: 13px;
        }
        QPushButton {
            background: #f4f6fa;
            border: 1px solid #a9b5c9;
            border-radius: 5px;
            padding: 6px 16px;
            min-height: 18px;
            min-width: 70px;
            color: #1f2a3d;
        }
        QPushButton:hover {
            color: #14326f;
            border-color: #6d87b8;
            background: #e8eefb;
        }
        QPushButton:pressed {
            background: #d7e2f7;
            border-color: #4d6ba3;
        }
        QPushButton:default, QPushButton[text="确定"], QPushButton[text="导出"], QPushButton[text="OK"] {
            background: #1d4ed8;
            border-color: #1a45c0;
            color: #ffffff;
            font-weight: 600;
        }
        QPushButton:default:hover, QPushButton[text="确定"]:hover, QPushButton[text="导出"]:hover {
            background: #163fb3;
            border-color: #163fb3;
        }
        QLineEdit, QPlainTextEdit, QTextEdit {
            background: #ffffff;
            border: 1px solid #a9b5c9;
            border-radius: 5px;
            padding: 4px 7px;
            color: #182334;
            selection-background-color: #1d4ed8;
            selection-color: #ffffff;
        }
        QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus {
            border-color: #1d4ed8;
        }
        QComboBox {
            background: #ffffff;
            border: 1px solid #a9b5c9;
            border-radius: 5px;
            padding: 4px 7px;
            min-height: 18px;
        }
        QComboBox:hover {
            border-color: #76869f;
        }
        QComboBox:focus {
            border-color: #1d4ed8;
        }
        QListWidget {
            background: #ffffff;
            border: 1px solid #a9b5c9;
            border-radius: 5px;
        }
        QListWidget::item {
            padding: 4px;
        }
        QListWidget::item:selected {
            background: #d3e0fa;
            color: #14326f;
        }
        QCheckBox {
            spacing: 6px;
            color: #1f2a3d;
        }
        QCheckBox::indicator {
            width: 16px;
            height: 16px;
            border: 1px solid #8b99b0;
            border-radius: 4px;
            background: #ffffff;
        }
        QCheckBox::indicator:hover {
            border-color: #1d4ed8;
        }
        QCheckBox::indicator:checked {
            background: #1d4ed8;
            border-color: #1a45c0;
        }
    )";

    dialog->setStyleSheet(style);
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
    applyDialogStyle(&msgBox);
    msgBox.exec();
}

void warning(QWidget *parent, const QString &title, const QString &text)
{
    QMessageBox msgBox(safeParent(parent));
    msgBox.setWindowTitle(title);
    msgBox.setText(text);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setStandardButtons(QMessageBox::Ok);
    applyDialogStyle(&msgBox);
    msgBox.exec();
}

void critical(QWidget *parent, const QString &title, const QString &text)
{
    QMessageBox msgBox(safeParent(parent));
    msgBox.setWindowTitle(title);
    msgBox.setText(text);
    msgBox.setIcon(QMessageBox::Critical);
    msgBox.setStandardButtons(QMessageBox::Ok);
    applyDialogStyle(&msgBox);
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
    applyDialogStyle(&msgBox);
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
    applyDialogStyle(&dialog);

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
    applyDialogStyle(&dialog);

    const int ret = dialog.exec();
    if (ok) *ok = (ret == QDialog::Accepted);
    return ret == QDialog::Accepted ? dialog.textValue() : QString();
}

QString getOpenFileName(QWidget *parent, const QString &caption,
                        const QString &dir, const QString &filter)
{
    QFileDialog dialog(safeParent(parent), caption, dir, filter);
    dialog.setFileMode(QFileDialog::ExistingFile);
    applyDialogStyle(&dialog);

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
    applyDialogStyle(&dialog);

    if (dialog.exec() == QDialog::Accepted) {
        const QStringList files = dialog.selectedFiles();
        return files.isEmpty() ? QString() : files.first();
    }
    return QString();
}

} // namespace StyleDialogs
