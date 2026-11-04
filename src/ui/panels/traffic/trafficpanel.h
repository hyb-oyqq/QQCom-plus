#ifndef TRAFFICPANEL_H
#define TRAFFICPANEL_H

#include "ui/common/payloadcodec.h"
#include "domain/trafficmessage.h"
#include "domain/portdraft.h"
#include "traffictextview.h"
#include "trafficmodel.h"
#include "trafficfilter.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <optional>

class FilterBar;
class FilterFlags;
class ViewToggles;
struct FilterFlagsState;
class QAction;
class QComboBox;
class QFile;
class QLabel;
class QTextStream;
class QLineEdit;
class QMenu;
class PreferencesStore;
class QPushButton;
class QStackedWidget;
class QTimer;

class TrafficPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TrafficPanel(QWidget *parent = nullptr);
    ~TrafficPanel() override;
    void setPorts(const QList<PortDraft> &ports);
    void loadExample(const QList<PortDraft> &ports);
    // 真实通信消息的统一入口。高频到达时先攒批再入模（见 m_flushTimer），
    // 插入通知次数与消息速率解耦。
    void appendMessages(const QList<Traffic::Message> &messages);
    // 单条入口：与 appendMessages 同语义，免去逐条消息构造临时批次的开销。
    void appendMessage(const Traffic::Message &message);
    // 压测探针对账用的会话累计条数（含示例；透传组帧下是"记录条数"，≠ 网络包数）。
    quint64 totalAccepted() const { return m_totalAccepted; }
    bool isReprojecting() const { return m_projection.has_value(); }
    // 组合根注入的偏好存储；为空时全部显示选项不落盘（示例预览等场景）。
    void setPreferences(PreferencesStore *store);
    // 日志实际写入的目录。未手动指定时是程序目录下的 log 文件夹。
    QString logDirectory() const;
    // 改日志目录。文件名模板（基名、扩展名）保持不变，正在记录时立刻换一个新文件。
    void setLogDirectory(const QString &directory);
    // 忘掉手动指定的目录，回到程序目录下的 log 文件夹。
    void resetLogDirectory();
    void setCommandPanelToggle(bool expanded);
    // 头两行（顶栏 + 过滤条）"最少要多宽"没有独立的访问器：行里的控件各自声明下限
    // （见 `ui/common/rowmetrics.h`），布局据此算出 minimumSizeHint，主窗口直接用它。
    // 早先这里有个 filterRowWidthHint() 做逐项求和，已删除——那种手工账目漏掉一项
    // （面板自己的内边距）就会少几个像素，症状是顶栏最右的按钮探出右缘。
signals:
    // 会话累计条数与一次性提示（导出结果、日志写失败）都从这根信号出，
    // 由主窗口放进底部状态栏——面板内部不再为摘要保留一行 UI。
    void summaryChanged(const QString &text);
    // 性能状态：随心跳上报（最近 2 秒的界面迟滞/单批处理耗时 + 缓冲与日志占用）。
    // 主窗口把它挂到状态栏的 tooltip 上——性能属于"要看才发现"的信息，
    // 不值得为它再占一段状态栏宽度，但必须随手可得。
    void performanceChanged(const QString &text);
    // 指令面板切换信号
    void commandPanelToggled(bool expanded);
