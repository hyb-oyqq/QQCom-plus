#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "src/ui/panels/portselector/portselectorwidget.h"

class QLabel;
class QHideEvent;
class QShowEvent;
class QTimer;
class QSplitter;
class RouteEditorWidget;
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

protected:
    // 轮询只在窗口真实显示时运行；离屏实例（快照、自动化测试）不扫描硬件。
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void refreshEndpoints();
    Ui::MainWindow *ui;
    QList<PortDraft> m_ports;
    QList<PortSelectorWidget *> m_selectors;
    QTimer *m_autoRefresh;
    QSplitter *m_dataSplitter;
    bool m_dataRatioApplied = false;
    RouteEditorWidget *m_bridge;
    TrafficPanel *m_traffic;
    SendPanel *m_sender;
    QLabel *m_routeStatus;
};
#endif // MAINWINDOW_H
