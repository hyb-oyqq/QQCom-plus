#ifndef SENDPANEL_H
#define SENDPANEL_H

#include "portselectorwidget.h"

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class RouteEditorWidget;

class SendPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SendPanel(QWidget *parent = nullptr);
    void setPorts(const QList<PortDraft> &ports);
    void setPayload(const QByteArray &bytes);
    void showRoutes();
signals:
    void routeDraftCountChanged(int count);
private:
    struct CommandDraft { QString name; QString text; int mode; int encoding; int suffix; };
    QWidget *createSendPage();
    QWidget *createCommandsPage();
    void updatePreview();
    void updateTarget();
    void rebuildCommands();
    void loadCommand(int row);
    void moveCommand(int offset);
    QList<PortDraft> m_ports;
    QList<CommandDraft> m_commands;
    QTabWidget *m_tabs;
    QComboBox *m_target;
    QComboBox *m_peer;
    QComboBox *m_mode;
    QComboBox *m_encoding;
    QComboBox *m_suffix;
    QPlainTextEdit *m_editor;
    QLabel *m_preview;
    QLabel *m_length;
    QPushButton *m_save;
    QTableWidget *m_commandTable;
    RouteEditorWidget *m_routes;
};

#endif
