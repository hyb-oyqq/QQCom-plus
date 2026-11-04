#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QIcon>
#include <QSet>
#include "domain/trafficmessage.h"
#include "ui/panels/portselector/portselectorwidget.h"
#include "transport/transporttypes.h"

class QEvent;
class QFrame;
class QLabel;
class QHideEvent;
class QResizeEvent;
class QShowEvent;
class QTimer;
class QSplitter;
class QAction;
class QToolButton;
class QThread;
class EndpointRuntime;
class EndpointAlertState;
class PortDiscoveryService;
class PreferencesStore;
class RouteEditorWidget;
class SendPanel;
class TrafficPanel;
class CommandPanel;
struct CommandItem;

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
    // --auto-open（压测/QA 专用，test-only）：把端点 A 的草稿替换为命令行给定配置，
    // 并触发与"打开 A"按钮完全相同的 open 路径——打开失败弹窗、Error 行、状态行等
    // 需求 3 的行为照常生效。仅由 main() 在构造并 show 之后调用一次。
    void autoOpenEndpoint(const PortDraft &draft);
    // 压测探针对账用的会话累计值：接收记录条数（含示例）与 RX 原始字节数。
    quint64 sessionRecordCount() const;
    qint64 sessionRxBytes() const;
    // 状态栏里的临时提示：单独占一格，到时清空。不覆盖 A/B 连接状态，也不动统计。
    // 空文本会把这一格藏起来。alert 为真时这条用错误色，避免和旁边的灰色状态混在一起。
    //
    // 不要改用 QStatusBar::showMessage()：它会把所有 addWidget() 挂上的普通部件一并隐藏
    // （permanent widget 才不受影响）。整条状态栏都是 addWidget，一条临时消息就会让
    // RX/TX/条数/桥接整行消失，反复出错时计时器一直被重置，看起来像统计数据丢了。
    // detail 非空时作为悬停全文。底部只显示 text，避免长句被裁。
    void showStatusNote(const QString &text, int ms, bool alert = false, const QString &detail = {});

signals:
    void updateCheckRequested();

protected:
    // 轮询只在窗口真实显示时运行；离屏实例（快照、自动化测试）不扫描硬件。
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    // 窗口缩放会让水平分栏重新分配两块宽度：展开态下把结果记进快照，
    // 收起时才有一个"用户认可的宽度"可依（见 m_expandedColumnWidth 的说明）。
    void resizeEvent(QResizeEvent *event) override;
    // 不设内容下限：用户要把窗口拖到比面板更小，布局报出的 minimumSizeHint 不能再当成地板。
    QSize minimumSizeHint() const override;
    // 布局请求之后若下限被写回，立刻清掉。拖边框看的是这一刻的 minimumSize。
    bool event(QEvent *event) override;
    // 仅在有未确认提醒时监听本窗口内的点击/按键，输入照常交给原控件。
    bool eventFilter(QObject *watched, QEvent *event) override;
    // 退出时把窗口几何（大小 + 位置 + 最大化状态）落盘，下次启动恢复。
    void closeEvent(QCloseEvent *event) override;

