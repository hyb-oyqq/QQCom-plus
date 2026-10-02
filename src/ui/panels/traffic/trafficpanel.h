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
class QTableView;
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
    // 切换换行模式等全局显示形态时重建视图行高：数据不动，只让行高按当前模式重算。
    void refreshLayout() { beginResetModel(); endResetModel(); }
private:
    // 渲染缓存：按需渲染 DataColumn 文本，减少自动换行时 sizeHint() 的重复开销。
    // 键为行号，值为当前格式的渲染文本；格式切换或模型重置后失效。
    QString renderDataColumn(int row) const;
    void clearRenderCache();
    QList<Traffic::Message> m_records;
    PayloadCodec::Format m_format = PayloadCodec::Format::Hex;
    mutable QHash<int, QString> m_renderCache;  // 缓存容量由 QHash 自动管理
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
    // 真实通信消息的统一入口：走与示例数据完全相同的预算、筛选与显示路径。
    // 暂停期间不进模型，只累计丢弃条数并在恢复后清零（显示暂停不拖垮通信）。
    // 高频到达时先攒批再入模（见 m_flushTimer），插入通知次数与消息速率解耦。
    void appendMessages(const QList<Traffic::Message> &messages);
    // 组合根注入的偏好存储；为空时全部显示选项不落盘（示例预览等场景）。
    void setPreferences(PreferencesStore *store);
signals:
    void reusePayload(const QByteArray &bytes);
private:
    void updateFilter();
    void updatePortVisibility();
    void updateSummary();
    void exportVisible();
    // 把攒批里的消息整批入模并收尾（列宽防抖、滚动到底、计数摘要）。
    void flushPending();
    // 换行查看模式：开启后数据内容列按列宽折行，行高钳制交给委托。
    void applyWrapMode(bool wrap);
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
    // 按实际内容调整固定列宽：端口标签与对端地址的长度和端点类型、地址位数相关，
    // 写死的常量迟早会截断它们。数据内容列始终 Stretch，不参与。
    void fitFixedColumns();
    TrafficModel *m_model;
    TrafficFilter *m_filter;
    QTableView *m_table;
    QComboBox *m_format;
    QLineEdit *m_search;
    // A/B 端点开关直接放在顶栏选项栏里，快捷切换显示范围；其余显示选项在"显示设置"菜单。
    QPushButton *m_portAButton;
    QPushButton *m_portBButton;
    QAction *m_rxAction;
    QAction *m_txAction;
    QAction *m_sampleAction; // 示例数据独立开关，避免与真实收发混看
    QAction *m_wholeAction;  // 只看按边界完整的记录，滤掉字节流片段
    QAction *m_wrapAction;   // 自动换行：数据内容列按列宽折行的查看模式
    QAction *m_recordAction; // 记录到文件：按当前显示格式持续写日志
    QMenu *m_lengthMenu;     // 最小字节数档位；勾选状态即当前过滤条件
    QSet<QUuid> m_visibleEndpoints;
    QPushButton *m_follow;
    QLabel *m_summary;
    QLabel *m_exampleBadge;
    QStackedWidget *m_stack;
    bool m_paused = false;
    quint64 m_droppedWhilePaused = 0;
    QTimer *m_fitDebounce = nullptr; // 高频追加时限流量宽：防抖到节拍执行
    // 攒批缓冲：appendMessages 先入缓冲，16ms 一拍整批入模。
    QList<Traffic::Message> m_pending;
    QTimer *m_flushTimer = nullptr;
    PreferencesStore *m_preferences = nullptr;
    bool m_loading = false; // 加载偏好期间联动触发的保存要抑制，否则半成品状态会覆盖已存值
    QFile *m_logFile = nullptr;   // 打开期间持续追加；关闭开关或析构时收口
    QString m_logPath;            // 上次记录的文件；随开关状态一起持久化
};

#endif
