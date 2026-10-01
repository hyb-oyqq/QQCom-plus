#ifndef TRAFFICPANEL_H
#define TRAFFICPANEL_H

#include "portselectorwidget.h"
#include <QAbstractTableModel>
#include <QDateTime>
#include <QSortFilterProxyModel>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTableView;

struct TrafficRecord
{
    QDateTime time;
    QUuid endpoint;
    QString name;
    bool tx = false;
    QByteArray payload;
};

class TrafficModel : public QAbstractTableModel
{
public:
    explicit TrafficModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void replaceRecords(QList<TrafficRecord> records);
    void setHex(bool hex);
    const TrafficRecord &record(int row) const;
private:
    QList<TrafficRecord> m_records;
    bool m_hex = true;
};

class TrafficFilter : public QSortFilterProxyModel
{
public:
    explicit TrafficFilter(QObject *parent = nullptr);
    void setOptions(const QUuid &endpoint, bool rx, bool tx, const QString &search);
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
private:
    QUuid m_endpoint;
    bool m_rx = true;
    bool m_tx = true;
    QString m_search;
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
    void updateSummary();
    void exportVisible();
    TrafficModel *m_model;
    TrafficFilter *m_filter;
    QTableView *m_table;
    QComboBox *m_source;
    QComboBox *m_format;
    QLineEdit *m_search;
    QCheckBox *m_rx;
    QCheckBox *m_tx;
    QCheckBox *m_follow;
    QLabel *m_summary;
    QLabel *m_exampleBadge;
    QStackedWidget *m_stack;
    bool m_paused = false;
};

#endif
