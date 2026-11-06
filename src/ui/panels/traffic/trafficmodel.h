#ifndef TRAFFICMODEL_H
#define TRAFFICMODEL_H

#include "domain/trafficmessage.h"
#include <QObject>
#include <QUuid>

// 监视历史保存结构化记录，供重绘、过滤和导出复用；不持有控件或传输对象。
class TrafficModel : public QObject
{
    Q_OBJECT
public:
    // 保持现有历史容量策略。字节数统计预览载荷，元数据另由条数上限约束。
    static constexpr qint64 kSafetyByteBudget = 512LL * 1024 * 1024;
    static constexpr int kSafetyRowBudget = 20000000;

    explicit TrafficModel(QObject *parent = nullptr);
    int rowCount() const { return m_records.size(); }
    qint64 rawBytes() const { return m_rawBytes; }
    qint64 byteBudget() const { return m_byteBudget; }
    int rowBudget() const { return m_rowBudget; }
    const Traffic::Message &record(int row) const { return m_records.at(row); }
    QString historyId() const { return m_historyId; }
    quint64 firstSequence() const { return m_firstSequence; }
    quint64 nextSequence() const { return m_nextSequence; }
    void setBudget(qint64 bytes, int rows);
    void replaceRecords(QList<Traffic::Message> records);
    // 返回本批释放的最旧记录数，包含单批本身超限时的释放，供界面提示。
    int appendRecords(QList<Traffic::Message> records);

private:
    int trimToBudget();
    QList<Traffic::Message> m_records;
    qint64 m_rawBytes = 0;
    qint64 m_byteBudget = kSafetyByteBudget;
    int m_rowBudget = kSafetyRowBudget;
    QString m_historyId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    quint64 m_firstSequence = 1;
    quint64 m_nextSequence = 1;
};

#endif
