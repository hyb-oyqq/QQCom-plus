#ifndef TRAFFICPANEL_H
#define TRAFFICPANEL_H

#include "ui/common/payloadcodec.h"
#include "domain/trafficmessage.h"
#include "domain/portdraft.h"
#include "traffictextview.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QWidget>

class FilterBar;
class FilterFlags;
class QAction;
class QComboBox;
class QFile;
class QLabel;
class QLineEdit;
class QMenu;
class PreferencesStore;
class QPushButton;
class QStackedWidget;
class QTimer;

// 消息是 domain 的值类型（Traffic::Message）：原始字节、时间戳、来源、长度都是事实。
//
// TrafficModel 是**屏幕上能重画的完整事实来源**：监视区里看到的每一行都由它投影而来。
// 显示设置与过滤条件要能立刻作用于"已经收到的内容"，前提就是原始记录还在——
// 只留渲染后的字符串是做不到的（这也正是它早先只能"影响新行"的原因）。
// 导出、记录与关键词搜索同样以它为唯一数据源。
class TrafficModel : public QObject
{
    Q_OBJECT
public:
    // 上限**不是历史策略，只是内存安全阀**（用户钦定：像 JCom 一样一直留着）。
    // 早先的 16 MiB / 5 万条是"重投影还能点一下就有"的性能取舍，代价是压测跑
    // 一万多条就开始丢最早的记录——用户拿 JCom 一比就发现丢了（16 MiB ÷ 每条字节
    // 数就是那条隐形的历史长度）。现在默认不再设小预算：正常压测——几十小时、
    // 上百万条——都碰不到安全阀。留着它是因为无界增长最终以 OOM 收场，
    // 那比丢最旧数据更糟。
    //
    // 安全阀触顶时**必须播报**：appendRecords 把释放条数交回调用方，由面板写进
    // 状态栏摘要（见 TrafficPanel::m_releasedRecords）。无声丢数据是这里唯一
    // 不可接受的行为——"屏幕上少了几万条"和"根本没收到"必须能分辨。
    static constexpr qint64 kSafetyByteBudget = 512LL * 1024 * 1024; // 512 MiB 原始载荷
    static constexpr int kSafetyRowBudget = 20000000;                // 2000 万条：元数据兜底

    explicit TrafficModel(QObject *parent = nullptr);
    int rowCount() const;
    qint64 rawBytes() const { return m_rawBytes; }
    qint64 byteBudget() const { return m_byteBudget; }
    int rowBudget() const { return m_rowBudget; }
    // 注入上限。默认就是上面两个安全阀；测试用它把预算压到极小以便验证淘汰语义，
    // 将来的"内存紧张时收紧"策略也从这里进。
    void setBudget(qint64 bytes, int rows);
    void replaceRecords(QList<Traffic::Message> records);
    // 返回**本次从头部释放掉的最旧条数**（0 表示没释放）。调用方必须把它播报出去，
    // 不能丢进 /dev/null。
    int appendRecords(QList<Traffic::Message> records);
    const Traffic::Message &record(int row) const;
private:
    QList<Traffic::Message> m_records;
    qint64 m_rawBytes = 0; // m_records 的原始字节总数，避免每次追加都全量重算
    qint64 m_byteBudget = kSafetyByteBudget;
    int m_rowBudget = kSafetyRowBudget;
};

// 过滤判定的唯一实现：文本显示在消息到达时调用它，条件由调用方持有。
// 全部条件都作用在结构化字段上：端点/方向来自消息本身，
// 内容条件则对同一份原始字节做多种视图的匹配，所见即可搜。
class TrafficFilter
{
public:
    // 一条内容条件（关键词或排除词）。正则模式下构造时预编译一次，热路径只 match()。
    // 非法正则**不进判定**（该条件被丢弃，由条件条标红提示）：退化的方向必须是"不过滤"，
    // 宁可多显示，也绝不因为写错一个括号把整屏数据静默吞掉。
    class TextCondition
    {
    public:
        TextCondition() = default;
        TextCondition(const QString &expression, bool regex, Qt::CaseSensitivity sensitivity);
        bool matches(const QString &text) const;
        bool isEmpty() const;
    private:
        QString m_literal;
        QRegularExpression m_pattern;
        Qt::CaseSensitivity m_sensitivity = Qt::CaseInsensitive;
        // 必须显式记"这条是正则"：默认构造的 QRegularExpression 是**有效且匹配一切**的，
        // 用它当"是否正则"的判据，字面条件会全部退化成匹配所有文本。
        bool m_regex = false;
    };

