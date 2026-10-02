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
class QLabel;
class QLineEdit;
class QMenu;
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
private:
    QList<Traffic::Message> m_records;
    PayloadCodec::Format m_format = PayloadCodec::Format::Hex;
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
    void setPorts(const QList<PortDraft> &ports);
    void loadExample(const QList<PortDraft> &ports);
    // 真实通信消息的统一入口：走与示例数据完全相同的预算、筛选与显示路径。
    // 暂停期间不进模型，只累计丢弃条数并在恢复后清零（显示暂停不拖垮通信）。
    void appendMessages(const QList<Traffic::Message> &messages);
signals:
    void reusePayload(const QByteArray &bytes);
private:
    void updateFilter();
    void updatePortVisibility();
    void updateSummary();
    void exportVisible();
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
    QMenu *m_lengthMenu;     // 最小字节数档位；勾选状态即当前过滤条件
    QSet<QUuid> m_visibleEndpoints;
    QPushButton *m_follow;
    QLabel *m_summary;
    QLabel *m_exampleBadge;
    QStackedWidget *m_stack;
    bool m_paused = false;
    quint64 m_droppedWhilePaused = 0;
    QTimer *m_fitDebounce = nullptr; // 高频追加时限流量宽：防抖到节拍执行
};

#endif
