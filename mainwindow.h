#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "domain/trafficmessage.h"
#include "ui/panels/portselector/portselectorwidget.h"
#include "transport/transporttypes.h"

class QLabel;
class QHideEvent;
class QShowEvent;
class QTimer;
class QSplitter;
class QThread;
class EndpointRuntime;
class PreferencesStore;
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
    void restoreEndpointDrafts(PreferencesStore *preferences);
    void saveEndpointDrafts();
    void toggleEndpoint(int index);
    void handleEndpointState(int index, int state, const QString &detail);
    void handleEndpointPeers(int index, const QStringList &peers);
    void handleMessage(const Traffic::Message &message);
    void updateEndpointStatus();
    void updateRateLabels();
    Ui::MainWindow *ui;
    PreferencesStore *m_preferences = nullptr; // 端点草稿持久化（面板各有自己的注入通道）
    QList<PortDraft> m_ports;
    QList<PortSelectorWidget *> m_selectors;
    QTimer *m_autoRefresh;
    QSplitter *m_dataSplitter;
    bool m_dataRatioApplied = false;
    RouteEditorWidget *m_bridge;
    TrafficPanel *m_traffic;
    SendPanel *m_sender;
    QLabel *m_routeStatus;

    // 通信运行时：GUI 组装与观察，命令与数据都经 Qt 队列跨线程。
    QThread *m_ioThread = nullptr;
    EndpointRuntime *m_runtime = nullptr;
    Transport::State m_endpointStates[2] = {Transport::State::Closed, Transport::State::Closed};
    QStringList m_endpointPeers[2];
    qint64 m_rxBytes = 0;
    qint64 m_txBytes = 0;
    qint64 m_rxBytesAtTick = 0;
    qint64 m_txBytesAtTick = 0;
    QTimer *m_rateTimer = nullptr;
    QLabel *m_rxStatus = nullptr;
    QLabel *m_txStatus = nullptr;
    QLabel *m_trafficStatus = nullptr; // 监视区会话累计条数（面板经 summaryChanged 上报）
    QLabel *m_statusNote = nullptr;
};
#endif // MAINWINDOW_H