    struct Options
    {
        QSet<QUuid> endpoints; // 集合为空表示不显示任何端点的记录
        bool rx = true;
        bool tx = true;
        bool states = true;   // 状态迁移（端口打开/关闭/重连等）
        bool warnings = true; // 警告日志
        bool errors = true;   // 错误日志
        // 内容条件：关键词任一命中即通过（组内 OR），排除词命中任一即隐藏（黑名单）。
        // 两者共用同一套候选文本视图，见 Matcher。
        QStringList keywords;
        QStringList excludes;
        bool caseSensitive = false;
        bool regex = false;

        // 面板用它判断"条件是否变了、视图是否还和条件一致"。
        bool operator==(const Options &other) const
        {
            return endpoints == other.endpoints && rx == other.rx && tx == other.tx
                && states == other.states && warnings == other.warnings && errors == other.errors
                && keywords == other.keywords && excludes == other.excludes
                && caseSensitive == other.caseSensitive && regex == other.regex;
        }
        bool operator!=(const Options &other) const { return !(*this == other); }
    };

    // 预编译后的判定器：正则只编译一次；候选文本按"便宜 → 昂贵"排序并逐项短路，
    // 最贵的两条 HEX 串在多数消息上根本不会被构造（热路径性能红线，见设计稿 §6）。
    class Matcher
    {
    public:
        Matcher() = default;
        Matcher(const Options &options, PayloadCodec::Format format);
        bool accepts(const Traffic::Message &record) const;
        // 供摘要与持久化复用；Matcher 持有的是构造时的条件快照。
        const Options &options() const { return m_options; }
    private:
        Options m_options;
        PayloadCodec::Format m_format = PayloadCodec::Format::Hex;
        QList<TextCondition> m_keywords;
        QList<TextCondition> m_excludes;
    };

    // 一次性判定，等价于 Matcher(options, format).accepts(record)：供测试与低频场景使用。
    // 热路径必须自己构造一次 Matcher 反复调用，不要每条消息都走这里。
    static bool accepts(const Traffic::Message &record, const Options &options, PayloadCodec::Format format);
};

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
    // 组合根注入的偏好存储；为空时全部显示选项不落盘（示例预览等场景）。
    void setPreferences(PreferencesStore *store);
signals:
    // 会话累计条数与一次性提示（导出结果、日志写失败）都从这根信号出，
    // 由主窗口放进底部状态栏——面板内部不再为摘要保留一行 UI。
    void summaryChanged(const QString &text);
    // 性能状态：随心跳上报（最近 2 秒的界面迟滞/单批处理耗时 + 缓冲与日志占用）。
    // 主窗口把它挂到状态栏的 tooltip 上——性能属于"要看才发现"的信息，
    // 不值得为它再占一段状态栏宽度，但必须随手可得。
    void performanceChanged(const QString &text);
