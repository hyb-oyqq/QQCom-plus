#ifndef TRAFFICPANEL_H
#define TRAFFICPANEL_H

#include "ui/common/payloadcodec.h"
#include "domain/trafficmessage.h"
#include "domain/portdraft.h"
#include <QAbstractTableModel>
#include <QDateTime>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QWidget>

class QAction;
class QActionGroup;
class QComboBox;
class QFile;
class QLabel;
class QLineEdit;
class QMenu;
class PreferencesStore;
class QPushButton;
class QStackedWidget;
class TrafficTextView;
class QTimer;

// 消息是 domain 的值类型（Traffic::Message）：原始字节、时间戳、来源、长度都是事实，
// 显示格式和列开关只是读取它的不同方式。面板不再自带一份精简副本。
class TrafficModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column
    {
        TimeColumn = 0,
        SourceColumn,
        PeerColumn,
        DirectionColumn,
        BytesColumn,
        DataColumn,
        ColumnCount
    };

    explicit TrafficModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void replaceRecords(QList<Traffic::Message> records);
    void appendRecords(QList<Traffic::Message> records);
    // 数据内容列的渲染格式，由 PayloadCodec 与发送区共用同一套规则。
    void setFormat(PayloadCodec::Format format);
    const Traffic::Message &record(int row) const;
private:
    // 渲染缓存：按需渲染 DataColumn 文本，避免同一帧内多处询问时重复渲染。
    // 键为消息 id：会话内唯一，且不随淘汰旧记录导致的行号平移而变化。
    QString renderDataColumn(int row) const;
    void clearRenderCache();
    QList<Traffic::Message> m_records;
    qint64 m_rawBytes = 0; // m_records 的原始字节总数，避免每次追加都全量重算
    PayloadCodec::Format m_format = PayloadCodec::Format::Hex;
    mutable QHash<quint64, QString> m_renderCache;
};

class TrafficFilter : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    // 过滤条件。全部条件都作用在结构化字段上：端点/方向来自消息本身，
    // 关键词则对同一份原始字节做多种视图的匹配，所见即可搜。
    struct Options
    {
        QSet<QUuid> endpoints; // 集合为空表示不显示任何端点的记录
        bool rx = true;
        bool tx = true;
        bool sample = true; // 示例数据可单独关掉，不与真实数据混淆
        bool textOnly = false; // 只看按消息边界完整的记录，滤掉字节流片段
        int minBytes = 0;
        QString search;
    };

    explicit TrafficFilter(QObject *parent = nullptr);
    void setOptions(const Options &options);
    // 搜索同时匹配当前显示格式的渲染文本，切换格式后仍能按所见内容过滤。
    void setDisplayFormat(PayloadCodec::Format format);
    // 过滤判定的唯一实现：代理逐行用它；文本显示在消息到达时也用它，
    // 两处看到的"此刻可见内容"才一致。Options 与格式都由调用方持有。
    static bool accepts(const Traffic::Message &record, const Options &options, PayloadCodec::Format format);
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
private:
    Options m_options;
    PayloadCodec::Format m_format = PayloadCodec::Format::Hex;
};

class TrafficPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TrafficPanel(QWidget *parent = nullptr);
    ~TrafficPanel() override;
    void setPorts(const QList<PortDraft> &ports);
    void loadExample(const QList<PortDraft> &ports);
    // 真实通信消息的统一入口。暂停期间不进显示，只累计丢弃条数并在恢复后清零
    // （显示暂停不拖垮通信）。高频到达时先攒批再入模（见 m_flushTimer），
    // 插入通知次数与消息速率解耦。
    void appendMessages(const QList<Traffic::Message> &messages);
    // 组合根注入的偏好存储；为空时全部显示选项不落盘（示例预览等场景）。
    void setPreferences(PreferencesStore *store);
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
    // 记录跟随显示流：暂停显示即暂停记录；切换编码格式后，新行跟随新格式。
    void toggleLogRecording(bool enabled);
    void writeLog(const QList<Traffic::Message> &messages);
    void closeLogFile();
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
    QAction *m_sampleAction; // 示例数据独立开关，避免与真实收发混看
    QAction *m_wholeAction;  // 只看按边界完整的记录，滤掉字节流片段
    QAction *m_recordAction; // 记录到文件：按当前显示格式持续写日志
    QMenu *m_lengthMenu;     // 最小字节数档位；勾选状态即当前过滤条件
    QSet<QUuid> m_visibleEndpoints;
    QPushButton *m_follow;
    QLabel *m_summary;
    QLabel *m_exampleBadge;
    QStackedWidget *m_stack;
    bool m_paused = false;
    quint64 m_droppedWhilePaused = 0;
    quint64 m_totalAccepted = 0; // 会话累计接收条数（含示例），文本历史随淘汰可超过模型上限
    // 攒批缓冲：appendMessages 先入缓冲，16ms 一拍整批入模。
    QList<Traffic::Message> m_pending;
    QTimer *m_flushTimer = nullptr;
    // 文本视图按到达时的过滤条件收流：updateFilter 是唯一写入点。
    // 文本行按到达时格式固化，没有"重算全部行"这回事。
    TrafficFilter::Options m_textOptions;
    PreferencesStore *m_preferences = nullptr;
    bool m_loading = false; // 加载偏好期间联动触发的保存要抑制，否则半成品状态会覆盖已存值
    QFile *m_logFile = nullptr;   // 打开期间持续追加；关闭开关或析构时收口
    QString m_logPath;            // 上次记录的文件；随开关状态一起持久化
};

#endif
