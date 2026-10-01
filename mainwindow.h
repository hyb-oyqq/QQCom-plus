#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "src/ui/portselectorwidget.h"

class QListWidget;
class QLabel;
class SendPanel;
class TrafficPanel;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    void loadExample();
    void showRoutes();

private:
    void addPort(int kind);
    void updatePortList();
    void synchronizePorts();
    void selectPort(int row);
    Ui::MainWindow *ui;
    QList<PortDraft> m_ports;
    QListWidget *m_portList;
    PortSelectorWidget *m_selector;
    TrafficPanel *m_traffic;
    SendPanel *m_sender;
    QLabel *m_portCount;
    QLabel *m_routeStatus;
    QLabel *m_selectedTitle;
    int m_portSequence = 0;
};
#endif // MAINWINDOW_H