private:
    void updateFilter();
    // 重投影：把**已应用**的过滤条件与**当前**显示设置重新作用到模型缓冲里的全部记录上。
    //   clearAll → 从模型重放命中项 → 行号从 1 连续重排。
    // 两个维度必须分开（设计稿 §5 决策表）：
    //   · 外观（显示设置/编码格式）= 每行长什么样 → 立刻自动重投影；
    //   · 过滤条件 = 哪些行进来 → 只作用新行，补到历史必须点「应用过滤」。
    // 所以 applyCurrentConditions 决定这次用哪份条件过滤：
    //   true  —— 用**当前**条件（applyMatcher），并把 m_projectedOptions 记为当前条件。
    //            「应用过滤」按钮与 loadExample 走这条：它们的语义就是"让视图跟上当前条件"。
    //   false —— 外观路径专用：用**上次已应用**的条件过滤，不碰 m_projectedOptions，因此
    //            不会把未应用的条件套到历史行上。但它改的是"每行长什么样"，而候选项里含
    //            "按当前显示格式渲染的文本"（见 TrafficFilter::Matcher）——所以只在
    //            DEC / BASE64 / 转义字节等**格式专属渲染**里命中的条件会随编码格式移动命中行
    //            （同一条件在新渲染下的重新求值；此时 m_projectedOptions == m_filterOptions、
    //            「应用过滤」提示保持干净）。
    // announce 与 applyCurrentConditions 是两个独立维度，不要混用：loadExample 只要"不播报"
    // （applyCurrentConditions 仍为 true），外观路径两者都为 false。
    void reprojectView(bool announce = true, bool applyCurrentConditions = true);
    // 重投影的防抖入口。只由**外观**变化调用（显示设置、编码格式）：一次点击一个
    // 状态，用户期待切了就变，没道理再让他多点一次。过滤条件不走这里——它由条件条上
    // 的「应用过滤」按钮显式触发（用户钦定），连续敲字期间不该反复重画几万行。
    void scheduleReproject();
    // 「每行长什么样」（编码格式 + 显示设置）是否与上次投影时不同。
    bool appearanceChanged() const;
    // 记下"这次投影用的是当前外观/条件"。conditionsApplied 区分两条路径能否顺带更新
    // 已应用条件：外观路径传 false（只更新外观签名，m_projectedOptions 不动），
    // 这样未应用的条件在 refreshApplyHint 里仍被判为"待应用"。
    void markReprojected(bool conditionsApplied = true);
    // 条件条上「应用过滤」的待办提示：条件改了但还没补到已有记录上，必须说出来。
    void refreshApplyHint();
    // 过滤条的折叠：收起时只隐藏第二行，条件与记忆状态都不动。
    void setFilterExpanded(bool expanded);
    // 折叠按钮的文字：状态 + 生效条件数，收起时也能一眼看出"当前在过滤"。
    void refreshFilterToggle();
    void updatePortVisibility();
    void updateSummary();
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
    // 把当前实际写入的文件名放进「记录到文件」的 tooltip（未记录时恢复原说明）：
    // 用户需要知道"重开了、这次写到哪个文件"，而不是只看到一个勾。
    void refreshRecordTooltip();
    // 摘要经 summaryChanged 上报；内容不变就不重发，状态栏不做无效刷新。
    void reportSummary(const QString &text);
    // 显示选项持久化：变更即写（QSettings 延迟落盘），析构时兜底再存一次。
    void loadPreferences();
    void savePreferences();
    TrafficModel *m_model = nullptr;
    TrafficTextView *m_text = nullptr; // 唯一显示视图：JCom 式文本框逐行追加
    QLabel *m_empty = nullptr;
    QComboBox *m_format;
    // 过滤入口：顶栏的折叠按钮（原搜索框的位置）+ 紧挨着它的方向/事件开关。
    // 开关常驻顶栏（不进可折叠的第二行）：调频次高于关键词，收起条件条时也要能点。
    QPushButton *m_filterToggle;
    FilterFlags *m_flags;
    FilterBar *m_filterBar;
    // 折叠状态用显式标志而不是 isVisible()：窗口整体隐藏时 isVisible() 恒为假，
    // 记忆状态会跟着被读错。
    bool m_filterExpanded = true;
    // A/B 端点开关直接放在顶栏选项栏里，快捷切换显示范围；其余显示选项在"显示设置"菜单。
    QPushButton *m_portAButton;
    QPushButton *m_portBButton;
    // 行内容开关：全部在到达时生效（控制台语义），纯数据模式下被忽略。
    // 方向与事件开关不在其中——它们管的是"哪些行进来"，已迁到顶栏（见 §7.4）。
    QAction *m_numberAction;    // 行号（会话内绝对计数）
    QAction *m_timestampAction;
    QAction *m_directionAction;
    QAction *m_lengthAction;  // 数据长度 [N B]
    QAction *m_hexAction;     // 同时显示 HEX 对照
    QAction *m_pureAction;    // 纯数据视图：无一切前缀
    QAction *m_wrapAction;    // 自动换行
    QAction *m_recordAction;  // 记录到文件：按当前显示格式持续写日志
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
    bool m_everProjected = false; // 一次投影都没做过时视图是空的，没有"跟不上"这回事
    QTimer *m_reprojectTimer = nullptr;
    PreferencesStore *m_preferences = nullptr;
    bool m_loading = false; // 加载偏好期间联动触发的保存要抑制，否则半成品状态会覆盖已存值
    QFile *m_logFile = nullptr;   // 打开期间持续写入；关闭开关、重开或析构时收口
    // 用户选定的**模板**路径（不含时间戳），随开关状态一起持久化：它只用来定
    // "目录 + 基名 + 扩展名"，实际文件名每次带时间戳另起。存模板而不是实际路径，
    // 是为了避免重启后时间戳层层叠加（traffic-2026...-20261004....txt）。
    QString m_logPath;
    // 本次实际写入的完整路径（带时间戳），仅供 tooltip 显示"正在写哪个文件"。
    QString m_logFilePath;
    // 本次记录会话已写入的字节数：flushPending 写完一批后从 m_logFile->size() 读一次
    // （O(1)，不碰目录查询）。摘要里的"日志 X"与性能 tooltip 都取它；不记录时恒为 0。
    qint64 m_logBytes = 0;
};

#endif
