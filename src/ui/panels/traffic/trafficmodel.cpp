#include "trafficmodel.h"
#include <utility>

namespace {
constexpr qsizetype PreviewLimit = 4096;

void applyPreviewLimit(Traffic::Message &record)
{
    if (record.rawLength <= 0) record.rawLength = record.raw.size();
    if (record.raw.size() > PreviewLimit) {
        // 复制有限预览，避免截短长度后仍保留大载荷的分配容量。
        record.raw = QByteArray(record.raw.constData(), PreviewLimit);
        record.truncated = true;
    } else if (record.rawLength > record.raw.size()) {
        record.truncated = true;
    }
}
}

TrafficModel::TrafficModel(QObject *parent) : QObject(parent) {}

int TrafficModel::trimToBudget()
{
    int drop = 0;
    while (drop < m_records.size()
           && (m_rawBytes > m_byteBudget || m_records.size() - drop > m_rowBudget)) {
        m_rawBytes -= m_records.at(drop).raw.size();
        ++drop;
    }
    if (drop > 0) m_records.remove(0, drop);
    return drop;
}

void TrafficModel::setBudget(qint64 bytes, int rows)
{
    m_byteBudget = qMax<qint64>(1, bytes);
    m_rowBudget = qMax(1, rows);
    trimToBudget();
}

void TrafficModel::replaceRecords(QList<Traffic::Message> records)
{
    // 替换入口供示例预览使用，实时数据走 appendRecords。
    if (records.size() > 2000) records = records.last(2000);
    m_records.clear();
    m_rawBytes = 0;
    appendRecords(std::move(records));
}

int TrafficModel::appendRecords(QList<Traffic::Message> records)
{
    if (records.isEmpty()) return 0;
    for (auto &record : records) {
        applyPreviewLimit(record);
        m_rawBytes += record.raw.size();
    }
    m_records.append(std::move(records));
    return trimToBudget();
}
