#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "domain/trafficmessage.h"
#include "ui/panels/portselector/portselectorwidget.h"
#include "transport/transporttypes.h"

class QLabel;
class QHideEvent;
class QResizeEvent;
class QShowEvent;
class QTimer;
class QSplitter;
class QThread;
class EndpointRuntime;
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

protected:
    // 轮询只在窗口真实显示时运行；离屏实例（快照、自动化测试）不扫描硬件。
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    // 窗口缩放会让水平分栏重新分配两块宽度：展开态下把结果记进快照，
    // 收起时才有一个"用户认可的宽度"可依（见 m_expandedColumnWidth 的说明）。
    void resizeEvent(QResizeEvent *event) override;
    // 退出时把窗口几何（大小 + 位置 + 最大化状态）落盘，下次启动恢复。
    void closeEvent(QCloseEvent *event) override;

private:
    void refreshEndpoints();
    void restoreEndpointDrafts(PreferencesStore *preferences);
    // 恢复上次的窗口几何。**首次 showEvent 时调用一次**，不能在构造末尾：
    // 窗口框架要到首次显示才建立，构造期恢复会因为框架还没算而差出一整条标题栏。
    void restoreWindowState();
    void saveEndpointDrafts();
    void toggleEndpoint(int index);
    void handleEndpointState(int index, int state, const QString &detail);
    void handleEndpointPeers(int index, const QStringList &peers);
    void handleMessage(const Traffic::Message &message);
    void updateEndpointStatus();
    // 重算窗口最小宽度：左右两块面板内容完整可见所需的宽度之和（用户钦定——
    // 窗口放不下时应该变大窗口，而不是压缩"分组"下拉、裁掉表格手柄列）。
    void updateWindowMinimumWidth();
    // 把"展开态下两块内容的宽度"记进快照。收起指令面板时唯一的依据——信号到达时
    // 面板已被压成 0 宽、分栏也已重分配，那时再读几何拿到的是错误值。
    // 由 showEvent / resizeEvent / splitterMoved / 切换前一刻共同维护。
    void recordExpandedGeometry();
    void updateRateLabels();
    // 状态栏右端的**临时提示**：借用 m_statusNote 这一格显示一段短消息，到时自动
    // 恢复成"A 已连接 · B 未连接"的汇总。
    //
    // 为什么不用 QStatusBar::showMessage()：它会把所有 `addWidget()` 加的**普通**部件
    // 一并隐藏（Qt 的既定语义，permanent widget 才不受影响）。而统计行正是用 addWidget
    // 挂上去的，于是任何一条临时消息一出现，RX/TX/条数/桥接整行就消失，5 秒后才回来；
    // 反复连接时每次都重置那个 5 秒计时器，看起来就是"统计数据不显示了，很久才恢复"
    // （用户反馈）。提示与统计各有各的位置，不该互相顶掉。
    void showStatusNote(const QString &text, int ms);
    void handleCommandSend(const CommandItem &command);

    Ui::MainWindow *ui;
    PreferencesStore *m_preferences = nullptr; // 端点草稿持久化（面板各有自己的注入通道）
    QList<PortDraft> m_ports;
    QList<PortSelectorWidget *> m_selectors;
    QTimer *m_autoRefresh;
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
    // 左侧数据区容器：设置它的最小宽度（= m_trafficMinimumWidth）作为硬下限。
    QWidget *m_dataArea = nullptr;
    // 左侧列 = 端口条 + 数据区（水平分栏的左半）。分栏的两个 child 是"左列"与"指令面板"，
    // 所以分栏的 sizes[0] 是**左列**宽度，不是数据区宽度（见 m_expandedColumnWidth）。
    QWidget *m_leftColumn = nullptr;
    // 数据区（左侧）的宽度下限：展开指令面板时右侧只拿"它自己的最小宽度"，
    // 左侧不得被压到这条线以下——过滤条在最小窗宽下就靠它保住按钮不被挤压。
    // 取值 = 过滤条第 1 行全部按钮 + 关键词框下限 恰好铺开所需的宽度（见 .cpp 注释）。
    int m_trafficMinimumWidth = 0;
    // **左侧列**的宽度下限（端口条与数据区各自下限的较大者）。分栏的 sizes[0] 是左列宽度，
    // 所以"窗口最少要多宽"与"收起时左列不许被压缩"都以它为准——端口条也在左列里，
    // 只按数据区下限算会让它在窄窗口下被挤。同样由布局自己报出（minimumSizeHint）。
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
    QStringList m_endpointPeers[2];
    qint64 m_rxBytes = 0;
    qint64 m_txBytes = 0;
    qint64 m_rxBytesAtTick = 0;
    qint64 m_txBytesAtTick = 0;
    QTimer *m_rateTimer = nullptr;
    // 临时提示的复原计时器：单发，每次显示提示都重启，避免上一条的计时器提前清屏。
    QTimer *m_statusNoteTimer = nullptr;
    QLabel *m_rxStatus = nullptr;
    QLabel *m_txStatus = nullptr;
    QLabel *m_trafficStatus = nullptr; // 监视区会话累计条数（面板经 summaryChanged 上报）
    QLabel *m_statusNote = nullptr;
};
#endif // MAINWINDOW_H