private:
    void refreshEndpoints();
    void restoreEndpointDrafts(PreferencesStore *preferences);
    // 恢复上次的窗口几何。**首次 showEvent 时调用一次**，不能在构造末尾：
    // 窗口框架要到首次显示才建立，构造期恢复会因为框架还没算而差出一整条标题栏。
    void restoreWindowState();
    void fitSendPane(int height);
    void saveEndpointDrafts();
    void toggleEndpoint(int index);
    void recoverEndpoints();
    void handleEndpointState(int index, int state, const QString &detail);
    void handleEndpointPeers(int index, const QStringList &peers);
    void handleMessage(const Traffic::Message &message);
    void updateEndpointAlerts();
    void acknowledgeEndpointAlerts();
    void requestAlertAttention();
    void stopAlertAttention();
    void updateEndpointStatus();
    // 清掉窗口最小尺寸，并禁止布局把内容下限写回去。
    void releaseWindowSizeFloor();
    // 状态栏「置顶」：切换 WindowStaysOnTopHint，并记入 preferences。
    void setStayOnTop(bool on);
    // 状态栏「外观」：亮色、暗色或跟随系统。persist 为真时记入 preferences。
    void setColorScheme(const QString &key, bool persist);
    // 把"展开态下两块内容的宽度"记进快照。收起指令面板时唯一的依据——信号到达时
    // 面板已被压成 0 宽、分栏也已重分配，那时再读几何拿到的是错误值。
    // 由 showEvent / resizeEvent / splitterMoved / 切换前一刻共同维护。
    void recordExpandedGeometry();
    // 扩展区折叠后把它移出横向分栏。分栏里只剩左列时，左右拖动条不存在，也不占宽度。
    void syncExtensionPanel(bool expanded);
    void updateRateLabels();
    void handleCommandSend(const CommandItem &command);

    Ui::MainWindow *ui;
    PreferencesStore *m_preferences = nullptr; // 端点草稿持久化（面板各有自己的注入通道）
    QList<PortDraft> m_ports;
    QList<PortSelectorWidget *> m_selectors;
    QTimer *m_autoRefresh;
    QTimer *m_recoverTimer = nullptr;
    QSplitter *m_dataSplitter;
    QSplitter *m_horizontalSplitter = nullptr;  // 水平分割器：左侧数据区，右侧指令面板
    bool m_dataRatioApplied = false;
    // 窗口几何只在首次显示时恢复一次（见 showEvent 说明）。
    bool m_windowStateRestored = false;
    // 指令面板**展开时**两块内容的宽度快照。
    //
    // 必须在展开态下记，事后再问分栏是问不到的：`CommandPanel::setExpanded()` 会先
    // 把面板 `setMaximumWidth(0)`、再 emit expandedChanged，等信号到达时面板宽度
    // 已经是 0、分栏也已把空出来的空间分给了数据区——此时读 sizes[0] 拿到的是
    // "已经变宽之后"的值，用它去算目标窗口宽度只会把错误固化下来。
    // 记录点：用户拖分栏（splitterMoved）与窗口缩放（resizeEvent），两者都会让分栏重分配。
    // **这两个宽度是"左侧列"的宽度**（端口条 + 数据区），因为分栏的两个 child 就是
    // 左列与指令面板——不是单指数据区。
    int m_expandedColumnWidth = 0;
    int m_expandedPanelWidth = 0;
    // 分栏手柄**实际占掉**的宽度：由 `splitterWidth - 两块之和` 反推而来，不用
    // `QSplitter::handleWidth()`——那个报的是样式默认值（本机 10），而 QSS 把横向手柄
    // 画成 4px，实际只吃掉 4px。信 handleWidth() 会让每次展开把窗口多撑 6px（实测 1849→1855）。
    int m_expandedHandle = 0;
    // 左侧数据区容器。宽度下限只作测量，不挂到控件上。
    QWidget *m_dataArea = nullptr;
    // 左侧列 = 端口条 + 数据区（水平分栏的左半）。分栏的两个 child 是"左列"与"指令面板"，
    // 所以分栏的 sizes[0] 是**左列**宽度，不是数据区宽度（见 m_expandedColumnWidth）。
    QWidget *m_leftColumn = nullptr;
    // 数据区内容铺开时的宽度（布局 minimumSizeHint）。只给「展开指令面板」当首选宽度，
    // 不再写成窗口或分栏的硬下限——用户要能把窗口拖得比内容更窄。
    int m_trafficMinimumWidth = 0;
    // 左侧列内容铺开时的宽度（端口条与数据区各自下限的较大者）。展开面板时，
    // 若还没有用户拖过的宽度快照，用它当左列的起步宽度。不是窗口最小宽度。
    int m_leftColumnMinimumWidth = 0;
    RouteEditorWidget *m_bridge;
    TrafficPanel *m_traffic;
    SendPanel *m_sender;
    CommandPanel *m_commandPanel = nullptr;
    QLabel *m_routeStatus;

    // 通信运行时：GUI 组装与观察，命令与数据都经 Qt 队列跨线程。
    QThread *m_ioThread = nullptr;
    EndpointRuntime *m_runtime = nullptr;
    Transport::State m_endpointStates[2] = {Transport::State::Closed, Transport::State::Closed};
    EndpointAlertState *m_endpointAlerts[2] = {};
    bool m_alertsSuppressed[2] = {};
    QIcon m_normalWindowIcon;
    QIcon m_alertWindowIcons[2][2]; // warning/error × dim/bright
    int m_windowAlertLevel = 0;
    bool m_windowAlertBright = true;
    bool m_alertInputFilterInstalled = false;
    // 异常断开后置位，用户取消等待或重新打开后清掉。只在这一位为真时才按秒重试。
    bool m_recoverArmed[2] = {};
    // 用户取消了本次等待。在途的重试失败不能再把等待重新打开，直到用户再次点打开。
    bool m_recoverCancelled[2] = {};
    // 用户点停止后，随后到达的 Closed 不能再当成异常断开。
    bool m_userStop[2] = {};
    // 串口断线时的扫描代次。必须看到更新的一次扫描，才认为“设备重新出现”是新事实。
    quint64 m_serialRecoverAfterScan[2] = {};
    quint64 m_serialScanGeneration = 0;
    // 最近一次串口扫描快照。重试前用它确认同名设备已经重新出现。
    QSet<QString> m_presentSerialPorts;
    PortDiscoveryService *m_discovery = nullptr;
    QStringList m_endpointPeers[2];
    qint64 m_rxBytes = 0;
    qint64 m_txBytes = 0;
    qint64 m_droppedBytes = 0;
    qint64 m_rxBytesAtTick = 0;
    qint64 m_txBytesAtTick = 0;
    QTimer *m_rateTimer = nullptr;
    // 临时提示的复原计时器：单发，每次显示提示都重启，避免上一条的计时器提前清屏。
    QTimer *m_statusNoteTimer = nullptr;
    QLabel *m_rxStatus = nullptr;
    QLabel *m_txStatus = nullptr;
    QLabel *m_dropStatus = nullptr;
    QWidget *m_dropGroup = nullptr;
    QLabel *m_rxRate = nullptr;
    QLabel *m_txRate = nullptr;
    QLabel *m_trafficStatus = nullptr; // 监视区会话累计条数（面板经 summaryChanged 上报）
    QLabel *m_statusNote = nullptr;
    // A/B 连接状态各一块：圆点表示通断，文字是状态。临时提示不再借用这一格。
    QLabel *m_endpointState[2] = {nullptr, nullptr};
    QFrame *m_endpointDot[2] = {nullptr, nullptr};
    QToolButton *m_stayOnTop = nullptr;
    QToolButton *m_colorScheme = nullptr;
    QAction *m_schemeActions[3] = {};
};
#endif // MAINWINDOW_H
