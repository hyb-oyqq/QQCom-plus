#ifndef TRAFFICPANEL_H
#define TRAFFICPANEL_H

#include "ui/common/payloadcodec.h"
#include "domain/trafficmessage.h"
#include "domain/portdraft.h"
#include "traffictextview.h"
#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QWidget>

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

// 消息是 domain 的值类型（Traffic::Message）：原始字节、时间戳、来源、长度都是事实，
// 面板不再自带一份精简副本。TrafficModel 是结构化存储：导出、记录与搜索的事实来源，
// 不驱动任何绘制（显示由 TrafficTextView 承担）。
class TrafficModel : public QObject
{
    Q_OBJECT
public:
    explicit TrafficModel(QObject *parent = nullptr);
    int rowCount() const;
    void replaceRecords(QList<Traffic::Message> records);
    void appendRecords(QList<Traffic::Message> records);
    const Traffic::Message &record(int row) const;
private:
    QList<Traffic::Message> m_records;
    qint64 m_rawBytes = 0; // m_records 的原始字节总数，避免每次追加都全量重算
};

// 过滤判定的唯一实现：文本显示在消息到达时调用它，Options 与格式都由调用方持有。
// 全部条件都作用在结构化字段上：端点/方向来自消息本身，
// 关键词则对同一份原始字节做多种视图的匹配，所见即可搜。
class TrafficFilter
{
public:
    struct Options
    {
        QSet<QUuid> endpoints; // 集合为空表示不显示任何端点的记录
        bool rx = true;
        bool tx = true;
        bool events = true; // 错误/警告日志（Warning/Error 方向的离散事件）有独立开关
        QString search;
    };

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
    // 组合根注入的偏好存储；为空时全部显示选项不落盘（示例预览等场景）。
    void setPreferences(PreferencesStore *store);
signals:
    // 会话累计条数与一次性提示（导出结果、日志写失败）都从这根信号出，
    // 由主窗口放进底部状态栏——面板内部不再为摘要保留一行 UI。
    void summaryChanged(const QString &text);
private:
    void updateFilter();
    void updatePortVisibility();
    void updateSummary();
    void exportVisible();
    // 把攒批里的消息整批入模并收尾（滚动到底、计数摘要）。
    void flushPending();
    // m_format 下标 → 格式枚举的唯一换算点，格式切换联动与日志落盘共用。
    PayloadCodec::Format currentDisplayFormat() const;
    // 日志记录：把入模的每条消息按当前显示格式写成一行，持续追加到用户选定的文件。
    // 记录跟随显示流：切换编码格式后，新行跟随新格式。
    void toggleLogRecording(bool enabled);
    void writeLog(const QList<Traffic::Message> &messages);
    void closeLogFile();
    // 摘要经 summaryChanged 上报；内容不变就不重发，状态栏不做无效刷新。
    void reportSummary(const QString &text);
    // 显示选项持久化：变更即写（QSettings 延迟落盘），析构时兜底再存一次。
    void loadPreferences();
    void savePreferences();
    TrafficModel *m_model = nullptr;
    TrafficTextView *m_text = nullptr; // 唯一显示视图：JCom 式文本框逐行追加
    QLabel *m_empty = nullptr;
    QComboBox *m_format;
    QLineEdit *m_search;
    // A/B 端点开关直接放在顶栏选项栏里，快捷切换显示范围；其余显示选项在"显示设置"菜单。
    QPushButton *m_portAButton;
    QPushButton *m_portBButton;
    QAction *m_rxAction;
    QAction *m_txAction;
    // 行内容开关：全部在到达时生效（控制台语义），纯数据模式下被忽略。
    QAction *m_timestampAction;
    QAction *m_directionAction;
    QAction *m_lengthAction;  // 数据长度 [N B]
    QAction *m_hexAction;     // 同时显示 HEX 对照
    QAction *m_errorAction;   // 错误/警告日志（过滤开关）
    QAction *m_pureAction;    // 纯数据视图：无一切前缀
    QAction *m_wrapAction;    // 自动换行
    QAction *m_recordAction;  // 记录到文件：按当前显示格式持续写日志
    QSet<QUuid> m_visibleEndpoints;
    QPushButton *m_follow;
    QStackedWidget *m_stack;
    quint64 m_totalAccepted = 0; // 会话累计接收条数（含示例），文本历史随淘汰可超过模型上限
    QString m_lastSummary;       // 上次上报的摘要文本，去重用
    // 攒批缓冲：appendMessages 先入缓冲，16ms 一拍整批入模。
    QList<Traffic::Message> m_pending;
    QTimer *m_flushTimer = nullptr;
    // 文本视图按到达时的过滤条件收流：updateFilter 是唯一写入点。
    // 文本行按到达时格式与开关组合固化，没有"重算全部行"这回事。
    TrafficFilter::Options m_textOptions;
    TrafficTextView::DisplayOptions m_displayOptions;
    PreferencesStore *m_preferences = nullptr;
    bool m_loading = false; // 加载偏好期间联动触发的保存要抑制，否则半成品状态会覆盖已存值
    QFile *m_logFile = nullptr;   // 打开期间持续追加；关闭开关或析构时收口
    QString m_logPath;            // 上次记录的文件；随开关状态一起持久化
};

#endif