private:
    void updateFilter();
    // 重投影：把**已应用**的过滤条件与**当前**显示设置重新作用到模型缓冲里的全部记录上。
    //   clearAll → 从模型重放命中项 → 行号从 1 连续重排。
    // 两个维度必须分开（设计稿 §5 决策表）：
    //   · 外观（显示设置/编码格式）= 每行长什么样 → 立刻自动重投影；
    //   · 空闲时关键词和排除词只作用新行，补到历史必须点「应用过滤」。
    //     已在重建时，修改条件立即取消旧任务并按最新条件重新开始。
    //     端点、方向、转发和事件开关一点就作用到已有行。
    // 所以 applyCurrentConditions 决定这次用哪份条件过滤：
    //   true  —— 用**当前**条件（applyMatcher），并把 m_projectedOptions 记为当前条件。
    //            「应用过滤」按钮与 loadExample 走这条：它们的语义就是"让视图跟上当前条件"。
    //   false —— 空闲的外观路径：关键词和排除词仍用上次已应用的条件，
    //            不会把未应用的内容条件套到历史行上。端点、方向、转发和事件开关
    //            这次投影直接用当前值，并写回已应用条件。
    //            候选项里含"按当前显示格式渲染的文本"（见 TrafficFilter::Matcher）——
    //            所以只在 DEC / BASE64 / 转义字节等**格式专属渲染**里命中的条件会随编码
    //            格式移动命中行（同一条件在新渲染下的重新求值；内容条件未改时
    //            「应用过滤」提示保持干净）。
    // announce 与 applyCurrentConditions 是两个独立维度，不要混用：loadExample 只要"不播报"
    // （applyCurrentConditions 仍为 true），外观路径两者都为 false。
    void reprojectView(bool announce = true, bool applyCurrentConditions = true);
    void advanceProjection();
    void cancelProjection();
    // 重投影的防抖入口。外观（行前缀、HEX、换行、编码格式）和顶栏显示开关走这里：
    // 空闲时关键词和排除词等「应用过滤」；重建进行中则立即替换当前任务。
    void scheduleReproject();
    // 「每行长什么样」（编码格式 + 显示设置）是否与上次投影时不同。
    bool appearanceChanged() const;
    // 首批增量或清空时，以当前外观和条件建立视图基线。
    void markReprojected();
    // 条件条上「应用过滤」的待办提示：条件改了但还没补到已有记录上，必须说出来。
    void refreshApplyHint();
    // 过滤条的折叠：收起时只隐藏第二行，条件与记忆状态都不动。
    void setFilterExpanded(bool expanded);
    // 折叠按钮的文字：状态 + 生效条件数，收起时也能一眼看出"当前在过滤"。
    void refreshFilterToggle();
    int activeFlagCount() const;
    // 过滤开关的完整汇总：方向两项来自顶栏 FilterFlags，事件四项来自显示设置菜单的
    // QAction。两项分住两处是"顶栏腾位置"的结果，汇总只此一处——其余代码一律读它，
    // 不再各自记"哪项在哪"。
    FilterFlagsState filterFlagsState() const;
    void updatePortVisibility();
    void syncEventActions();
    void updateSummary();
    // 空数据区的两句说明。打开前缀时是收发记录；纯数据时只说接收，并写明 A、B 的颜色。
    QString waitingPlaceholder() const;
    // 性能心跳：量"这一拍比预期晚了多少"（界面迟滞）与"上一批处理花了多久"，
    // 取最近 2 秒的窗口最大值上报。迟滞是用户感知卡顿的直接代理量：
    // 界面线程被长任务占住时，连 250 ms 的心跳都会迟到。
    void updatePerformance();
    void exportVisible();
    // 把攒批里的消息整批入模并收尾（滚动到底、计数摘要）。
    void flushPending();
    // m_format 下标 → 格式枚举的唯一换算点，格式切换联动与日志落盘共用。
    PayloadCodec::Format currentDisplayFormat() const;
    // 日志记录：把入模的每条消息按当前显示格式写成一行，持续写入本次会话的文件。
    // 记录跟随显示流：切换编码格式后，新行跟随新格式，且在日志里留一行格式切换标记。
    void toggleLogRecording(bool enabled);
    void writeLog(const QList<Traffic::Message> &messages);
    void closeLogFile();
    // 记录文件在三个时刻"重开"（启动恢复记录、点「清空」、手动勾选「记录到文件」）：
    // 每次都新起一个带时间戳的文件，**绝不覆盖、绝不追加到旧文件**（用户钦定）——
    // 覆盖会吞掉上一段的证据，追加会把两段无关的会话粘在一起。返回是否打开成功；
    // 失败时沿用既有语义：播报原因、复位勾选。打开成功后立刻写一行元信息头（# 开头）。
    bool openNewLogFile();
    // 记录期内的续写入口：数据批与 # 元信息行共用。写文本、flush；写失败即停记录并播报，
    // 绝不静默丢日志——这是记录功能唯一不可接受的行为。
    void writeLogText(const QString &text);
    // 写失败的统一收口：先说出原因（沿用既有文案「日志写入失败，记录已停止」），
    // 再关闭文件、复位勾选。收口只此一处，数据批与元信息行走同一条路径。
    void stopLogOnFailure(const QString &reason);
    // 把当前目录和实际写入的文件名放进「记录到文件」的 tooltip。
    // 未记录时说明目录在哪、从哪里改；记录中保留「正在写 文件名」，测试靠这句定位文件。
    void refreshRecordTooltip();
    void refreshOpenLogAction();
    // 「日志位置」的说明和「恢复默认位置」是否可点，跟着当前目录走。
    void refreshLogLocation();
    // 用系统默认程序打开正在写的日志。文件还在追加，编辑器看到的是打开那一刻的内容。
    void openCurrentLog();
    // 空的 m_logPath 表示用程序目录下的 log。这里给出真正用来开文件的模板路径。
    QString logTemplatePath() const;
    // 摘要经 summaryChanged 上报；内容不变就不重发，状态栏不做无效刷新。
    void reportSummary(const QString &text);
    // 显示选项持久化：变更即写（QSettings 延迟落盘），析构时兜底再存一次。
    void loadPreferences();
    void savePreferences();
    TrafficModel *m_model = nullptr;
    TrafficTextView *m_text = nullptr; // 唯一显示视图：JCom 式文本框逐行追加
    QLabel *m_empty = nullptr;
    QComboBox *m_format;
    // 过滤入口：顶栏的折叠按钮（原搜索框的位置）+ 紧挨着它的方向开关与显示开关。
    // 三类开关常驻顶栏（不进可折叠的第二行）：它们的调频次都高于关键词，收起条件条
    // 时也要能点。分工不同——方向管"哪些行进来"，显示开关管"每行长什么样"。
    QPushButton *m_filterToggle;
    FilterFlags *m_flags;
    ViewToggles *m_viewToggles = nullptr; // 显示前缀 / 显示颜色 / 始终显示 HEX / 自动换行
    FilterBar *m_filterBar;
    // 事件开关：显示信息 / 显示警告 / 显示错误 / 显示状态
    QAction *m_showInfoAction = nullptr;
    QAction *m_showWarningAction = nullptr;
    QAction *m_showErrorAction = nullptr;
    QAction *m_showStateAction = nullptr;
    // 纯数据下菜单强制取消勾选，仍保存有前缀时的选择供恢复与持久化。
    bool m_eventChoices[4] = {true, true, true, true}; // info / warning / error / state
    bool m_eventOptionsActive = true;
    // 指令面板切换按钮
    QPushButton *m_commandPanelToggle = nullptr;
    // 折叠状态用显式标志而不是 isVisible()：窗口整体隐藏时 isVisible() 恒为假，
    // 记忆状态会跟着被读错。
    bool m_filterExpanded = true;
    // A/B 端点开关紧挨过滤按钮，使能时为黄；其余显示选项在「设置」菜单。
    QPushButton *m_portAButton;
    QPushButton *m_portBButton;
    // 行内容开关。关掉「显示前缀」后进入纯数据：行号、时间、长度、端口方向和 HEX 对照
    // 一起不可用，屏幕只连续显示接收字节。端口方向没有单独开关，跟着前缀总开关。
    QAction *m_prefixAction;            // 显示前缀
    QAction *m_showAnsiAction = nullptr; // 显示颜色：勾上着色并藏起转义，关掉则原文显示前后缀
    QAction *m_prefixLineNumberAction;  // 前缀行号
    QAction *m_prefixTimestampAction;   // 前缀时间戳
    QAction *m_prefixLengthAction;      // 前缀数据长度
    QAction *m_alwaysShowHexAction;     // 始终显示 HEX
    QAction *m_autoWrapAction;          // 自动换行
    QAction *m_recordAction;  // 记录到文件：按当前显示格式持续写日志
    QAction *m_openLogAction = nullptr;           // 打开正在写的那一份
    QAction *m_logLocationAction = nullptr;       // 日志位置：改目录
    QAction *m_resetLogDirectoryAction = nullptr; // 恢复默认位置
    QSet<QUuid> m_visibleEndpoints;
    QPushButton *m_follow;
    QStackedWidget *m_stack;
    quint64 m_totalAccepted = 0; // 会话累计接收条数（含示例），文本历史随淘汰可超过模型上限
    // 被过滤条件挡下的条数：只有它能把"看到的不多"和"根本没收到"区分开。
    quint64 m_filteredOut = 0;
    // 安全阀释放计数（会话内累计，清空时归零）：模型侧释放了多少**条**，
    // 文本视图侧释放了多少**行**。两个数字都必须说出来——历史被裁掉是事实，
    // 用户需要在状态栏一眼看到，而不是自己发现"怎么拖不到最早那条了"。
    quint64 m_releasedRecords = 0;
    quint64 m_releasedLines = 0;
    QString m_lastSummary;       // 上次上报的摘要文本，去重用
    // ---- 性能心跳（见 updatePerformance）----
    QTimer *m_perfTimer = nullptr;
    QElapsedTimer m_perfClock; // 上一拍的时刻，用来算"迟到了多少"
    // 最近 2 秒的采样环：报窗口内最大值，因为用户感知的是长尾而不是平均。
    static constexpr int PerfWindowTicks = 8; // 8 × 250 ms = 2 s
    qint64 m_lagSamples[PerfWindowTicks] = {};
    qint64 m_workSamples[PerfWindowTicks] = {};
    int m_perfCursor = 0;
    qint64 m_lastBatchWorkMs = 0; // 上一批 flushPending 的耗时，心跳取样后清零
    qint64 m_reportedLagMs = 0;   // 最近一次上报的窗口最大迟滞（摘要里的告警用它）
    // 攒批缓冲：appendMessages 先入缓冲，16ms 一拍整批入模。
    QList<Traffic::Message> m_pending;
    QTimer *m_flushTimer = nullptr;
    // 过滤条件的当前值：新到达的行用它判定（增量路径，热路径只做 match）。
    TrafficFilter::Options m_filterOptions;
    // 条件的可执行形态：正则预编译、候选视图按需构造，都发生在 updateFilter 里一次。
    TrafficFilter::Matcher m_matcher;
    TrafficTextView::DisplayOptions m_displayOptions;
    // 视图一致性签名：最后一次重投影时用的条件、编码格式与显示设置。
    // 视图是投影，"条件变了"不等于"视图已经跟上"——两者的差就是"待重投影"。
    // 显示设置与格式一变就必须重投影（用户明确要求：改开关要立刻作用于已有内容），
    // 过滤条件只决定新行，追溯过去同样靠这一次重投影。
    TrafficFilter::Options m_projectedOptions;
    PayloadCodec::Format m_projectedFormat = PayloadCodec::Format::Hex;
    TrafficTextView::DisplayOptions m_projectedDisplay;
    bool m_projectedDark = false;
    bool m_everProjected = false; // 一次投影都没做过时视图是空的，没有"跟不上"这回事
    // 防抖窗口内按当前条件先画上的行。签名若被拨回上次投影的值，仍要重放一次。
    bool m_incrementalDiverged = false;
    // 新数据曾按不同条件处理；即使条件改回基线，也不能把混合历史当成已应用。
    // 与外观偏离分开：空闲编辑关键词不应自动触发全量重建。
    bool m_filterHistoryDiverged = false;
    QTimer *m_reprojectTimer = nullptr;
    // 仅持有当前任务；条件变化立即替换，历史和新到达数据统一使用本轮快照。
    struct Projection {
        TrafficFilter::Options options;
        TrafficFilter::Matcher matcher;
        PayloadCodec::Format format = PayloadCodec::Format::Hex;
        TrafficTextView::DisplayOptions display;
        int row = 0;
        quint64 accepted = 0;
        quint64 filtered = 0;
        double scrollFraction = 0;
        bool announce = false;
        bool dark = false;
    };
    std::optional<Projection> m_projection;
    QTimer *m_projectionTimer = nullptr;
    PreferencesStore *m_preferences = nullptr;
    bool m_loading = false; // 加载偏好期间联动触发的保存要抑制，否则半成品状态会覆盖已存值
    QFile *m_logFile = nullptr;   // 打开期间持续写入；关闭开关、重开或析构时收口
    QTextStream *m_logStream = nullptr; // 与 m_logFile 同寿命，避免每批重建编码器
    // 用户选定的**模板**路径（不含时间戳）。空表示还没手动指定，用程序目录下的
    // log/QQCom-log.txt。它只用来定"目录 + 基名 + 扩展名"，实际文件名每次带时间戳另起。
    // 存模板而不是实际路径，是为了避免重启后时间戳层层叠加。
    QString m_logPath;
    // 本次实际写入的完整路径（带时间戳），仅供 tooltip 显示"正在写哪个文件"。
    QString m_logFilePath;
    // 本次记录会话已写入的字节数：flushPending 写完一批后从 m_logFile->size() 读一次
    // （O(1)，不碰目录查询）。摘要里的"日志 X"与性能 tooltip 都取它；不记录时恒为 0。
    qint64 m_logBytes = 0;
};

#endif
