#ifndef TRAFFICPANEL_H
#define TRAFFICPANEL_H

#include "../../common/payloadcodec.h"
#include "../portselector/portselectorwidget.h"
#include <QAbstractTableModel>
#include <QDateTime>
#include <QSet>
#include <QSortFilterProxyModel>

class QAction;
class QActionGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QStackedWidget;
class QTableView;

struct TrafficRecord
{
    QDateTime time;
    QUuid endpoint;
    // Derived endpoint identity plus peer, e.g. "A · 串口"; there is no user-set name.
    QString portLabel;
    bool tx = false;
    QByteArray payload;
};

class TrafficModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit TrafficModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void replaceRecords(QList<TrafficRecord> records);
    // 数据内容列的渲染格式，由 PayloadCodec 与发送区共用同一套规则。
    void setFormat(PayloadCodec::Format format);
    const TrafficRecord &record(int row) const;
private:
    QList<TrafficRecord> m_records;
    PayloadCodec::Format m_format = PayloadCodec::Format::Hex;
};

class TrafficFilter : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit TrafficFilter(QObject *parent = nullptr);
    // 集合为空表示不显示任何端点的记录；非空则只显示集合内端点。
    void setOptions(const QSet<QUuid> &endpoints, bool rx, bool tx, const QString &search);
    // 搜索同时匹配当前显示格式的渲染文本，切换格式后仍能按所见内容过滤。
    void setDisplayFormat(PayloadCodec::Format format);
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
private:
    QSet<QUuid> m_endpoints;
    bool m_rx = true;
    bool m_tx = true;
    QString m_search;
    PayloadCodec::Format m_format = PayloadCodec::Format::Hex;
};

class TrafficPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TrafficPanel(QWidget *parent = nullptr);
    void setPorts(const QList<PortDraft> &ports);
    void loadExample(const QList<PortDraft> &ports);
signals:
    void reusePayload(const QByteArray &bytes);
private:
    void updateFilter();
    void updatePortVisibility();
    void updateSummary();
    void exportVisible();
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
    QSet<QUuid> m_visibleEndpoints;
    QPushButton *m_follow;
    QLabel *m_summary;
    QLabel *m_exampleBadge;
    QStackedWidget *m_stack;
    bool m_paused = false;
};

#endif
